/*
 * (C) Copyright 2014 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/pbutils/encoding-profile.h>
#include <commons/kmsrecordingprofile.h>
#include <commons/kms-core-enumtypes.h>
#include <commons/kms-core-marshal.h>
#include <commons/kmsagnosticcaps.h>
#include <commons/kmsutils.h>

#include "kmshttpgetendpoint.h"
#include "kmsmuxingpipeline.h"

#define PLUGIN_NAME "httpgetendpoint"
#define parent_class kms_http_get_endpoint_parent_class

#define AUDIO_APPSINK "audio_appsink"
#define AUDIO_APPSRC "audio_appsrc"
#define VIDEO_APPSINK "video_appsink"
#define VIDEO_APPSRC "video_appsrc"

#define BASE_TIME_DATA "base_time_data"

#define KMS_HTTP_GET_ENDPOINT_PIPELINE "get-pipeline"

GST_DEBUG_CATEGORY_STATIC (kms_http_get_endpoint_debug_category);
#define GST_CAT_DEFAULT kms_http_get_endpoint_debug_category

#define KMS_HTTP_GET_ENDPOINT_GET_PRIVATE(obj) (  \
  G_TYPE_INSTANCE_GET_PRIVATE (                   \
    (obj),                                        \
    KMS_TYPE_HTTP_GET_ENDPOINT,                   \
    KmsHttpGetEndpointPrivate                     \
  )                                               \
)

static void
kms_http_get_endpoint_add_sinkpad (KmsHttpGetEndpoint * self,
    const gchar * name, KmsElementPadType type);

struct _KmsHttpGetEndpointPrivate
{
  GstElement *appsink;
  KmsRecordingProfile profile;
  KmsMuxingPipeline *mux;
  gboolean use_dvr;

  GstElement *videoappsink;
  GstElement *audioappsink;
};

typedef struct _BaseTimeType
{
  GstClockTime pts;
  GstClockTime dts;
} BaseTimeType;

/* Object properties */
enum
{
  PROP_0,
  PROP_DVR,
  PROP_PROFILE,
  N_PROPERTIES
};

#define HTTP_GET_ENDPOINT_RECORDING_PROFILE_DEFAULT KMS_RECORDING_PROFILE_NONE

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

/* Object signals */
enum
{
  /* signals */
  SIGNAL_NEW_SAMPLE,

  /* actions */
  SIGNAL_PULL_SAMPLE,
  LAST_SIGNAL
};

static guint http_get_ep_signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE_WITH_CODE (KmsHttpGetEndpoint, kms_http_get_endpoint,
    KMS_TYPE_HTTP_ENDPOINT,
    GST_DEBUG_CATEGORY_INIT (kms_http_get_endpoint_debug_category, PLUGIN_NAME,
        0, "debug category for http get endpoint plugin"));

static void
release_base_time_type (gpointer data)
{
  g_slice_free (BaseTimeType, data);
}

