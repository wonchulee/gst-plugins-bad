/*
 * GStreamer
 * Copyright (C) 2009 Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gstvdputils.h"
#include "gstvdpvideobuffer.h"

#include "gstvdpoutputsrcpad.h"

GST_DEBUG_CATEGORY_STATIC (gst_vdp_output_src_pad_debug);
#define GST_CAT_DEFAULT gst_vdp_output_src_pad_debug

enum
{
  PROP_0,
  PROP_DEVICE
};

typedef enum _GstVdpOutputSrcPadFormat GstVdpOutputSrcPadFormat;

enum _GstVdpOutputSrcPadFormat
{
  GST_VDP_OUTPUT_SRC_PAD_FORMAT_RGB,
  GST_VDP_OUTPUT_SRC_PAD_FORMAT_VDPAU
};

struct _GstVdpOutputSrcPad
{
  GstPad pad;

  GstCaps *caps;

  GstCaps *input_caps;
  GstVdpOutputSrcPadFormat output_format;
  VdpRGBAFormat rgba_format;
  gint width, height;

  /* properties */
  GstVdpDevice *device;
};

struct _GstVdpOutputSrcPadClass
{
  GstPadClass pad_class;
};

#define DEBUG_INIT(bla) \
GST_DEBUG_CATEGORY_INIT (gst_vdp_output_src_pad_debug, "vdpoutputsrcpad", 0, "GstVdpOutputSrcPad");

G_DEFINE_TYPE_WITH_CODE (GstVdpOutputSrcPad, gst_vdp_output_src_pad,
    GST_TYPE_PAD, DEBUG_INIT ());

GstFlowReturn
gst_vdp_output_src_pad_push (GstVdpOutputSrcPad * vdp_pad,
    GstVdpOutputBuffer * output_buf, GError ** error)
{
  GstPad *pad;
  GstBuffer *outbuf;

  g_return_val_if_fail (GST_IS_VDP_OUTPUT_SRC_PAD (vdp_pad), GST_FLOW_ERROR);
  g_return_val_if_fail (GST_IS_VDP_OUTPUT_BUFFER (output_buf), GST_FLOW_ERROR);

  pad = (GstPad *) vdp_pad;

  if (G_UNLIKELY (!GST_PAD_CAPS (pad)))
    return GST_FLOW_NOT_NEGOTIATED;

  switch (vdp_pad->output_format) {
    case GST_VDP_OUTPUT_SRC_PAD_FORMAT_RGB:
    {
      guint size;

      gst_vdp_output_buffer_calculate_size (output_buf, &size);

      /* FIXME: we don't do pad_alloc here since we really want a buffer of
       * the specified size */
      outbuf = gst_buffer_new_and_alloc (size);
      gst_buffer_set_caps (outbuf, GST_PAD_CAPS (vdp_pad));

      if (!gst_vdp_output_buffer_download (output_buf, outbuf, error)) {
        gst_buffer_unref (GST_BUFFER_CAST (output_buf));
        gst_buffer_unref (outbuf);
        return GST_FLOW_ERROR;
      }

      gst_buffer_copy_metadata (outbuf, (const GstBuffer *) output_buf,
          GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS);
      gst_buffer_unref (GST_BUFFER_CAST (output_buf));
      break;
    }
    case GST_VDP_OUTPUT_SRC_PAD_FORMAT_VDPAU:
    {
      outbuf = GST_BUFFER_CAST (output_buf);
      break;
    }

    default:
      g_assert_not_reached ();
      break;
  }

  gst_buffer_set_caps (outbuf, GST_PAD_CAPS (vdp_pad));

  return gst_pad_push (pad, outbuf);
}

static GstFlowReturn
gst_vdp_output_src_pad_create_buffer (GstVdpOutputSrcPad * vdp_pad,
    GstVdpOutputBuffer ** output_buf, GError ** error)
{
  GstFlowReturn ret;
  GstBuffer *neg_buf;
  GstStructure *structure;

  /* negotiate */
  ret = gst_pad_alloc_buffer_and_set_caps (GST_PAD_CAST (vdp_pad),
      GST_BUFFER_OFFSET_NONE, 0, GST_PAD_CAPS (vdp_pad), &neg_buf);

  if (ret == GST_FLOW_OK) {
    gint new_width, new_height;

    structure = gst_caps_get_structure (GST_BUFFER_CAPS (neg_buf), 0);
    if (!gst_structure_get_int (structure, "width", &new_width) ||
        !gst_structure_get_int (structure, "height", &new_height))
      goto invalid_caps;

    if (new_width != vdp_pad->width || new_height != vdp_pad->height) {
      GST_DEBUG_OBJECT (vdp_pad, "new dimensions: %dx%d", new_width,
          new_height);

      vdp_pad->width = new_width;
      vdp_pad->height = new_height;

      gst_caps_set_simple (vdp_pad->input_caps,
          "width", G_TYPE_INT, new_width,
          "height", G_TYPE_INT, new_height, NULL);
    }

    gst_buffer_unref (neg_buf);
  }

  *output_buf = gst_vdp_output_buffer_new (vdp_pad->device,
      vdp_pad->rgba_format, vdp_pad->width, vdp_pad->height);
  if (!*output_buf)
    goto output_buf_error;

  gst_buffer_set_caps (GST_BUFFER_CAST (*output_buf), vdp_pad->input_caps);

  return GST_FLOW_OK;

invalid_caps:
  gst_buffer_unref (neg_buf);

  g_set_error (error, GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
      "Sink element allocated buffer with invalid caps");
  return GST_FLOW_ERROR;

output_buf_error:
  gst_buffer_unref (neg_buf);
  g_set_error (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ,
      "Couldn't create a GstVdpOutputBuffer");
  return GST_FLOW_ERROR;

}

