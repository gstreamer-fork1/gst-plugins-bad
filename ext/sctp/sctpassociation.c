/*
 * Copyright (c) 2015, Collabora Ltd.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sctpassociation.h"

#include <string.h>
#include <errno.h>
#include <stdlib.h>

#define GST_SCTP_ASSOCIATION_STATE_TYPE (gst_sctp_association_state_get_type())
static GType
gst_sctp_association_state_get_type (void)
{
  static const GEnumValue values[] = {
    {GST_SCTP_ASSOCIATION_STATE_NEW, "state-new", "state-new"},
    {GST_SCTP_ASSOCIATION_STATE_READY, "state-ready", "state-ready"},
    {GST_SCTP_ASSOCIATION_STATE_CONNECTING, "state-connecting",
        "state-connecting"},
    {GST_SCTP_ASSOCIATION_STATE_CONNECTED, "state-connected",
        "state-connected"},
    {GST_SCTP_ASSOCIATION_STATE_DISCONNECTING, "state-disconnecting",
        "state-disconnecting"},
    {GST_SCTP_ASSOCIATION_STATE_DISCONNECTED, "state-disconnected",
        "state-disconnected"},
    {GST_SCTP_ASSOCIATION_STATE_ERROR, "state-error", "state-error"},
    {0, NULL, NULL}
  };
  static volatile GType id = 0;

  if (g_once_init_enter ((gsize *) & id)) {
    GType _id;
    _id = g_enum_register_static ("GstSctpAssociationState", values);
    g_once_init_leave ((gsize *) & id, _id);
  }

  return id;
}

G_DEFINE_TYPE (GstSctpAssociation, gst_sctp_association, G_TYPE_OBJECT);

enum
{
  SIGNAL_STREAM_RESET,
  SIGNAL_ASSOC_RESTART,
  LAST_SIGNAL
};


enum
{
  PROP_0,

  PROP_ASSOCIATION_ID,
  PROP_LOCAL_PORT,
  PROP_REMOTE_PORT,
  PROP_STATE,
  PROP_USE_SOCK_STREAM,
  PROP_DEBUG_SCTP,
  PROP_AGGRESSIVE_HEARTBEAT,

  NUM_PROPERTIES
};

static guint signals[LAST_SIGNAL] = { 0 };

static GParamSpec *properties[NUM_PROPERTIES];

#define DEFAULT_NUMBER_OF_SCTP_STREAMS 65535
#define DEFAULT_LOCAL_SCTP_PORT 0
#define DEFAULT_REMOTE_SCTP_PORT 0

G_LOCK_DEFINE_STATIC (associations_lock);
static GHashTable *associations_by_id = NULL;
static GHashTable *ids_by_association = NULL;
static guint32 number_of_associations = 0;

/* Interface implementations */
static void gst_sctp_association_dispose (GObject * object);
static void gst_sctp_association_finalize (GObject * object);
static void gst_sctp_association_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_sctp_association_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static struct socket *create_sctp_socket (GstSctpAssociation *
    gst_sctp_association);
static struct sockaddr_conn get_sctp_socket_address (GstSctpAssociation *
    gst_sctp_association, guint16 port);
static gpointer connection_thread_func (GstSctpAssociation * self);
static gboolean client_role_connect (GstSctpAssociation * self);
static int sctp_packet_out (void *addr, void *buffer, size_t length, guint8 tos,
    guint8 set_df);
static int receive_cb (struct socket *sock, union sctp_sockstore addr,
    void *data, size_t datalen, struct sctp_rcvinfo rcv_info, gint flags,
    void *ulp_info);
static void handle_notification (GstSctpAssociation * self,
    const union sctp_notification *notification, size_t length);
static void handle_association_changed (GstSctpAssociation * self,
    const struct sctp_assoc_change *sac);
static void handle_stream_reset_event (GstSctpAssociation * self,
    const struct sctp_stream_reset_event *ssr);
static void handle_message (GstSctpAssociation * self, guint8 * data,
    guint32 datalen, guint16 stream_id, guint32 ppid);

static void maybe_set_state_to_ready (GstSctpAssociation * self);
static void gst_sctp_association_change_state_unlocked (
    GstSctpAssociation * self, GstSctpAssociationState new_state);