static void
kms_http_get_endpoint_change_internal_pipeline_state (KmsHttpEndpoint * httpep,
    gboolean start)
{
  KmsHttpGetEndpoint *self = KMS_HTTP_GET_ENDPOINT (httpep);

  if (self->priv->mux == NULL) {
    GST_WARNING_OBJECT (self, "Not initialized");
    goto end;
  }

  if (start) {
    /* Set pipeline to PLAYING */
    GST_DEBUG ("Setting pipeline to PLAYING");
    if (kms_muxing_pipeline_set_state (self->priv->mux,
            GST_STATE_PLAYING) == GST_STATE_CHANGE_ASYNC) {
      GST_DEBUG ("Change to PLAYING will be asynchronous");
    }

    if (kms_recording_profile_supports_type (self->priv->profile,
            KMS_ELEMENT_PAD_TYPE_AUDIO)) {
      kms_http_get_endpoint_add_sinkpad (self, AUDIO_APPSINK,
          KMS_ELEMENT_PAD_TYPE_AUDIO);
    }

    if (kms_recording_profile_supports_type (self->priv->profile,
            KMS_ELEMENT_PAD_TYPE_VIDEO)) {
      kms_http_get_endpoint_add_sinkpad (self, VIDEO_APPSINK,
          KMS_ELEMENT_PAD_TYPE_VIDEO);
    }
  } else {
    kms_element_remove_sink_by_type (KMS_ELEMENT (self),
        KMS_ELEMENT_PAD_TYPE_AUDIO);
    kms_element_remove_sink_by_type (KMS_ELEMENT (self),
        KMS_ELEMENT_PAD_TYPE_VIDEO);
    /* Set pipeline to READY */
    GST_DEBUG ("Setting pipeline to READY.");
    if (kms_muxing_pipeline_set_state (self->priv->mux,
            GST_STATE_READY) == GST_STATE_CHANGE_ASYNC) {
      GST_DEBUG ("Change to READY will be asynchronous");
    }
    // Reset base time data
    BASE_TIME_LOCK (self);
    g_object_set_data_full (G_OBJECT (self), BASE_TIME_DATA, NULL, NULL);
    BASE_TIME_UNLOCK (self);
  }

end:
  httpep->start = start;
}

static GstFlowReturn
new_sample_get_handler (GstElement * appsink, gpointer user_data)
{
  GstElement *appsrc = GST_ELEMENT (user_data);
  GstFlowReturn ret;
  GstSample *sample = NULL;
  GstBuffer *buffer;
  BaseTimeType *base_time;
  KmsHttpGetEndpoint *self =
      KMS_HTTP_GET_ENDPOINT (GST_OBJECT_PARENT (appsink));

  g_signal_emit_by_name (appsink, "pull-sample", &sample);
  if (sample == NULL)
    return GST_FLOW_OK;

  buffer = gst_sample_get_buffer (sample);
  if (buffer == NULL) {
    ret = GST_FLOW_OK;
    goto end;
  }

  gst_buffer_ref (buffer);
  buffer = gst_buffer_make_writable (buffer);

  BASE_TIME_LOCK (self);

  base_time = g_object_get_data (G_OBJECT (self), BASE_TIME_DATA);

  if (base_time == NULL) {
    base_time = g_slice_new0 (BaseTimeType);
    base_time->pts = buffer->pts;
    base_time->dts = GST_CLOCK_TIME_NONE;
    GST_DEBUG_OBJECT (appsrc, "Setting pts base time to: %" G_GUINT64_FORMAT,
        base_time->pts);
    g_object_set_data_full (G_OBJECT (self), BASE_TIME_DATA, base_time,
        release_base_time_type);
  }

  if (!GST_CLOCK_TIME_IS_VALID (base_time->pts)
      && GST_BUFFER_PTS_IS_VALID (buffer)) {
    base_time->pts = buffer->pts;
    GST_DEBUG_OBJECT (appsrc, "Setting pts base time to: %" G_GUINT64_FORMAT,
        base_time->pts);
    base_time->dts = GST_CLOCK_TIME_NONE;
  }

  if (GST_CLOCK_TIME_IS_VALID (base_time->pts)) {
    if (GST_BUFFER_PTS_IS_VALID (buffer)) {
      if (base_time->pts > buffer->pts) {
        buffer->pts = G_GUINT64_CONSTANT (0);
      } else {
        buffer->pts -= base_time->pts;
      }
    }
  } else {
    buffer->pts = G_GUINT64_CONSTANT (0);
  }

  buffer->dts = buffer->pts;

  BASE_TIME_UNLOCK (GST_OBJECT_PARENT (appsink));

  GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_LIVE);

  if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_HEADER))
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);

  /* Pass the buffer through appsrc element which is */
  /* placed in a different pipeline */
  g_signal_emit_by_name (appsrc, "push-buffer", buffer, &ret);

  gst_buffer_unref (buffer);

  if (ret != GST_FLOW_OK) {
    /* something went wrong */
    GST_ERROR ("Could not send buffer to appsrc %s. Cause %s",
        GST_ELEMENT_NAME (appsrc), gst_flow_get_name (ret));
  }