static GstFlowReturn
gst_vdp_output_src_pad_alloc_with_caps (GstVdpOutputSrcPad * vdp_pad,
    GstCaps * caps, GstVdpOutputBuffer ** output_buf, GError ** error)
{
  GstFlowReturn ret;

  ret = gst_pad_alloc_buffer ((GstPad *) vdp_pad, 0, 0, caps,
      (GstBuffer **) output_buf);
  if (ret != GST_FLOW_OK)
    return ret;

  if (!GST_IS_VDP_OUTPUT_BUFFER (*output_buf))
    goto invalid_buf;

  return GST_FLOW_OK;

invalid_buf:
  gst_buffer_unref (GST_BUFFER (*output_buf));
  g_set_error (error, GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
      "Sink element returned buffer of wrong type");
  return GST_FLOW_ERROR;
}

GstFlowReturn
gst_vdp_output_src_pad_alloc_buffer (GstVdpOutputSrcPad * vdp_pad,
    GstVdpOutputBuffer ** output_buf, GError ** error)
{
  GstCaps *caps;
  GstFlowReturn ret;

  g_return_val_if_fail (GST_IS_VDP_OUTPUT_SRC_PAD (vdp_pad), GST_FLOW_ERROR);

  caps = GST_PAD_CAPS (vdp_pad);
  if (!caps)
    return GST_FLOW_NOT_NEGOTIATED;

  switch (vdp_pad->output_format) {
    case GST_VDP_OUTPUT_SRC_PAD_FORMAT_RGB:
    {
      ret = gst_vdp_output_src_pad_create_buffer (vdp_pad, output_buf, error);
      if (ret != GST_FLOW_OK)
        return ret;

      break;
    }

    case GST_VDP_OUTPUT_SRC_PAD_FORMAT_VDPAU:
    {
      ret = gst_vdp_output_src_pad_alloc_with_caps (vdp_pad, caps, output_buf,
          error);
      if (ret != GST_FLOW_OK)
        return ret;

      break;
    }

    default:
      g_assert_not_reached ();
      break;
  }

  return GST_FLOW_OK;

}

gboolean
gst_vdp_output_src_pad_negotiate_output (GstVdpOutputSrcPad * vdp_pad,
    GstCaps * video_caps)
{
  GstCaps *allowed_caps, *output_caps, *src_caps;
  const GstStructure *structure;

  g_return_val_if_fail (GST_IS_VDP_OUTPUT_SRC_PAD (vdp_pad), FALSE);
  g_return_val_if_fail (GST_IS_CAPS (video_caps), FALSE);

  allowed_caps = gst_pad_get_allowed_caps (GST_PAD_CAST (vdp_pad));
  if (G_UNLIKELY (!allowed_caps))
    goto allowed_caps_error;
  if (G_UNLIKELY (gst_caps_is_empty (allowed_caps))) {
    gst_caps_unref (allowed_caps);
    goto allowed_caps_error;
  }
  GST_DEBUG ("allowed_caps: %" GST_PTR_FORMAT, allowed_caps);

  output_caps = gst_vdp_video_to_output_caps (video_caps);
  src_caps = gst_caps_intersect (output_caps, allowed_caps);
  gst_caps_unref (output_caps);
  gst_caps_unref (allowed_caps);

  if (gst_caps_is_empty (src_caps))
    goto not_negotiated;

  gst_pad_fixate_caps (GST_PAD_CAST (vdp_pad), src_caps);

  GST_DEBUG ("src_caps: %" GST_PTR_FORMAT, src_caps);

  structure = gst_caps_get_structure (src_caps, 0);
  if (gst_structure_has_name (structure, "video/x-raw-rgb")) {
    if (!gst_vdp_caps_to_rgba_format (src_caps, &vdp_pad->rgba_format))
      return FALSE;

    vdp_pad->output_format = GST_VDP_OUTPUT_SRC_PAD_FORMAT_RGB;
  } else if (gst_structure_has_name (structure, "video/x-vdpau-output")) {
    if (!gst_structure_get_int (structure, "rgba-format",
            (gint *) & vdp_pad->rgba_format))
      return FALSE;

    vdp_pad->output_format = GST_VDP_OUTPUT_SRC_PAD_FORMAT_VDPAU;
  } else
    return FALSE;

  if (!gst_structure_get_int (structure, "width", &vdp_pad->width))
    return FALSE;
  if (!gst_structure_get_int (structure, "height", &vdp_pad->height))
    return FALSE;

  if (gst_pad_set_caps (GST_PAD (vdp_pad), src_caps)) {
    vdp_pad->input_caps = gst_caps_copy (video_caps);
    return TRUE;
  }
  return FALSE;

allowed_caps_error:
  GST_ERROR_OBJECT (vdp_pad, "Got invalid allowed caps");
  return FALSE;

not_negotiated:
  gst_caps_unref (src_caps);
  GST_ERROR_OBJECT (vdp_pad, "Couldn't find suitable output format");
  return FALSE;
}