static void
gst_sctp_association_class_init (GstSctpAssociationClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->dispose = gst_sctp_association_dispose;
  gobject_class->finalize = gst_sctp_association_finalize;
  gobject_class->set_property = gst_sctp_association_set_property;
  gobject_class->get_property = gst_sctp_association_get_property;

  signals[SIGNAL_STREAM_RESET] =
      g_signal_new ("stream-reset", G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_FIRST, G_STRUCT_OFFSET (GstSctpAssociationClass,
          on_sctp_stream_reset), NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[SIGNAL_ASSOC_RESTART] =
      g_signal_new ("association-restart", G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_FIRST, G_STRUCT_OFFSET (GstSctpAssociationClass,
          on_sctp_association_restart), NULL, NULL, g_cclosure_marshal_generic,
      G_TYPE_NONE, 0);

  properties[PROP_ASSOCIATION_ID] = g_param_spec_uint ("association-id",
      "The SCTP association-id", "The SCTP association-id.", 0, G_MAXUSHORT,
      DEFAULT_LOCAL_SCTP_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_LOCAL_PORT] = g_param_spec_uint ("local-port", "Local SCTP",
      "The local SCTP port for this association", 0, G_MAXUSHORT,
      DEFAULT_LOCAL_SCTP_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_REMOTE_PORT] =
      g_param_spec_uint ("remote-port", "Remote SCTP",
      "The remote SCTP port for this association", 0, G_MAXUSHORT,
      DEFAULT_LOCAL_SCTP_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_STATE] = g_param_spec_enum ("state", "SCTP Association state",
      "The state of the SCTP association", GST_SCTP_ASSOCIATION_STATE_TYPE,
      GST_SCTP_ASSOCIATION_STATE_NEW,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_USE_SOCK_STREAM] =
      g_param_spec_boolean ("use-sock-stream", "Use sock-stream",
      "When set to TRUE, a sequenced, reliable, connection-based connection is used."
      "When TRUE the partial reliability parameters of the channel is ignored.",
      FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_DEBUG_SCTP] =
      g_param_spec_boolean ("debug-sctp", "Debug SCTP stack",
      "When set to TRUE, enable SCTP stack debugging.",
      FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_AGGRESSIVE_HEARTBEAT] =
      g_param_spec_boolean ("aggressive-heartbeat", "Aggressive heartbeat",
      "When set to TRUE, set the heartbeat interval to 10ms and the assoc "
      "rtx max to 1.",
      FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, NUM_PROPERTIES, properties);
}

static void
gst_sctp_association_init (GstSctpAssociation * self)
{
  /* No need to lock mutex here as long as the function is only called from gst_sctp_association_get */
  if (number_of_associations == 0) {
    usrsctp_init (0, sctp_packet_out, g_print);

    /* Explicit Congestion Notification */
    usrsctp_sysctl_set_sctp_ecn_enable (0);

    usrsctp_sysctl_set_sctp_nr_outgoing_streams_default
        (DEFAULT_NUMBER_OF_SCTP_STREAMS);
  }
  number_of_associations++;

  self->local_port = DEFAULT_LOCAL_SCTP_PORT;
  self->remote_port = DEFAULT_REMOTE_SCTP_PORT;
  self->sctp_ass_sock = NULL;

  self->connection_thread = NULL;
  g_mutex_init (&self->association_mutex);
  self->done_connect = FALSE;

  self->state = GST_SCTP_ASSOCIATION_STATE_NEW;

  self->use_sock_stream = FALSE;

  usrsctp_register_address ((void *) self);
}

static void
gst_sctp_association_dispose (GObject * object)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (object);

  G_LOCK (associations_lock);

  g_hash_table_remove (associations_by_id, GUINT_TO_POINTER (self->association_id));
  g_hash_table_remove (ids_by_association, self);

  usrsctp_deregister_address ((void *) self);
  number_of_associations--;
  if (number_of_associations == 0) {
    usrsctp_finish ();
  }
  G_UNLOCK (associations_lock);

  if (self->connection_thread)
    g_thread_join (self->connection_thread);

  if (G_OBJECT_CLASS (gst_sctp_association_parent_class)->dispose)
    G_OBJECT_CLASS (gst_sctp_association_parent_class)->dispose (object);
}

static void
gst_sctp_association_finalize (GObject * object)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (object);

  g_mutex_clear (&self->association_mutex);

  G_OBJECT_CLASS (gst_sctp_association_parent_class)->finalize (object);
}