end:
  if (sample != NULL)
    gst_sample_unref (sample);

  return ret;
}

static void
eos_handler (GstElement * appsink, gpointer user_data)
{
  if (KMS_IS_HTTP_ENDPOINT (user_data)) {
    KmsHttpEndpoint *httep = KMS_HTTP_ENDPOINT (user_data);

    GST_DEBUG ("EOS detected on %s", GST_ELEMENT_NAME (httep));
    g_signal_emit_by_name (httep, "eos", 0);
  } else {
    GstElement *appsrc = GST_ELEMENT (user_data);
    GstFlowReturn ret;

    GST_DEBUG ("EOS detected on %s", GST_ELEMENT_NAME (appsink));
    g_signal_emit_by_name (appsrc, "end-of-stream", &ret);
    if (ret != GST_FLOW_OK)
      GST_ERROR ("Could not send EOS to %s", GST_ELEMENT_NAME (appsrc));
  }
}

static GstFlowReturn
new_sample_emit_signal_handler (GstElement * appsink, gpointer user_data)
{
  KmsHttpGetEndpoint *self = KMS_HTTP_GET_ENDPOINT (user_data);
  GstFlowReturn ret;

  g_signal_emit (G_OBJECT (self), http_get_ep_signals[SIGNAL_NEW_SAMPLE], 0,
      &ret);

  return ret;
}