static GstCaps *
gst_vdp_output_src_pad_getcaps (GstPad * pad)
{
  GstVdpOutputSrcPad *vdp_pad = (GstVdpOutputSrcPad *) pad;

  const GstCaps *templ_caps;

  if (vdp_pad->caps)
    return gst_caps_ref (vdp_pad->caps);

  else if ((templ_caps = gst_pad_get_pad_template_caps (pad)))
    return gst_caps_copy (templ_caps);

  return NULL;
}

static gboolean
gst_vdp_output_src_pad_activate_push (GstPad * pad, gboolean active)
{
  GstVdpOutputSrcPad *vdp_pad = GST_VDP_OUTPUT_SRC_PAD (pad);

  if (!active) {
    if (vdp_pad->caps)
      gst_caps_unref (vdp_pad->caps);
    vdp_pad->caps = NULL;

    if (vdp_pad->device)
      gst_object_unref (vdp_pad->device);
    vdp_pad->device = NULL;
  }

  return TRUE;
}

GstVdpOutputSrcPad *
gst_vdp_output_src_pad_new (GstPadTemplate * templ, const gchar * name)
{
  return g_object_new (GST_TYPE_VDP_OUTPUT_SRC_PAD, "name", name,
      "template", templ, "direction", GST_PAD_SRC, NULL);
}

static void
gst_vdp_output_src_pad_update_caps (GstVdpOutputSrcPad * vdp_pad)
{
  GstCaps *caps;
  const GstCaps *templ_caps;

  if (vdp_pad->caps)
    gst_caps_unref (vdp_pad->caps);

  caps = gst_vdp_output_buffer_get_allowed_caps (vdp_pad->device);

  if ((templ_caps = gst_pad_get_pad_template_caps (GST_PAD (vdp_pad)))) {
    vdp_pad->caps = gst_caps_intersect (caps, templ_caps);
    gst_caps_unref (caps);
  } else
    vdp_pad->caps = caps;
}

static void
gst_vdp_output_src_pad_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstVdpOutputSrcPad *vdp_pad = (GstVdpOutputSrcPad *) object;

  switch (prop_id) {
    case PROP_DEVICE:
      g_value_set_object (value, vdp_pad->device);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vdp_output_src_pad_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVdpOutputSrcPad *vdp_pad = (GstVdpOutputSrcPad *) object;

  switch (prop_id) {
    case PROP_DEVICE:
      if (vdp_pad->device)
        g_object_unref (vdp_pad->device);
      vdp_pad->device = g_value_dup_object (value);
      gst_vdp_output_src_pad_update_caps (vdp_pad);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vdp_output_src_pad_init (GstVdpOutputSrcPad * vdp_pad)
{
  GstPad *pad = GST_PAD (vdp_pad);

  vdp_pad->caps = NULL;

  vdp_pad->device = NULL;

  gst_pad_set_getcaps_function (pad,
      GST_DEBUG_FUNCPTR (gst_vdp_output_src_pad_getcaps));
  gst_pad_set_activatepush_function (pad,
      GST_DEBUG_FUNCPTR (gst_vdp_output_src_pad_activate_push));
}

static void
gst_vdp_output_src_pad_class_init (GstVdpOutputSrcPadClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = gst_vdp_output_src_pad_get_property;
  object_class->set_property = gst_vdp_output_src_pad_set_property;

  /**
   * GstVdpVideoSrcPad:device:
   *
   * The #GstVdpDevice this pool is bound to.
   */
  g_object_class_install_property
      (object_class,
      PROP_DEVICE,
      g_param_spec_object ("device",
          "Device",
          "The GstVdpDevice the pad should use",
          GST_TYPE_VDP_DEVICE, G_PARAM_READWRITE));
}