static void
gst_sctp_association_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (object);

  g_mutex_lock (&self->association_mutex);
  if (self->state != GST_SCTP_ASSOCIATION_STATE_NEW) {
    switch (prop_id) {
      case PROP_LOCAL_PORT:
      case PROP_REMOTE_PORT:
        g_warning ("These properties cannot be set in this state");
        goto error;
    }
  }

  switch (prop_id) {
    case PROP_ASSOCIATION_ID:
      self->association_id = g_value_get_uint (value);
      break;
    case PROP_LOCAL_PORT:
      self->local_port = g_value_get_uint (value);
      break;
    case PROP_REMOTE_PORT:
      self->remote_port = g_value_get_uint (value);
      break;
    case PROP_STATE:
      self->state = g_value_get_enum (value);
      break;
    case PROP_USE_SOCK_STREAM:
      self->use_sock_stream = g_value_get_boolean (value);
      break;
    case PROP_DEBUG_SCTP:
      self->debug_sctp = g_value_get_boolean (value);
      if (self->debug_sctp) {
        usrsctp_sysctl_set_sctp_logging_level (0xffffffff);
        usrsctp_sysctl_set_sctp_debug_on (SCTP_DEBUG_ALL);
      } else {
        usrsctp_sysctl_set_sctp_logging_level (0);
        usrsctp_sysctl_set_sctp_debug_on (0);
      }
      break;
    case PROP_AGGRESSIVE_HEARTBEAT:
      self->aggressive_heartbeat = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
      break;
  }

  g_mutex_unlock (&self->association_mutex);
  if (prop_id == PROP_LOCAL_PORT || prop_id == PROP_REMOTE_PORT)
    maybe_set_state_to_ready (self);

  return;

error:
  g_mutex_unlock (&self->association_mutex);
}

static void
maybe_set_state_to_ready (GstSctpAssociation * self)
{
  g_mutex_lock (&self->association_mutex);
  if ((self->state == GST_SCTP_ASSOCIATION_STATE_NEW) &&
      (self->local_port != 0 && self->remote_port != 0)
      && (self->packet_out_cb != NULL) && (self->packet_received_cb != NULL)) {
    gst_sctp_association_change_state_unlocked (self,
        GST_SCTP_ASSOCIATION_STATE_READY);
  }
  g_mutex_unlock (&self->association_mutex);
}

static void
gst_sctp_association_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (object);

  g_mutex_lock (&self->association_mutex);

  switch (prop_id) {
    case PROP_ASSOCIATION_ID:
      g_value_set_uint (value, self->association_id);
      break;
    case PROP_LOCAL_PORT:
      g_value_set_uint (value, self->local_port);
      break;
    case PROP_REMOTE_PORT:
      g_value_set_uint (value, self->remote_port);
      break;
    case PROP_STATE:
      g_value_set_enum (value, self->state);
      break;
    case PROP_USE_SOCK_STREAM:
      g_value_set_boolean (value, self->use_sock_stream);
      break;
    case PROP_DEBUG_SCTP:
      g_value_set_boolean (value, self->debug_sctp);
      break;
    case PROP_AGGRESSIVE_HEARTBEAT:
      g_value_set_boolean (value, self->aggressive_heartbeat);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
      break;
  }

  g_mutex_unlock (&self->association_mutex);
}

/* Public functions */

GstSctpAssociation *
gst_sctp_association_get (guint32 association_id)
{
  GstSctpAssociation *association;

  G_LOCK (associations_lock);
  if (!associations_by_id) {
    associations_by_id =
        g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
    ids_by_association =
        g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
  }

  association =
      g_hash_table_lookup (associations_by_id,
          GUINT_TO_POINTER (association_id));
  if (!association) {
    association =
        g_object_new (GST_SCTP_TYPE_ASSOCIATION, "association-id",
        association_id, NULL);
    g_hash_table_insert (associations_by_id, GUINT_TO_POINTER (association_id),
        association);
    g_hash_table_insert (ids_by_association, association,
        GUINT_TO_POINTER (association_id));
  } else {
    g_object_ref (association);
  }
  G_UNLOCK (associations_lock);
  return association;
}