static GstPadProbeReturn
set_audio_caps (GstPad * pad, GstPadProbeInfo * info, gpointer httpep)
{
  KmsHttpGetEndpoint *self = KMS_HTTP_GET_ENDPOINT (httpep);
  GstEvent *event = gst_pad_probe_info_get_event (info);
  GstElement *audioappsrc;
  GstCaps *caps;

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;

  g_object_get (self->priv->mux, KMS_MUXING_PIPELINE_AUDIO_APPSRC,
      &audioappsrc, NULL);

  gst_event_parse_caps (event, &caps);

  GST_INFO_OBJECT (audioappsrc, "Setting caps to: %" GST_PTR_FORMAT, caps);

  g_object_set (audioappsrc, "caps", caps, NULL);

  g_object_unref (audioappsrc);

  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
set_video_caps (GstPad * pad, GstPadProbeInfo * info, gpointer httpep)
{
  KmsHttpGetEndpoint *self = KMS_HTTP_GET_ENDPOINT (httpep);
  GstEvent *event = gst_pad_probe_info_get_event (info);
  GstElement *videoappsrc;
  GstCaps *caps;

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;

  g_object_get (self->priv->mux, KMS_MUXING_PIPELINE_VIDEO_APPSRC,
      &videoappsrc, NULL);

  gst_event_parse_caps (event, &caps);

  GST_INFO_OBJECT (videoappsrc, "Setting caps to: %" GST_PTR_FORMAT, caps);

  g_object_set (videoappsrc, "caps", caps, NULL);

  g_object_unref (videoappsrc);

  return GST_PAD_PROBE_OK;
}

static GstCaps *
kms_http_get_endpoint_get_caps_from_profile (KmsHttpGetEndpoint * self,
    KmsElementPadType type)
{
  GstEncodingContainerProfile *cprof;
  const GList *profiles, *l;
  GstCaps *caps = NULL;

  switch (type) {
    case KMS_ELEMENT_PAD_TYPE_VIDEO:
      cprof =
          kms_recording_profile_create_profile (self->priv->profile, FALSE,
          TRUE);
      break;
    case KMS_ELEMENT_PAD_TYPE_AUDIO:
      cprof =
          kms_recording_profile_create_profile (self->priv->profile, TRUE,
          FALSE);
      break;
    default:
      return NULL;
  }

  profiles = gst_encoding_container_profile_get_profiles (cprof);

  for (l = profiles; l != NULL; l = l->next) {
    GstEncodingProfile *prof = l->data;

    if ((GST_IS_ENCODING_AUDIO_PROFILE (prof) &&
            type == KMS_ELEMENT_PAD_TYPE_AUDIO) ||
        (GST_IS_ENCODING_VIDEO_PROFILE (prof) &&
            type == KMS_ELEMENT_PAD_TYPE_VIDEO)) {
      caps = gst_encoding_profile_get_input_caps (prof);
      break;
    }
  }
  gst_encoding_profile_unref (cprof);
  return caps;
}

static GstCaps *
kms_http_get_endpoint_allowed_caps (KmsElement * self, KmsElementPadType type)
{
  GstElement *appsink;
  GstPad *pad;
  GstCaps *caps;

  switch (type) {
    case KMS_ELEMENT_PAD_TYPE_VIDEO:
      appsink = KMS_HTTP_GET_ENDPOINT (self)->priv->videoappsink;
      break;
    case KMS_ELEMENT_PAD_TYPE_AUDIO:
      appsink = KMS_HTTP_GET_ENDPOINT (self)->priv->audioappsink;
      break;
    default:
      return NULL;
  }

  pad = gst_element_get_static_pad (appsink, "sink");
  caps = gst_pad_get_allowed_caps (pad);
  gst_object_unref (pad);

  return caps;
}

static gboolean
kms_http_get_endpoint_query_caps (KmsElement * element, GstPad * pad,
    GstQuery * query)
{
  KmsHttpGetEndpoint *self = KMS_HTTP_GET_ENDPOINT (element);
  GstCaps *allowed = NULL, *caps = NULL;
  GstCaps *filter, *result, *tcaps;
  GstElement *appsrc;

  gst_query_parse_caps (query, &filter);

  switch (kms_element_get_pad_type (element, pad)) {
    case KMS_ELEMENT_PAD_TYPE_VIDEO:
      g_object_get (self->priv->mux, KMS_MUXING_PIPELINE_VIDEO_APPSRC,
          &appsrc, NULL);
      allowed =
          kms_http_get_endpoint_allowed_caps (element,
          KMS_ELEMENT_PAD_TYPE_VIDEO);
      caps =
          kms_http_get_endpoint_get_caps_from_profile (self,
          KMS_ELEMENT_PAD_TYPE_VIDEO);
      result = gst_caps_from_string (KMS_AGNOSTIC_VIDEO_CAPS);
      break;
    case KMS_ELEMENT_PAD_TYPE_AUDIO:
      g_object_get (self->priv->mux, KMS_MUXING_PIPELINE_AUDIO_APPSRC,
          &appsrc, NULL);
      allowed =
          kms_http_get_endpoint_allowed_caps (element,
          KMS_ELEMENT_PAD_TYPE_AUDIO);
      caps =
          kms_http_get_endpoint_get_caps_from_profile (self,
          KMS_ELEMENT_PAD_TYPE_AUDIO);
      result = gst_caps_from_string (KMS_AGNOSTIC_AUDIO_CAPS);
      break;
    default:
      GST_ERROR_OBJECT (pad, "unknown pad");
      return FALSE;
  }

  /* make sure we only return results that intersect our padtemplate */
  tcaps = gst_pad_get_pad_template_caps (pad);
  if (tcaps != NULL) {
    /* Update result caps */
    gst_caps_unref (result);

    if (allowed == NULL) {
      result = gst_caps_ref (tcaps);
    } else {
      result = gst_caps_intersect (allowed, tcaps);
    }
    gst_caps_unref (tcaps);
  } else {
    GST_WARNING_OBJECT (pad,
        "Can not get capabilities from pad's template. Using agnostic's' caps");
  }

  if (caps == NULL) {
    GST_ERROR_OBJECT (self, "No caps from profile");
  } else {
    GstPad *srcpad;

    /* Get encodebin's caps filtering by profile */
    srcpad = gst_element_get_static_pad (appsrc, "src");
    tcaps = gst_pad_peer_query_caps (srcpad, caps);
    if (tcaps != NULL) {
      /* Filter against filtered encodebin's caps */
      GstCaps *aux;

      aux = gst_caps_intersect (tcaps, result);
      gst_caps_unref (result);
      gst_caps_unref (tcaps);
      result = aux;
    } else if (caps != NULL) {
      /* Filter against profile */
      GstCaps *aux;

      GST_WARNING_OBJECT (appsrc, "Using generic profile's caps");
      aux = gst_caps_intersect (caps, result);
      gst_caps_unref (result);
      result = aux;
    }
    g_object_unref (srcpad);
  }

  g_object_unref (appsrc);

  /* filter against the query filter when needed */
  if (filter != NULL) {
    GstCaps *aux;

    aux = gst_caps_intersect (result, filter);
    gst_caps_unref (result);
    result = aux;
  }

  gst_query_set_caps_result (query, result);
  gst_caps_unref (result);

  if (allowed != NULL)
    gst_caps_unref (allowed);

  if (caps != NULL)
    gst_caps_unref (caps);

  return TRUE;
}

static gboolean
kms_http_get_endpoint_query_accept_caps (KmsElement * element, GstPad * pad,
    GstQuery * query)
{
  KmsHttpGetEndpoint *self = KMS_HTTP_GET_ENDPOINT (element);
  GstCaps *caps, *accept;
  GstElement *appsrc;
  gboolean ret = TRUE;

  switch (kms_element_get_pad_type (element, pad)) {
    case KMS_ELEMENT_PAD_TYPE_VIDEO:
      g_object_get (self->priv->mux, KMS_MUXING_PIPELINE_VIDEO_APPSRC,
          &appsrc, NULL);
      caps = kms_http_get_endpoint_get_caps_from_profile (self,
          KMS_ELEMENT_PAD_TYPE_VIDEO);
      break;
    case KMS_ELEMENT_PAD_TYPE_AUDIO:
      g_object_get (self->priv->mux, KMS_MUXING_PIPELINE_AUDIO_APPSRC,
          &appsrc, NULL);
      caps = kms_http_get_endpoint_get_caps_from_profile (self,
          KMS_ELEMENT_PAD_TYPE_AUDIO);
      break;
    default:
      GST_ERROR_OBJECT (pad, "unknown pad");
      return FALSE;
  }

  if (caps == NULL) {
    GST_ERROR_OBJECT (self, "Can not accept caps without profile");
    gst_query_set_accept_caps_result (query, FALSE);
    g_object_unref (appsrc);
    return TRUE;
  }

  gst_query_parse_accept_caps (query, &accept);

  ret = gst_caps_can_intersect (accept, caps);

  if (ret) {
    GstPad *srcpad;

    srcpad = gst_element_get_static_pad (appsrc, "src");
    ret = gst_pad_peer_query_accept_caps (srcpad, accept);
    gst_object_unref (srcpad);
  } else {
    GST_ERROR_OBJECT (self, "Incompatbile caps %" GST_PTR_FORMAT, caps);
  }

  gst_caps_unref (caps);
  g_object_unref (appsrc);

  gst_query_set_accept_caps_result (query, ret);

  return TRUE;
}

static gboolean
kms_http_get_endpoint_sink_query (KmsElement * self, GstPad * pad,
    GstQuery * query)
{
  gboolean ret;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
      ret = kms_http_get_endpoint_query_caps (self, pad, query);
      break;
    case GST_QUERY_ACCEPT_CAPS:
      ret = kms_http_get_endpoint_query_accept_caps (self, pad, query);
      break;
    default:
      ret = KMS_ELEMENT_CLASS (parent_class)->sink_query (self, pad, query);
  }

  return ret;
}