gboolean
gst_sctp_association_start (GstSctpAssociation * self)
{
  gchar *thread_name;

  g_mutex_lock (&self->association_mutex);
  if (self->state != GST_SCTP_ASSOCIATION_STATE_READY &&
      self->state != GST_SCTP_ASSOCIATION_STATE_DISCONNECTED) {
    g_warning ("SCTP association is in wrong state and cannot be started");
    goto configure_required;
  }

  if ((self->sctp_ass_sock = create_sctp_socket (self)) == NULL)
    goto error;

  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_CONNECTING);
  g_mutex_unlock (&self->association_mutex);

  thread_name = g_strdup_printf ("connection_thread_%u", self->association_id);
  self->connection_thread = g_thread_new (thread_name,
      (GThreadFunc) connection_thread_func, self);
  g_free (thread_name);

  return TRUE;
error:
  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_ERROR);
configure_required:
  g_mutex_unlock (&self->association_mutex);
  return FALSE;
}

void
gst_sctp_association_set_on_packet_out (GstSctpAssociation * self,
    GstSctpAssociationPacketOutCb packet_out_cb, gpointer user_data)
{
  g_return_if_fail (GST_SCTP_IS_ASSOCIATION (self));

  g_mutex_lock (&self->association_mutex);
  self->packet_out_cb = packet_out_cb;
  self->packet_out_user_data = user_data;
  g_mutex_unlock (&self->association_mutex);

  maybe_set_state_to_ready (self);
}

void
gst_sctp_association_set_on_packet_received (GstSctpAssociation * self,
    GstSctpAssociationPacketReceivedCb packet_received_cb, gpointer user_data)
{
  g_return_if_fail (GST_SCTP_IS_ASSOCIATION (self));

  g_mutex_lock (&self->association_mutex);
  self->packet_received_cb = packet_received_cb;
  self->packet_received_user_data = user_data;
  g_mutex_unlock (&self->association_mutex);

  maybe_set_state_to_ready (self);
}

void
gst_sctp_association_incoming_packet (GstSctpAssociation * self, guint8 * buf,
    guint32 length)
{
  /* Discard any packets received before we've attempted to connect out.
   *
   * This resolves a glare condition where both ends attempt to create
   * an association simultaneously: if we receive the INIT from the remote
   * side before we have fully configured ourselves then we would ordinarily
   * reject it with an ABORT, causing the remote side to give up. Instead,
   * drop anything received before we're ready and rely on our outbound INIT
   * to create the association, instead.
   */
  if (self->done_connect) {
    usrsctp_conninput ((void *) self, (const void *) buf, (size_t) length, 0);
  } else {
    g_info ("Discarding inbound packet before SCTP fully configured.");
  }
}

gboolean
gst_sctp_association_send_data (GstSctpAssociation * self, guint8 * buf,
    guint32 length, guint16 stream_id, guint32 ppid, gboolean ordered,
    GstSctpAssociationPartialReliability pr, guint32 reliability_param)
{
  struct sctp_sendv_spa spa;
  gint32 bytes_sent;
  gboolean result = FALSE;
  struct sockaddr_conn remote_addr;

  g_mutex_lock (&self->association_mutex);
  if (self->state != GST_SCTP_ASSOCIATION_STATE_CONNECTED)
    goto end;

  memset (&spa, 0, sizeof (spa));

  spa.sendv_sndinfo.snd_ppid = g_htonl (ppid);
  spa.sendv_sndinfo.snd_sid = stream_id;
  spa.sendv_sndinfo.snd_flags = ordered ? 0 : SCTP_UNORDERED;
  spa.sendv_sndinfo.snd_context = 0;
  spa.sendv_sndinfo.snd_assoc_id = 0;
  spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
  if (pr != GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_NONE) {
    spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
    spa.sendv_prinfo.pr_value = g_htonl (reliability_param);
    if (pr == GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_TTL)
      spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_TTL;
    else if (pr == GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_RTX)
      spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_RTX;
    else if (pr == GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_BUF)
      spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_BUF;
  }

  remote_addr = get_sctp_socket_address (self, self->remote_port);
  bytes_sent =
      usrsctp_sendv (self->sctp_ass_sock, buf, length,
      (struct sockaddr *) &remote_addr, 1, (void *) &spa,
      (socklen_t) sizeof (struct sctp_sendv_spa), SCTP_SENDV_SPA, 0);
  if (bytes_sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      /* Resending this buffer is taken care of by the gstsctpenc */
      goto end;
    } else {
      g_info ("Error sending data on stream %u: (%u) %s", stream_id, errno,
          strerror (errno));
      goto end;
    }
  }

  result = TRUE;
end:
  g_mutex_unlock (&self->association_mutex);
  return result;
}


void
gst_sctp_association_reset_stream (GstSctpAssociation * self, guint16 stream_id)
{
  struct sctp_reset_streams *srs;
  socklen_t length;

  length = (socklen_t) (sizeof (struct sctp_reset_streams) + sizeof (guint16));
  srs = (struct sctp_reset_streams *) g_malloc0 (length);
  srs->srs_flags = SCTP_STREAM_RESET_OUTGOING;
  srs->srs_number_streams = 1;
  srs->srs_stream_list[0] = stream_id;

  g_mutex_lock (&self->association_mutex);
  srs->srs_assoc_id = self->sctp_assoc_id;
  if (self->state == GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    if (usrsctp_setsockopt (self->sctp_ass_sock, IPPROTO_SCTP,
        SCTP_RESET_STREAMS, srs, length) < 0) {
      g_info ("Resetting stream id=%u failed", stream_id);
    }
  }
  g_mutex_unlock (&self->association_mutex);

  g_free (srs);
}

static void
gst_sctp_association_force_close_unlocked (GstSctpAssociation * self)
{
  if (self->sctp_ass_sock) {
    usrsctp_close (self->sctp_ass_sock);
    self->sctp_ass_sock = NULL;

  }
  self->done_connect = FALSE;
  self->sctp_assoc_id = 0;
}

void
gst_sctp_association_force_close (GstSctpAssociation * self)
{
  g_mutex_lock (&self->association_mutex);
  gst_sctp_association_force_close_unlocked (self);
  g_mutex_unlock (&self->association_mutex);
}

static void
gst_sctp_association_disconnect_unlocked (GstSctpAssociation * self,
    gboolean try_shutdown)
{
  if (self->state == GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    gst_sctp_association_change_state_unlocked (self,
        GST_SCTP_ASSOCIATION_STATE_DISCONNECTING);

    if (try_shutdown && self->use_sock_stream && self->sctp_ass_sock) {
      g_info ("SCTP association shutting down");
      self->shutdown = FALSE;
      if (usrsctp_shutdown (self->sctp_ass_sock, SHUT_RDWR) == 0) {
        /* wait for shutdown to complete */
        guint cs_to_wait = 100; /* 1s */
        while (!self->shutdown && cs_to_wait > 0) {
          g_usleep (G_USEC_PER_SEC / 100);
          cs_to_wait--;
        }
        self->shutdown = FALSE;
      }
    }
  }

  /* Fall through to ensure the transition to disconnected occurs */

  if (self->state == GST_SCTP_ASSOCIATION_STATE_DISCONNECTING) {
    if (self->connection_thread) {
      /* Release lock while waiting for connection thread to exit */
      g_mutex_unlock (&self->association_mutex);
      g_thread_join (self->connection_thread);
      g_mutex_lock (&self->association_mutex);
      self->connection_thread = NULL;
    }
    gst_sctp_association_force_close_unlocked (self);

    gst_sctp_association_change_state_unlocked (self,
        GST_SCTP_ASSOCIATION_STATE_DISCONNECTED);
    g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO,
        "SCTP association disconnected!");
  }
}

void
gst_sctp_association_disconnect (GstSctpAssociation * self)
{
  g_mutex_lock (&self->association_mutex);
  gst_sctp_association_disconnect_unlocked (self, TRUE);
  g_mutex_unlock (&self->association_mutex);
}