static void
kms_http_get_endpoint_dispose (GObject * object)
{
  KmsHttpGetEndpoint *self = KMS_HTTP_GET_ENDPOINT (object);

  GST_DEBUG_OBJECT (self, "dispose");

  g_clear_object (&self->priv->appsink);
  g_clear_object (&self->priv->mux);

  /* clean up as possible. May be called multiple times */

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
kms_http_get_endpoint_finalize (GObject * object)
{
  KmsHttpGetEndpoint *self = KMS_HTTP_GET_ENDPOINT (object);

  GST_DEBUG_OBJECT (self, "finalize");

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstSample *
kms_http_get_endpoint_pull_sample_action (KmsHttpGetEndpoint * self)
{
  GstSample *sample;

  g_signal_emit_by_name (self->priv->appsink, "pull-sample", &sample);

  return sample;
}

static void
kms_http_get_endpoint_create_sink (KmsHttpGetEndpoint * self)
{
  self->priv->appsink = gst_element_factory_make ("appsink", NULL);

  g_object_set (self->priv->appsink, "emit-signals", TRUE, "qos", TRUE,
      "max-buffers", 1, "async", FALSE, NULL);
  g_signal_connect (self->priv->appsink, "new-sample",
      G_CALLBACK (new_sample_emit_signal_handler), self);
  g_signal_connect (self->priv->appsink, "eos", G_CALLBACK (eos_handler), self);
}

static void
kms_http_get_endpoint_add_sinkpad (KmsHttpGetEndpoint * self,
    const gchar * name, KmsElementPadType type)
{
  GstElement **appsink, *appsrc;
  GstPadProbeCallback cb;
  GstPad *sinkpad;
  gboolean unconfigured;

  if (type == KMS_ELEMENT_PAD_TYPE_AUDIO) {
    g_object_get (self->priv->mux, KMS_MUXING_PIPELINE_AUDIO_APPSRC, &appsrc,
        NULL);
    cb = set_audio_caps;
    appsink = &self->priv->audioappsink;
  } else if (type == KMS_ELEMENT_PAD_TYPE_VIDEO) {
    g_object_get (self->priv->mux, KMS_MUXING_PIPELINE_VIDEO_APPSRC, &appsrc,
        NULL);
    cb = set_video_caps;
    appsink = &self->priv->videoappsink;
  } else {
    GST_ERROR_OBJECT (self, "Invalid pad type provided");
    return;
  }

  if ((unconfigured = *appsink == NULL)) {
    /* First time that appsink is created */
    *appsink = gst_element_factory_make ("appsink", name);
    g_object_set (*appsink, "emit-signals", TRUE, "async", FALSE, "sync", FALSE,
        "qos", TRUE, NULL);
    g_signal_connect (*appsink, "new-sample",
        G_CALLBACK (new_sample_get_handler), appsrc);
    g_signal_connect (*appsink, "eos", G_CALLBACK (eos_handler), appsrc);
    gst_bin_add (GST_BIN (self), *appsink);
  }

  sinkpad = gst_element_get_static_pad (*appsink, "sink");

  if (unconfigured) {
    gst_pad_add_probe (sinkpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, cb, self,
        NULL);
    gst_element_sync_state_with_parent (*appsink);
  }

  kms_element_connect_sink_target (KMS_ELEMENT (self), sinkpad, type);

  g_object_unref (sinkpad);
  g_object_unref (appsrc);
}

static void
kms_http_get_endpoint_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  KmsHttpGetEndpoint *self = KMS_HTTP_GET_ENDPOINT (object);

  KMS_ELEMENT_LOCK (KMS_ELEMENT (self));
  switch (property_id) {
    case PROP_DVR:
      self->priv->use_dvr = g_value_get_boolean (value);
      break;
    case PROP_PROFILE:
      if (self->priv->profile != KMS_RECORDING_PROFILE_NONE) {
        GST_WARNING_OBJECT (self, "Already configured");
        break;
      }

      if ((self->priv->profile =
              g_value_get_enum (value)) == KMS_RECORDING_PROFILE_NONE) {
        break;
      }

      kms_http_get_endpoint_create_sink (self);
      self->priv->mux = kms_muxing_pipeline_new (KMS_MUXING_PIPELINE_PROFILE,
          self->priv->profile, KMS_MUXING_PIPELINE_SINK, self->priv->appsink,
          NULL);

      if (KMS_HTTP_ENDPOINT (self)->start) {
        /* Set internal pipeline to Playing state */
        kms_http_get_endpoint_change_internal_pipeline_state (KMS_HTTP_ENDPOINT
            (self), TRUE);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  KMS_ELEMENT_UNLOCK (KMS_ELEMENT (self));
}

static void
kms_http_get_endpoint_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  KmsHttpGetEndpoint *self = KMS_HTTP_GET_ENDPOINT (object);

  KMS_ELEMENT_LOCK (KMS_ELEMENT (self));
  switch (property_id) {
    case PROP_DVR:
      g_value_set_boolean (value, self->priv->use_dvr);
      break;
    case PROP_PROFILE:
      g_value_set_enum (value, self->priv->profile);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  KMS_ELEMENT_UNLOCK (KMS_ELEMENT (self));
}

static void
kms_http_get_endpoint_class_init (KmsHttpGetEndpointClass * klass)
{
  KmsHttpEndpointClass *kms_httpendpoint_class =
      KMS_HTTP_ENDPOINT_CLASS (klass);
  KmsElementClass *kms_element_class = KMS_ELEMENT_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "HttpGetEndpoint", "Generic", "Kurento http get end point plugin",
      "Santiago Carot-Nemesio <sancane.kurento@gmail.com>");

  gobject_class->set_property = kms_http_get_endpoint_set_property;
  gobject_class->get_property = kms_http_get_endpoint_get_property;
  gobject_class->dispose = kms_http_get_endpoint_dispose;
  gobject_class->finalize = kms_http_get_endpoint_finalize;

  kms_httpendpoint_class->start = GST_DEBUG_FUNCPTR
      (kms_http_get_endpoint_change_internal_pipeline_state);

  kms_element_class->sink_query =
      GST_DEBUG_FUNCPTR (kms_http_get_endpoint_sink_query);

  /* Install properties */
  obj_properties[PROP_DVR] = g_param_spec_boolean ("live-DVR",
      "Live digital video recorder", "Enables or disbles DVR", FALSE,
      G_PARAM_READWRITE);

  obj_properties[PROP_PROFILE] = g_param_spec_enum ("profile",
      "Recording profile",
      "The profile used for encapsulating the media",
      KMS_TYPE_RECORDING_PROFILE, HTTP_GET_ENDPOINT_RECORDING_PROFILE_DEFAULT,
      (G_PARAM_CONSTRUCT | G_PARAM_READWRITE));

  g_object_class_install_properties (gobject_class,
      N_PROPERTIES, obj_properties);

  /* set signals */
  http_get_ep_signals[SIGNAL_NEW_SAMPLE] =
      g_signal_new ("new-sample", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (KmsHttpGetEndpointClass, new_sample),
      NULL, NULL, __kms_core_marshal_ENUM__VOID, GST_TYPE_FLOW_RETURN, 0,
      G_TYPE_NONE);

  /* set actions */
  http_get_ep_signals[SIGNAL_PULL_SAMPLE] =
      g_signal_new ("pull-sample", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET (KmsHttpGetEndpointClass, pull_sample),
      NULL, NULL, __kms_core_marshal_BOXED__VOID, GST_TYPE_SAMPLE, 0,
      G_TYPE_NONE);

  klass->pull_sample = kms_http_get_endpoint_pull_sample_action;

  /* Registers a private structure for the instantiatable type */
  g_type_class_add_private (klass, sizeof (KmsHttpGetEndpointPrivate));
}

static void
kms_http_get_endpoint_init (KmsHttpGetEndpoint * self)
{
  self->priv = KMS_HTTP_GET_ENDPOINT_GET_PRIVATE (self);
  KMS_HTTP_ENDPOINT (self)->method = KMS_HTTP_ENDPOINT_METHOD_GET;

  self->priv->profile = KMS_RECORDING_PROFILE_NONE;
}

gboolean
kms_http_get_endpoint_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      KMS_TYPE_HTTP_GET_ENDPOINT);
}