static struct socket *
create_sctp_socket (GstSctpAssociation * self)
{
  struct socket *sock;
  struct linger l;
  struct sctp_event event;
  struct sctp_assoc_value stream_reset;
  int value = 1;
  guint16 event_types[] = {
    SCTP_ASSOC_CHANGE,
    SCTP_PEER_ADDR_CHANGE,
    SCTP_REMOTE_ERROR,
    SCTP_SEND_FAILED,
    SCTP_SHUTDOWN_EVENT,
    SCTP_ADAPTATION_INDICATION,
    /*SCTP_PARTIAL_DELIVERY_EVENT, */
    /*SCTP_AUTHENTICATION_EVENT, */
    SCTP_STREAM_RESET_EVENT,
    /*SCTP_SENDER_DRY_EVENT, */
    /*SCTP_NOTIFICATIONS_STOPPED_EVENT, */
    /*SCTP_ASSOC_RESET_EVENT, */
    SCTP_STREAM_CHANGE_EVENT
  };
  guint32 i;
  guint sock_type = self->use_sock_stream ? SOCK_STREAM : SOCK_SEQPACKET;

  if ((sock =
          usrsctp_socket (AF_CONN, sock_type, IPPROTO_SCTP, receive_cb, NULL, 0,
              (void *) self)) == NULL)
    goto error;

  if (usrsctp_set_non_blocking (sock, 1) < 0) {
    g_warning ("Could not set non-blocking mode on SCTP socket");
    goto error;
  }

  memset (&l, 0, sizeof (l));
  l.l_onoff = 1;
  l.l_linger = 0;
  if (usrsctp_setsockopt (sock, SOL_SOCKET, SO_LINGER, (const void *) &l,
          (socklen_t) sizeof (struct linger)) < 0) {
    g_warning ("Could not set SO_LINGER on SCTP socket");
    goto error;
  }

  if (usrsctp_setsockopt (sock, IPPROTO_SCTP, SCTP_NODELAY, &value,
          sizeof (int))) {
    g_warning ("Could not set SCTP_NODELAY");
    goto error;
  }

  memset (&stream_reset, 0, sizeof (stream_reset));
  stream_reset.assoc_id = SCTP_ALL_ASSOC;
  stream_reset.assoc_value = 1;
  if (usrsctp_setsockopt (sock, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET,
          &stream_reset, sizeof (stream_reset))) {
    g_warning ("Could not set SCTP_ENABLE_STREAM_RESET");
    goto error;
  }

  memset (&event, 0, sizeof (event));
  event.se_assoc_id = SCTP_ALL_ASSOC;
  event.se_on = 1;
  for (i = 0; i < sizeof (event_types) / sizeof (event_types[0]); i++) {
    event.se_type = event_types[i];
    if (usrsctp_setsockopt (sock, IPPROTO_SCTP, SCTP_EVENT,
            &event, sizeof (event)) < 0) {
      g_warning ("Failed to register event %u", event_types[i]);
    }
  }

  return sock;
error:
  if (sock) {
    usrsctp_close (sock);
    g_warning ("Could not create socket. Error: (%u) %s", errno,
        strerror (errno));
    errno = 0;
    sock = NULL;
  }
  return NULL;
}

static struct sockaddr_conn
get_sctp_socket_address (GstSctpAssociation * gst_sctp_association,
    guint16 port)
{
  struct sockaddr_conn addr;

  memset ((void *) &addr, 0, sizeof (struct sockaddr_conn));
#ifdef __APPLE__
  addr.sconn_len = sizeof (struct sockaddr_conn);
#endif
  addr.sconn_family = AF_CONN;
  addr.sconn_port = g_htons (port);
  addr.sconn_addr = (void *) gst_sctp_association;

  return addr;
}

static gpointer
connection_thread_func (GstSctpAssociation * self)
{
  /* TODO: Support both server and client role */
  client_role_connect (self);
  return NULL;
}

static gboolean
client_role_connect (GstSctpAssociation * self)
{
  struct sockaddr_conn addr;
  gint ret;

  g_mutex_lock (&self->association_mutex);
  addr = get_sctp_socket_address (self, self->local_port);

  /* After an SCTP association is reported as disconnected, there is
   * a window of time before the underlying SCTP stack cleans up.
   * If a client-initiated reconnect request occurs during this window
   * then we will attempt to bind using the same address information
   * which will fail with EADDRINUSE. Handle this by retrying whenever
   * a bind fails in this way.
   */
  do {
    ret =
        usrsctp_bind (self->sctp_ass_sock, (struct sockaddr *) &addr,
        sizeof (struct sockaddr_conn));
    if (ret < 0) {
      if (errno != EADDRINUSE) {
        g_info ("usrsctp_bind() error: (%u) %s", errno, strerror (errno));
        goto error;
      }
      g_mutex_unlock (&self->association_mutex);
      g_usleep (G_USEC_PER_SEC / 100);
      g_mutex_lock (&self->association_mutex);
    }
  } while (ret < 0);

  addr = get_sctp_socket_address (self, self->remote_port);
  ret =
      usrsctp_connect (self->sctp_ass_sock, (struct sockaddr *) &addr,
      sizeof (struct sockaddr_conn));
  if (ret < 0 && errno != EINPROGRESS) {
    g_info ("usrsctp_connect() error: (%u) %s", errno, strerror (errno));
    goto error;
  }
  self->done_connect = TRUE;
  g_mutex_unlock (&self->association_mutex);
  return TRUE;
error:
  g_mutex_unlock (&self->association_mutex);
  return FALSE;
}

static gboolean
association_is_valid (GstSctpAssociation * self)
{
  gboolean valid = FALSE;

  G_LOCK (associations_lock);

  valid = g_hash_table_contains (ids_by_association, self);

  G_UNLOCK (associations_lock);

  return valid;
}

static int
sctp_packet_out (void *addr, void *buffer, size_t length, guint8 tos,
    guint8 set_df)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (addr);

  if (association_is_valid (self) && self->packet_out_cb) {
    self->packet_out_cb (self, buffer, length, self->packet_out_user_data);
  }

  return 0;
}

static int
receive_cb (struct socket *sock, union sctp_sockstore addr, void *data,
    size_t datalen, struct sctp_rcvinfo rcv_info, gint flags, void *ulp_info)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (ulp_info);

  if (!association_is_valid (self)) {
    return 1;
  }

  if (!data) {
    /* This is a notification that socket shutdown is complete */
    g_info ("Received shutdown complete notification");
    self->shutdown = TRUE;
  } else {
    if (flags & MSG_NOTIFICATION) {
      handle_notification (self, (const union sctp_notification *) data,
          datalen);
      /* We use this instead of a bare `free()` so that we use the `free` from
       * the C runtime that usrsctp was built with. This makes a difference on
       * Windows where libusrstcp and GStreamer can be linked to two different
       * CRTs. */
      usrsctp_freedumpbuffer (data);
    } else {
      handle_message (self, data, datalen, rcv_info.rcv_sid,
          ntohl (rcv_info.rcv_ppid));
    }
  }

  return 1;
}

static void
handle_notification (GstSctpAssociation * self,
    const union sctp_notification *notification, size_t length)
{
  g_assert (notification->sn_header.sn_length == length);

  switch (notification->sn_header.sn_type) {
    case SCTP_ASSOC_CHANGE:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "Event: SCTP_ASSOC_CHANGE");
      handle_association_changed (self, &notification->sn_assoc_change);
      break;
    case SCTP_PEER_ADDR_CHANGE:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "Event: SCTP_PEER_ADDR_CHANGE");
      break;
    case SCTP_REMOTE_ERROR:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "Event: SCTP_REMOTE_ERROR");
      break;
    case SCTP_SEND_FAILED:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "Event: SCTP_SEND_FAILED");
      break;
    case SCTP_SHUTDOWN_EVENT:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "Event: SCTP_SHUTDOWN_EVENT");
      break;
    case SCTP_ADAPTATION_INDICATION:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO,
          "Event: SCTP_ADAPTATION_INDICATION");
      break;
    case SCTP_PARTIAL_DELIVERY_EVENT:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO,
          "Event: SCTP_PARTIAL_DELIVERY_EVENT");
      break;
    case SCTP_AUTHENTICATION_EVENT:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO,
          "Event: SCTP_AUTHENTICATION_EVENT");
      break;
    case SCTP_STREAM_RESET_EVENT:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "Event: SCTP_STREAM_RESET_EVENT");
      handle_stream_reset_event (self, &notification->sn_strreset_event);
      break;
    case SCTP_SENDER_DRY_EVENT:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "Event: SCTP_SENDER_DRY_EVENT");
      break;
    case SCTP_NOTIFICATIONS_STOPPED_EVENT:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO,
          "Event: SCTP_NOTIFICATIONS_STOPPED_EVENT");
      break;
    case SCTP_ASSOC_RESET_EVENT:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "Event: SCTP_ASSOC_RESET_EVENT");
      break;
    case SCTP_STREAM_CHANGE_EVENT:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "Event: SCTP_STREAM_CHANGE_EVENT");
      break;
    case SCTP_SEND_FAILED_EVENT:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "Event: SCTP_SEND_FAILED_EVENT");
      break;
    default:
      break;
  }
}

static void
_apply_aggressive_heartbeat_unlocked (GstSctpAssociation * self)
{
  struct sctp_assocparams assoc_params;
  struct sctp_paddrparams peer_addr_params;
  struct sockaddr_conn addr;

  if (!self->aggressive_heartbeat)
    return;

  memset (&assoc_params, 0, sizeof (assoc_params));
  assoc_params.sasoc_assoc_id = self->sctp_assoc_id;
  assoc_params.sasoc_asocmaxrxt = 1;
  if (usrsctp_setsockopt (self->sctp_ass_sock, IPPROTO_SCTP,
      SCTP_ASSOCINFO, &assoc_params, sizeof (assoc_params))) {
    g_warning ("Could not set SCTP_ASSOCINFO");
  }

  addr = get_sctp_socket_address (self, self->remote_port);
  memset (&peer_addr_params, 0, sizeof (peer_addr_params));
  memcpy (&peer_addr_params.spp_address, &addr, sizeof (addr));
  peer_addr_params.spp_flags = SPP_HB_ENABLE;
  peer_addr_params.spp_hbinterval = 10;
  if (usrsctp_setsockopt (self->sctp_ass_sock, IPPROTO_SCTP,
      SCTP_PEER_ADDR_PARAMS, &peer_addr_params, sizeof (peer_addr_params))) {
    g_warning ("Could not set SCTP_PEER_ADDR_PARAMS");
  }
}

static void
handle_sctp_comm_up (GstSctpAssociation * self,
    const struct sctp_assoc_change * sac)
{
  g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "SCTP_COMM_UP()");
  g_mutex_lock (&self->association_mutex);
  if (self->state == GST_SCTP_ASSOCIATION_STATE_CONNECTING) {
    self->sctp_assoc_id = sac->sac_assoc_id;
    _apply_aggressive_heartbeat_unlocked (self);
    gst_sctp_association_change_state_unlocked (self,
        GST_SCTP_ASSOCIATION_STATE_CONNECTED);
    g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "SCTP association connected!");
  } else if (self->state == GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    g_info ("SCTP association already open");
  } else {
    g_info ("SCTP association in unexpected state");
  }
  g_mutex_unlock (&self->association_mutex);
}

static void
handle_sctp_comm_lost_or_shutdown (GstSctpAssociation * self,
    const struct sctp_assoc_change * sac)
{
  g_info ("SCTP event %s received",
      sac->sac_state == SCTP_COMM_LOST ?
      "SCTP_COMM_LOST" : "SCTP_SHUTDOWN_COMP");

  g_mutex_lock (&self->association_mutex);
  gst_sctp_association_disconnect_unlocked (self, FALSE);
  g_mutex_unlock (&self->association_mutex);
}

static void
handle_association_changed (GstSctpAssociation * self,
    const struct sctp_assoc_change *sac)
{
  switch (sac->sac_state) {
    case SCTP_COMM_UP:
      handle_sctp_comm_up (self, sac);
      break;
    case SCTP_COMM_LOST:
      handle_sctp_comm_lost_or_shutdown (self, sac);
      break;
    case SCTP_RESTART:
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO,
          "SCTP event SCTP_RESTART received");
      g_signal_emit (self, signals[SIGNAL_ASSOC_RESTART], 0);
      break;
    case SCTP_SHUTDOWN_COMP:
      /* Occurs if in TCP mode when the far end sends SHUTDOWN */
      handle_sctp_comm_lost_or_shutdown (self, sac);
      break;
    case SCTP_CANT_STR_ASSOC:
      g_info ("SCTP event SCTP_CANT_STR_ASSOC received");
      break;
  }
}

static void
handle_stream_reset_event (GstSctpAssociation * self,
    const struct sctp_stream_reset_event *sr)
{
  guint32 i, n;
  if (!(sr->strreset_flags & SCTP_STREAM_RESET_DENIED) &&
      !(sr->strreset_flags & SCTP_STREAM_RESET_DENIED)) {
    n = (sr->strreset_length -
        sizeof (struct sctp_stream_reset_event)) / sizeof (uint16_t);
    for (i = 0; i < n; i++) {
      if (sr->strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN) {
        g_signal_emit (self, signals[SIGNAL_STREAM_RESET], 0,
            sr->strreset_stream_list[i]);
      }
    }
  }
}

static void
handle_message (GstSctpAssociation * self, guint8 * data, guint32 datalen,
    guint16 stream_id, guint32 ppid)
{
  if (self->packet_received_cb) {
    self->packet_received_cb (self, data, datalen, stream_id, ppid,
        self->packet_received_user_data);
  }
}

static void
gst_sctp_association_change_state_unlocked (GstSctpAssociation * self,
    GstSctpAssociationState new_state)
{
  /* Association mutex is held on entry */
  self->state = new_state;
  /* Unlock the mutex to emit the property change event to avoid deadlock
   * if the client calls back into this object from its event handler. */
  g_mutex_unlock (&self->association_mutex);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATE]);
  g_mutex_lock (&self->association_mutex);
}
