/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * This file:
 * Copyright (C) 2003 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2010 David Schleef <ds@schleef.org>
 * Copyright (C) 2014 Renesas Electronics Corporation
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

/**
 * SECTION:element-vspfilter
 *
 * The vspfilter element is used for the color conversion and
 * scaling with the hardware acceleration on R-Car SoC.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gstvspfilter.h"

#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/gstquery.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

GST_DEBUG_CATEGORY (vspfilter_debug);
#define GST_CAT_DEFAULT vspfilter_debug
GST_DEBUG_CATEGORY_EXTERN (GST_CAT_PERFORMANCE);

GType gst_vsp_filter_get_type (void);

#define gst_vsp_filter_parent_class parent_class
G_DEFINE_TYPE (GstVspFilter, gst_vsp_filter, GST_TYPE_VIDEO_FILTER);

enum
{
  PROP_0,
  PROP_VSP_DEVFILE_INPUT,
  PROP_VSP_DEVFILE_OUTPUT,
};

#define CLEAR(x) memset (&(x), 0, sizeof (x))

#define CSP_VIDEO_CAPS GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL)

static GstStaticPadTemplate gst_vsp_filter_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (CSP_VIDEO_CAPS)
    );

static GstStaticPadTemplate gst_vsp_filter_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (CSP_VIDEO_CAPS)
    );

static void gst_vsp_filter_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_vsp_filter_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);

static GstFlowReturn gst_vsp_filter_transform_frame_process (GstVideoFilter *
    filter, GstVspFilterFrameInfo * in_vframe_info,
    GstVspFilterFrameInfo * out_vframe_info,
    gint in_stride[GST_VIDEO_MAX_PLANES],
    gint out_stride[GST_VIDEO_MAX_PLANES]);

/* copies the given caps */
static GstCaps *
gst_vsp_filter_caps_remove_format_info (GstCaps * caps)
{
  GstStructure *st;
  gint i, n;
  GstCaps *res;

  res = gst_caps_new_empty ();

  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    st = gst_caps_get_structure (caps, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure (res, st))
      continue;

    st = gst_structure_copy (st);
    gst_structure_remove_fields (st, "format",
        "colorimetry", "chroma-site", NULL);

    gst_caps_append_structure (res, st);
  }

  return res;
}

struct extensions_t
{
  GstVideoFormat format;
  guint fourcc;
  enum v4l2_mbus_pixelcode code;
  int n_planes;
};

static const struct extensions_t exts[] = {
  {GST_VIDEO_FORMAT_RGB16, V4L2_PIX_FMT_RGB565, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_RGB, V4L2_PIX_FMT_RGB24, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_BGR, V4L2_PIX_FMT_BGR24, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_ARGB, V4L2_PIX_FMT_RGB32, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_xRGB, V4L2_PIX_FMT_RGB32, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_BGRA, V4L2_PIX_FMT_BGR32, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_BGRx, V4L2_PIX_FMT_BGR32, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_I420, V4L2_PIX_FMT_YUV420M, V4L2_MBUS_FMT_AYUV8_1X32, 3},
  {GST_VIDEO_FORMAT_NV12, V4L2_PIX_FMT_NV12M, V4L2_MBUS_FMT_AYUV8_1X32, 2},
  {GST_VIDEO_FORMAT_UYVY, V4L2_PIX_FMT_UYVY, V4L2_MBUS_FMT_AYUV8_1X32, 1},
};

static gint
set_colorspace (GstVideoFormat vid_fmt, guint * fourcc,
    enum v4l2_mbus_pixelcode *code, guint * n_planes)
{
  int nr_exts = sizeof (exts) / sizeof (exts[0]);
  int i;

  for (i = 0; i < nr_exts; i++) {
    if (vid_fmt == exts[i].format) {
      if (fourcc)
        *fourcc = exts[i].fourcc;
      if (code)
        *code = exts[i].code;
      if (n_planes)
        *n_planes = exts[i].n_planes;
      return 0;
    }
  }

  return -1;
}

static gint
xioctl (gint fd, gint request, void *arg)
{
  int r;

  do
    r = ioctl (fd, request, arg);
  while (-1 == r && EINTR == errno);

  return r;
}

static gboolean
gst_vsp_filter_is_caps_format_supported_for_vsp (GstVspFilter * space,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  gint in_w = 0, in_h = 0;
  gint out_w = 0, out_h = 0;
  GstVspFilterVspInfo *vsp_info;
  GstStructure *ins, *outs;
  GstVideoFormat in_fmt, out_fmt;
  guint in_v4l2pix, out_v4l2pix;
  struct v4l2_format v4l2fmt;
  gint ret;

  vsp_info = space->vsp_info;

  if (direction == GST_PAD_SRC) {
    ins = gst_caps_get_structure (othercaps, 0);
    outs = gst_caps_get_structure (caps, 0);
  } else {
    ins = gst_caps_get_structure (caps, 0);
    outs = gst_caps_get_structure (othercaps, 0);
  }

  in_fmt =
      gst_video_format_from_string (gst_structure_get_string (ins, "format"));
  if (in_fmt == GST_VIDEO_FORMAT_UNKNOWN) {
    GST_ERROR_OBJECT (space, "failed to convert video format");
    return FALSE;
  }

  out_fmt =
      gst_video_format_from_string (gst_structure_get_string (outs, "format"));
  if (out_fmt == GST_VIDEO_FORMAT_UNKNOWN) {
    GST_ERROR_OBJECT (space, "failed to convert video format");
    return FALSE;
  }

  /* Convert a pixel format definition from GStreamer to V4L2 */
  ret = set_colorspace (in_fmt, &in_v4l2pix, NULL, NULL);
  if (ret < 0) {
    GST_ERROR_OBJECT (space, "set_colorspace() failed");
    return FALSE;
  }

  ret = set_colorspace (out_fmt, &out_v4l2pix, NULL, NULL);
  if (ret < 0) {
    GST_ERROR_OBJECT (space, "set_colorspace() failed");
    return FALSE;
  }

  gst_structure_get_int (ins, "width", &in_w);
  gst_structure_get_int (ins, "height", &in_h);

  gst_structure_get_int (outs, "width", &out_w);
  gst_structure_get_int (outs, "height", &out_h);

  v4l2fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  v4l2fmt.fmt.pix_mp.width = in_w;
  v4l2fmt.fmt.pix_mp.height = in_h;
  v4l2fmt.fmt.pix_mp.pixelformat = in_v4l2pix;
  v4l2fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

  ret = xioctl (vsp_info->v4lout_fd, VIDIOC_TRY_FMT, &v4l2fmt);
  if (ret < 0) {
    GST_ERROR_OBJECT (space,
        "VIDIOC_TRY_FMT failed. (%dx%d pixelformat=%d)", in_w, in_h,
        in_v4l2pix);
    return FALSE;
  }

  v4l2fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  v4l2fmt.fmt.pix_mp.width = out_w;
  v4l2fmt.fmt.pix_mp.height = out_h;
  v4l2fmt.fmt.pix_mp.pixelformat = out_v4l2pix;
  v4l2fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

  ret = xioctl (vsp_info->v4lcap_fd, VIDIOC_TRY_FMT, &v4l2fmt);
  if (ret < 0) {
    GST_ERROR_OBJECT (space,
        "VIDIOC_TRY_FMT failed. (%dx%d pixelformat=%d)", out_w, out_h,
        out_v4l2pix);
    return FALSE;
  }

  return TRUE;
}

static GstCaps *
gst_vsp_filter_fixate_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstCaps *result;
  gint from_w, from_h;
  gint w = 0, h = 0;
  GstStructure *ins, *outs;
  GstVspFilter *space;

  space = GST_VSP_FILTER_CAST (trans);

  GST_DEBUG_OBJECT (trans, "caps %" GST_PTR_FORMAT, caps);
  GST_DEBUG_OBJECT (trans, "othercaps %" GST_PTR_FORMAT, othercaps);

  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);

  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (othercaps, 0);

  gst_structure_get_int (ins, "width", &from_w);
  gst_structure_get_int (ins, "height", &from_h);

  gst_structure_get_int (outs, "width", &w);
  gst_structure_get_int (outs, "height", &h);

  if (!w || !h) {
    gst_structure_fixate_field_nearest_int (outs, "height", from_h);
    gst_structure_fixate_field_nearest_int (outs, "width", from_w);
  }

  result = gst_caps_intersect (othercaps, caps);
  if (gst_caps_is_empty (result)) {
    gst_caps_unref (result);
    result = othercaps;
  } else {
    gst_caps_unref (othercaps);
  }

  /* fixate remaining fields */
  result = gst_caps_fixate (result);

  if (!gst_vsp_filter_is_caps_format_supported_for_vsp (space, direction,
          caps, result)) {
    GST_ERROR_OBJECT (trans, "Unsupported caps format for vsp");
    return NULL;
  }

  GST_DEBUG_OBJECT (trans, "result caps %" GST_PTR_FORMAT, result);

  return result;
}

static gboolean
gst_vsp_filter_filter_meta (GstBaseTransform * trans, GstQuery * query,
    GType api, const GstStructure * params)
{
  /* propose all metadata upstream */
  return TRUE;
}

/* The caps can be transformed into any other caps with format info removed.
 * However, we should prefer passthrough, so if passthrough is possible,
 * put it first in the list. */
static GstCaps *
gst_vsp_filter_transform_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *tmp, *tmp2;
  GstCaps *result;
  GstCaps *caps_full_range_sizes;
  GstStructure *structure;
  gint i, n;

  /* Get all possible caps that we can transform to */
  tmp = gst_vsp_filter_caps_remove_format_info (caps);

  caps_full_range_sizes = gst_caps_new_empty ();
  n = gst_caps_get_size (tmp);
  for (i = 0; i < n; i++) {
    structure = gst_caps_get_structure (tmp, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure (caps_full_range_sizes,
            structure))
      continue;

    /* make copy */
    structure = gst_structure_copy (structure);
    gst_structure_set (structure,
        "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);

    gst_caps_append_structure (caps_full_range_sizes, structure);
  }

  gst_caps_unref (tmp);

  if (filter) {
    tmp2 = gst_caps_intersect_full (filter, caps_full_range_sizes,
        GST_CAPS_INTERSECT_FIRST);

    gst_caps_unref (caps_full_range_sizes);
    tmp = tmp2;
  } else
    tmp = caps_full_range_sizes;

  result = tmp;

  GST_DEBUG_OBJECT (btrans, "transformed %" GST_PTR_FORMAT " into %"
      GST_PTR_FORMAT, caps, result);

  return result;
}

static gboolean
gst_vsp_filter_transform_meta (GstBaseTransform * trans, GstBuffer * outbuf,
    GstMeta * meta, GstBuffer * inbuf)
{
  /* copy other metadata */
  return TRUE;
}

static gboolean
request_buffers (GstVspFilter * space, gint fd, gint index,
    enum v4l2_buf_type buftype, gint n_bufs, enum v4l2_memory io[MAX_DEVICES])
{
  struct v4l2_requestbuffers req;
  GstVspFilterVspInfo *vsp_info;

  vsp_info = space->vsp_info;

  /* input buffer */
  CLEAR (req);

  req.count = n_bufs;
  req.type = buftype;
  req.memory = io[index];

  if (-1 == xioctl (fd, VIDIOC_REQBUFS, &req)) {
    GST_ERROR_OBJECT (space, "VIDIOC_REQBUFS for %s errno=%d",
        vsp_info->dev_name[index], errno);
    return FALSE;
  }

  GST_DEBUG_OBJECT (space, "req.count = %d", req.count);

  return TRUE;
}

static gboolean
set_format (GstVspFilter * space, gint fd, guint width, guint height,
    gint stride[GST_VIDEO_MAX_PLANES], gint index, guint captype,
    enum v4l2_buf_type buftype, enum v4l2_memory io[MAX_DEVICES])
{
  GstVspFilterVspInfo *vsp_info;
  struct v4l2_format fmt;
  gint i;

  vsp_info = space->vsp_info;

  CLEAR (fmt);

  fmt.type = buftype;
  fmt.fmt.pix_mp.width = width;
  fmt.fmt.pix_mp.height = height;
  fmt.fmt.pix_mp.pixelformat = vsp_info->format[index];
  fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

  for (i = 0; i < vsp_info->n_planes[index]; i++) {
    GST_DEBUG_OBJECT (space, "Set bytesperline = %d (index = %d plane = %d)\n",
        stride[i], index, i);
    fmt.fmt.pix_mp.plane_fmt[i].bytesperline = stride[i];
  }

  if (-1 == xioctl (fd, VIDIOC_S_FMT, &fmt)) {
    GST_ERROR_OBJECT (space, "VIDIOC_S_FMT for %s failed.",
        vsp_info->dev_name[index]);
    return FALSE;
  }

  GST_DEBUG_OBJECT (space, "pixelformat = %c%c%c%c (%c%c%c%c)",
      (fmt.fmt.pix_mp.pixelformat >> 0) & 0xff,
      (fmt.fmt.pix_mp.pixelformat >> 8) & 0xff,
      (fmt.fmt.pix_mp.pixelformat >> 16) & 0xff,
      (fmt.fmt.pix_mp.pixelformat >> 24) & 0xff,
      (vsp_info->format[index] >> 0) & 0xff,
      (vsp_info->format[index] >> 8) & 0xff,
      (vsp_info->format[index] >> 16) & 0xff,
      (vsp_info->format[index] >> 24) & 0xff);
  GST_DEBUG_OBJECT (space, "num_planes = %d", fmt.fmt.pix_mp.num_planes);

  for (i = 0; i < fmt.fmt.pix_mp.num_planes; i++) {
    GST_DEBUG_OBJECT (space, "plane_fmt[%d].sizeimage = %d",
        i, fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
    GST_DEBUG_OBJECT (space, "plane_fmt[%d].bytesperline = %d",
        i, fmt.fmt.pix_mp.plane_fmt[i].bytesperline);
    vsp_info->plane_stride[index][i] = fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
  }

  if (!request_buffers (space, fd, index, buftype, N_BUFFERS, io)) {
    GST_ERROR_OBJECT (space, "request_buffers for %s failed.",
        vsp_info->dev_name[index]);
    return FALSE;
  }

  return TRUE;
}

static gint
fgets_with_openclose (gchar * fname, gchar * buf, size_t maxlen)
{
  FILE *fp;
  gchar *s;

  if ((fp = fopen (fname, "r")) != NULL) {
    s = fgets (buf, maxlen, fp);
    fclose (fp);
    return (s != NULL) ? strlen (buf) : 0;
  } else {
    return -1;
  }
}

static gint
open_v4lsubdev (gchar * prefix, const gchar * target, gchar * path)
{
  gint i;
  gchar subdev_name[256];

  for (i = 0; i < 256; i++) {
    snprintf (path, 255, "/sys/class/video4linux/v4l-subdev%d/name", i);
    if (fgets_with_openclose (path, subdev_name, 255) < 0)
      break;
    if (((prefix &&
                (strncmp (subdev_name, prefix, strlen (prefix)) == 0)) ||
            (prefix == NULL)) && (strstr (subdev_name, target) != NULL)) {
      snprintf (path, 255, "/dev/v4l-subdev%d", i);
      return open (path, O_RDWR /* required | O_NONBLOCK */ , 0);
    }
  }

  return -1;
}

static gint
open_media_device (GstVspFilter * space)
{
  GstVspFilterVspInfo *vsp_info;
  struct stat st;
  gchar path[256];
  gchar *dev;
  gint i;

  vsp_info = space->vsp_info;

  dev = strrchr (vsp_info->dev_name[CAP], '/') + 1;
  for (i = 0; i < 256; i++) {
    sprintf (path, "/sys/class/video4linux/%s/device/media%d",
			dev, i);
    if (0 == stat (path, &st)) {
      sprintf (path, "/dev/media%d", i);
      GST_DEBUG_OBJECT (space, "media device = %s", path);
      return open (path, O_RDWR);
    }
  }

  GST_ERROR_OBJECT (space, "No media device for %s", vsp_info->ip_name);

  return -1;
}

static gint
get_media_entity (GstVspFilter * space, gchar * name,
    struct media_entity_desc *entity)
{
  gint i, ret;

  for (i = 0; i < 256; i++) {
    CLEAR (*entity);
    entity->id = i | MEDIA_ENT_ID_FLAG_NEXT;
    ret = ioctl (space->vsp_info->media_fd, MEDIA_IOC_ENUM_ENTITIES, entity);
    if (ret < 0) {
      if (errno == EINVAL)
        break;
    } else if (strcmp (entity->name, name) == 0) {
      return i;
    }
  }

  GST_ERROR_OBJECT (space, "No media entity for %s", name);

  return -1;
}

static gint
activate_link (GstVspFilter * space, struct media_entity_desc *src,
    struct media_entity_desc *sink)
{
  struct media_links_enum links;
  struct media_link_desc *target_link;
  gint ret, i;
  GstVspFilterVspInfo *vsp_info;

  vsp_info = space->vsp_info;

  target_link = NULL;
  CLEAR (links);
  links.pads = malloc (sizeof (struct media_pad_desc) * src->pads);
  links.links = malloc (sizeof (struct media_link_desc) * src->links);

  links.entity = src->id;
  ret = ioctl (vsp_info->media_fd, MEDIA_IOC_ENUM_LINKS, &links);
  if (ret) {
    GST_ERROR_OBJECT (space, "MEDIA_IOC_ENUM_LINKS failed");
    return ret;
  }

  for (i = 0; i < src->links; i++) {
    if (links.links[i].sink.entity == sink->id) {
      target_link = &links.links[i];
    } else if (links.links[i].flags & MEDIA_LNK_FL_ENABLED) {
      GST_WARNING_OBJECT (space, "An active link to %02x found.",
          links.links[i].sink.entity);
      return -1;
    }
  }

  if (!target_link)
    return -1;

  target_link->flags |= MEDIA_LNK_FL_ENABLED;

  return ioctl (vsp_info->media_fd, MEDIA_IOC_SETUP_LINK, target_link);
}

static gint
deactivate_link (GstVspFilter * space, struct media_entity_desc *src)
{
  struct media_links_enum links;
  struct media_link_desc *target_link;
  int ret, i;
  GstVspFilterVspInfo *vsp_info;

  vsp_info = space->vsp_info;

  CLEAR (links);
  links.pads = g_malloc (sizeof (struct media_pad_desc) * src->pads);
  links.links = g_malloc (sizeof (struct media_link_desc) * src->links);

  links.entity = src->id;
  ret = ioctl (vsp_info->media_fd, MEDIA_IOC_ENUM_LINKS, &links);
  if (ret) {
    GST_ERROR_OBJECT (space, "MEDIA_IOC_ENUM_LINKS failed");
    goto leave;
  }

  for (i = 0; i < src->links; i++) {
    if ((links.links[i].flags & MEDIA_LNK_FL_ENABLED) &&
        !(links.links[i].flags & MEDIA_LNK_FL_IMMUTABLE)) {
      struct media_entity_desc next;

      target_link = &links.links[i];
      CLEAR (next);
      next.id = target_link->sink.entity;
      ret = ioctl (vsp_info->media_fd, MEDIA_IOC_ENUM_ENTITIES, &next);
      if (ret) {
        GST_ERROR_OBJECT (space, "ioctl(MEDIA_IOC_ENUM_ENTITIES, %d) failed.",
            target_link->sink.entity);
        goto leave;
      }
      ret = deactivate_link (space, &next);
      if (ret)
        GST_ERROR_OBJECT (space, "deactivate_link(%s) failed.", next.name);
      target_link->flags &= ~MEDIA_LNK_FL_ENABLED;
      ret = ioctl (vsp_info->media_fd, MEDIA_IOC_SETUP_LINK, target_link);
      if (ret)
        GST_ERROR_OBJECT (space, "MEDIA_IOC_SETUP_LINK failed.");
      GST_DEBUG_OBJECT (space, "A link from %s to %s deactivated.", src->name,
          next.name);
    }
  }

leave:
  g_free (links.pads);
  g_free (links.links);

  return ret;
}

static gboolean
init_entity_pad (GstVspFilter * space, gint fd, gint index, guint pad,
    guint width, guint height, guint code)
{
  struct v4l2_subdev_format sfmt;

  CLEAR (sfmt);

  sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
  sfmt.pad = pad;
  sfmt.format.width = width;
  sfmt.format.height = height;
  sfmt.format.code = code;
  sfmt.format.field = V4L2_FIELD_NONE;
  sfmt.format.colorspace = V4L2_COLORSPACE_SRGB;

  if (-1 == xioctl (fd, VIDIOC_SUBDEV_S_FMT, &sfmt)) {
    GST_ERROR_OBJECT (space, "VIDIOC_SUBDEV_S_FMT for %s failed.",
        space->vsp_info->entity_name[index]);
    return FALSE;
  }

  return TRUE;
}

static gboolean
get_wpf_output_entity_name (GstVspFilter * space, gchar * wpf_output_name,
    size_t maxlen)
{
  GstVspFilterVspInfo *vsp_info;
  gchar *dev;
  gchar path[256];
  gchar *newline;

  vsp_info = space->vsp_info;

  dev = strrchr (vsp_info->dev_name[CAP], '/') + 1;
  snprintf (path, sizeof (path), "/sys/class/video4linux/%s/name", dev);
  if (fgets_with_openclose (path, wpf_output_name, maxlen) < 0) {
    GST_ERROR_OBJECT (space, "%s couldn't open", path);
    return FALSE;
  }
  /* Delete a newline */
  newline = strrchr (wpf_output_name, '\n');
  if (newline)
    *newline = '\0';

  return TRUE;
}

static gboolean
set_vsp_entities (GstVspFilter * space, GstVideoFormat in_fmt, gint in_width,
    gint in_height, gint in_stride[GST_VIDEO_MAX_PLANES],
    GstVideoFormat out_fmt, gint out_width, gint out_height,
    gint out_stride[GST_VIDEO_MAX_PLANES], enum v4l2_memory io[MAX_DEVICES])
{
  GstVspFilterVspInfo *vsp_info;
  gint ret;
  gchar tmp[256];

  vsp_info = space->vsp_info;

  if (vsp_info->already_setup_info)
    return TRUE;

  set_colorspace (in_fmt, &vsp_info->format[OUT], &vsp_info->code[OUT],
      &vsp_info->n_planes[OUT]);
  set_colorspace (out_fmt, &vsp_info->format[CAP], &vsp_info->code[CAP],
      &vsp_info->n_planes[CAP]);

  GST_DEBUG_OBJECT (space, "in format=%d  out format=%d", in_fmt, out_fmt);

  GST_DEBUG_OBJECT (space, "set_colorspace[OUT]: format=%d code=%d n_planes=%d",
      vsp_info->format[OUT], vsp_info->code[OUT], vsp_info->n_planes[OUT]);
  GST_DEBUG_OBJECT (space, "set_colorspace[CAP]: format=%d code=%d n_planes=%d",
      vsp_info->format[CAP], vsp_info->code[CAP], vsp_info->n_planes[CAP]);

  if (!set_format (space, vsp_info->v4lout_fd, in_width, in_height,
          in_stride, OUT, V4L2_CAP_VIDEO_OUTPUT_MPLANE,
          V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, io)) {
    GST_ERROR_OBJECT (space, "set_format for %s failed (%dx%d)",
        vsp_info->dev_name[OUT], in_width, in_height);
    return FALSE;
  }
  if (!set_format (space, vsp_info->v4lcap_fd, out_width, out_height,
          out_stride, CAP, V4L2_CAP_VIDEO_CAPTURE_MPLANE,
          V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, io)) {
    GST_ERROR_OBJECT (space, "set_format for %s failed (%dx%d)",
        vsp_info->dev_name[CAP], out_width, out_height);
    return FALSE;
  }

  sprintf (tmp, "%s %s", vsp_info->ip_name, vsp_info->entity_name[OUT]);
  ret = get_media_entity (space, tmp, &vsp_info->entity[OUT]);
  GST_DEBUG_OBJECT (space, "ret = %d, entity[OUT] = %s", ret,
      vsp_info->entity[OUT].name);
  sprintf (tmp, "%s %s", vsp_info->ip_name, vsp_info->entity_name[CAP]);
  ret = get_media_entity (space, tmp, &vsp_info->entity[CAP]);
  GST_DEBUG_OBJECT (space, "ret = %d, entity[CAP] = %s", ret,
      vsp_info->entity[CAP].name);

  /* Deactivate the current pipeline. */
  deactivate_link (space, &vsp_info->entity[OUT]);

  GST_DEBUG_OBJECT (space,
      "in_info->width=%d in_info->height=%d out_info->width=%d out_info->height=%d",
      in_width, in_height, out_width, out_height);

  /* link up entities for VSP1 V4L2 */
  if ((in_width != out_width) || (in_height != out_height)) {
    gchar path[256];
    const gchar *resz_entity_name = "uds.0";

    if (vsp_info->resz_subdev_fd < 0) {
      vsp_info->resz_subdev_fd =
          open_v4lsubdev (vsp_info->ip_name, resz_entity_name, path);
      if (vsp_info->resz_subdev_fd < 0) {
        GST_ERROR_OBJECT (space, "cannot open a subdev file for %s",
            resz_entity_name);
        return FALSE;
      }
    }

    sprintf (tmp, "%s %s", vsp_info->ip_name, resz_entity_name);
    ret = get_media_entity (space, tmp, &vsp_info->entity[RESZ]);
    if (ret < 0) {
      GST_ERROR_OBJECT (space, "Entity for %s not found.", resz_entity_name);
      return FALSE;
    }
    GST_DEBUG_OBJECT (space, "A entity for %s found.", resz_entity_name);
    ret =
        activate_link (space, &vsp_info->entity[OUT], &vsp_info->entity[RESZ]);
    if (ret) {
      GST_ERROR_OBJECT (space, "Cannot enable a link from %s to %s",
          vsp_info->entity_name[OUT], resz_entity_name);
      return FALSE;
    }

    GST_DEBUG_OBJECT (space, "A link from %s to %s enabled.",
        vsp_info->entity_name[OUT], resz_entity_name);

    ret =
        activate_link (space, &vsp_info->entity[RESZ], &vsp_info->entity[CAP]);
    if (ret) {
      GST_ERROR_OBJECT (space, "Cannot enable a link from %s to %s",
          resz_entity_name, vsp_info->entity_name[CAP]);
      return FALSE;
    }

    GST_DEBUG_OBJECT (space, "A link from %s to %s enabled.",
        resz_entity_name, vsp_info->entity_name[CAP]);

    if (!init_entity_pad (space, vsp_info->resz_subdev_fd, RESZ, 0, in_width,
            in_height, vsp_info->code[CAP])) {
      GST_ERROR_OBJECT (space, "init_entity_pad failed");
      return FALSE;
    }
    if (!init_entity_pad (space, vsp_info->resz_subdev_fd, RESZ, 1, out_width,
            out_height, vsp_info->code[CAP])) {
      GST_ERROR_OBJECT (space, "init_entity_pad failed");
      return FALSE;
    }
  } else {
    if (vsp_info->resz_subdev_fd >= 0) {
      close (vsp_info->resz_subdev_fd);
      vsp_info->resz_subdev_fd = -1;
    }

    ret = activate_link (space, &vsp_info->entity[OUT], &vsp_info->entity[CAP]);
    if (ret) {
      GST_ERROR_OBJECT (space, "Cannot enable a link from %s to %s",
          vsp_info->entity_name[OUT], vsp_info->entity_name[CAP]);
      return FALSE;
    }
    GST_DEBUG_OBJECT (space, "A link from %s to %s enabled.",
        vsp_info->entity_name[OUT], vsp_info->entity_name[CAP]);
  }

  if (!get_wpf_output_entity_name (space, tmp, sizeof (tmp))) {
    GST_ERROR_OBJECT (space, "get_wpf_output_entity_name failed");
    return FALSE;
  }
  ret = get_media_entity (space, tmp, &vsp_info->entity[3]);
  ret = activate_link (space, &vsp_info->entity[CAP], &vsp_info->entity[3]);
  if (ret) {
    GST_ERROR_OBJECT (space,
        "Cannot enable a link from %s to vsp1.2 wpf.0 output",
        vsp_info->entity_name[CAP]);
    return FALSE;
  }
  GST_DEBUG_OBJECT (space,
      "%s has been linked as the terminal of the entities link", tmp);

  /* sink pad in RPF */
  if (!init_entity_pad (space, vsp_info->v4lsub_fd[OUT], OUT, 0, in_width,
          in_height, vsp_info->code[OUT])) {
    GST_ERROR_OBJECT (space, "init_entity_pad failed");
    return FALSE;
  }
  /* source pad in RPF */
  if (!init_entity_pad (space, vsp_info->v4lsub_fd[OUT], OUT, 1, in_width,
          in_height, vsp_info->code[CAP])) {
    GST_ERROR_OBJECT (space, "init_entity_pad failed");
    return FALSE;
  }
  /* sink pad in WPF */
  if (!init_entity_pad (space, vsp_info->v4lsub_fd[CAP], CAP, 0, out_width,
          out_height, vsp_info->code[CAP])) {
    GST_ERROR_OBJECT (space, "init_entity_pad failed");
    return FALSE;
  }
  /* source pad in WPF */
  if (!init_entity_pad (space, vsp_info->v4lsub_fd[CAP], CAP, 1, out_width,
          out_height, vsp_info->code[CAP])) {
    GST_ERROR_OBJECT (space, "init_entity_pad failed");
    return FALSE;
  }

  vsp_info->already_setup_info = TRUE;

  return TRUE;
}

static void
close_device (GstVspFilter * space, gint fd, gint index)
{
  GST_DEBUG_OBJECT (space, "closing the device ...");

  if (-1 == close (fd)) {
    GST_ERROR_OBJECT (space, "close for %s failed",
        space->vsp_info->dev_name[index]);
    return;
  }
}

static void
stop_capturing (GstVspFilter * space, int fd, int index,
    enum v4l2_buf_type buftype)
{
  GstVspFilterVspInfo *vsp_info;

  vsp_info = space->vsp_info;

  GST_DEBUG_OBJECT (space, "stop streaming... ");;

  if (-1 == xioctl (fd, VIDIOC_STREAMOFF, &buftype)) {
    GST_ERROR_OBJECT (space, "VIDIOC_STREAMOFF for %s failed",
        vsp_info->dev_name[index]);
    return;
  }
}

static void
gst_vsp_filter_finalize (GObject * obj)
{
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;

  space = GST_VSP_FILTER (obj);
  vsp_info = space->vsp_info;

  g_free (vsp_info->dev_name[OUT]);
  g_free (vsp_info->dev_name[CAP]);

  g_free (space->vsp_info);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static gboolean
init_device (GstVspFilter * space, gint fd, gint index, guint captype,
    enum v4l2_buf_type buftype)
{
  GstVspFilterVspInfo *vsp_info;
  struct v4l2_capability cap;
  gchar *p;
  gchar path[256];

  vsp_info = space->vsp_info;

  if (vsp_info->already_device_initialized[index]) {
    GST_WARNING_OBJECT (space, "The device is already initialized");
    return FALSE;
  }

  if (-1 == xioctl (fd, VIDIOC_QUERYCAP, &cap)) {
    GST_ERROR_OBJECT (space, "VIDIOC_QUERYCAP for %s errno=%d",
        vsp_info->dev_name[index], errno);
    return FALSE;
  }

  /* look for a counterpart */
  p = g_strdup ((const char *) cap.card);
  p = strtok (p, " ");
  if (vsp_info->ip_name == NULL) {
    vsp_info->ip_name = p;
    GST_DEBUG_OBJECT (space, "ip_name = %s", vsp_info->ip_name);
  } else if (strcmp (vsp_info->ip_name, p) != 0) {
    GST_ERROR_OBJECT (space, "ip name mismatch vsp_info->ip_name=%s p=%s",
        vsp_info->ip_name, p);
    return FALSE;
  }

  vsp_info->entity_name[index] = strtok (NULL, " ");
  if (vsp_info->entity_name[index] == NULL) {
    GST_ERROR_OBJECT (space, "entity name not found. in %s", cap.card);
    return FALSE;
  }

  GST_DEBUG_OBJECT (space, "ENTITY NAME[%d] = %s",
      index, vsp_info->entity_name[index]);

  vsp_info->v4lsub_fd[index] = open_v4lsubdev (vsp_info->ip_name,
      (const char *) vsp_info->entity_name[index], path);
  if (vsp_info->v4lsub_fd[index] < 0) {
    GST_ERROR_OBJECT (space, "Cannot open '%s': %d, %s",
        path, errno, strerror (errno));
    return FALSE;
  }

  if (!(cap.capabilities & captype)) {
    GST_ERROR_OBJECT (space, "%s is not suitable device (%08x != %08x)",
        vsp_info->dev_name[index], cap.capabilities, captype);
    return FALSE;
  }

  if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
    GST_ERROR_OBJECT (space, "%s does not support streaming i/o",
        vsp_info->dev_name[index]);
    return FALSE;
  }

  vsp_info->already_device_initialized[index] = TRUE;

  GST_DEBUG_OBJECT (space, "Device initialization has suceeded");

  return TRUE;
}

static gint
open_device (GstVspFilter * space, gint index)
{
  GstVspFilterVspInfo *vsp_info;
  struct stat st;
  gint fd;
  const gchar *name;

  vsp_info = space->vsp_info;

  name = vsp_info->dev_name[index];

  if (-1 == stat (name, &st)) {
    GST_ERROR_OBJECT (space, "Cannot identify '%s': %d, %s",
        name, errno, strerror (errno));
    return -1;
  }

  if (!S_ISCHR (st.st_mode)) {
    GST_ERROR_OBJECT (space, "%s is no device", name);
    return -1;
  }

  fd = open (name, O_RDWR /* required | O_NONBLOCK */ , 0);

  if (-1 == fd) {
    GST_ERROR_OBJECT (space, "Cannot open '%s': %d, %s",
        name, errno, strerror (errno));
    return -1;
  }

  return fd;
}

static gboolean
gst_vsp_filter_vsp_device_init (GstVspFilter * space)
{
  GstVspFilterVspInfo *vsp_info;
  static const gchar *config_name = "gstvspfilter.conf";
  static const gchar *env_config_name = "GST_VSP_FILTER_CONFIG_DIR";
  gchar filename[256];
  gchar str[256];
  FILE *fp;

  vsp_info = space->vsp_info;

  /* Set the default path of gstvspfilter.conf */
  g_setenv (env_config_name, "/etc", FALSE);

  snprintf (filename, sizeof (filename), "%s/%s", g_getenv (env_config_name),
      config_name);

  GST_DEBUG_OBJECT (space, "Configuration scanning: read from %s", filename);

  fp = fopen (filename, "r");
  if (!fp) {
    GST_WARNING_OBJECT (space, "failed to read gstvspfilter.conf");
    goto skip_config;
  }

  while (fgets (str, sizeof (str), fp) != NULL) {
    if (strncmp (str, VSP_CONF_ITEM_INPUT, strlen (VSP_CONF_ITEM_INPUT)) == 0 &&
        !vsp_info->prop_dev_name[OUT]) {
      str[strlen (str) - 1] = '\0';
      g_free (vsp_info->dev_name[OUT]);
      vsp_info->dev_name[OUT] = g_strdup (str + strlen (VSP_CONF_ITEM_INPUT));
      continue;
    }

    if (strncmp (str, VSP_CONF_ITEM_OUTPUT, strlen (VSP_CONF_ITEM_OUTPUT)) == 0
        && !vsp_info->prop_dev_name[CAP]) {
      str[strlen (str) - 1] = '\0';
      g_free (vsp_info->dev_name[CAP]);
      vsp_info->dev_name[CAP] = g_strdup (str + strlen (VSP_CONF_ITEM_OUTPUT));
      continue;
    }
  }

  fclose (fp);

skip_config:
  GST_DEBUG_OBJECT (space, "input device=%s output device=%s",
      vsp_info->dev_name[OUT], vsp_info->dev_name[CAP]);

  vsp_info->v4lout_fd = open_device (space, OUT);
  vsp_info->v4lcap_fd = open_device (space, CAP);

  if (!init_device (space, vsp_info->v4lout_fd, OUT,
          V4L2_CAP_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)) {
    GST_ERROR_OBJECT (space, "init_device for %s failed",
        vsp_info->dev_name[OUT]);
    return FALSE;
  }

  if (!init_device (space, vsp_info->v4lcap_fd, CAP,
          V4L2_CAP_VIDEO_CAPTURE_MPLANE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)) {
    GST_ERROR_OBJECT (space, "init_device for %s failed",
        vsp_info->dev_name[CAP]);
    return FALSE;
  }

  vsp_info->media_fd = open_media_device (space);
  if (vsp_info->media_fd < 0) {
    GST_ERROR_OBJECT (space, "cannot open a media file for %s",
        vsp_info->ip_name);
    return FALSE;
  }

  return TRUE;
}

static void
gst_vsp_filter_vsp_device_deinit (GstVspFilter * space)
{
  GstVspFilterVspInfo *vsp_info;

  vsp_info = space->vsp_info;

  if (vsp_info->is_stream_started) {
    stop_capturing (space, vsp_info->v4lout_fd, OUT,
        V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    stop_capturing (space, vsp_info->v4lcap_fd, CAP,
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    vsp_info->is_stream_started = FALSE;
  }

  if (vsp_info->resz_subdev_fd >= 0) {
    close (vsp_info->resz_subdev_fd);
    vsp_info->resz_subdev_fd = -1;
  }

  close (vsp_info->v4lsub_fd[OUT]);
  close (vsp_info->v4lsub_fd[CAP]);

  close (vsp_info->media_fd);

  close_device (space, vsp_info->v4lout_fd, OUT);
  close_device (space, vsp_info->v4lcap_fd, CAP);

  g_free (vsp_info->ip_name);
  vsp_info->ip_name = NULL;

  vsp_info->already_device_initialized[OUT] =
      vsp_info->already_device_initialized[CAP] = FALSE;
}

static GstStateChangeReturn
gst_vsp_filter_change_state (GstElement * element, GstStateChange transition)
{
  GstVspFilter *space;
  GstStateChangeReturn ret;

  space = GST_VSP_FILTER_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_vsp_filter_vsp_device_init (space)) {
        GST_ERROR_OBJECT (space, "failed to initialize the vsp device");
        return GST_STATE_CHANGE_FAILURE;
      }
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_vsp_filter_vsp_device_deinit (space);
      break;
    default:
      break;
  }

  return ret;
}

/* configure the allocation query that was answered downstream, we can configure
 * some properties on it. Only called when not in passthrough mode. */
static gboolean
gst_vsp_filter_decide_allocation (GstBaseTransform * trans, GstQuery * query)
{
  GstVspFilter *space;
  GstBufferPool *pool = NULL;
  GstAllocator *allocator;
  guint n_allocators;
  guint dmabuf_pool_pos = 0;
  GstStructure *config;
  guint min, max, size;
  gboolean update_pool;
  guint i;

  space = GST_VSP_FILTER_CAST (trans);

  n_allocators = gst_query_get_n_allocation_params (query);
  for (i = 0; i < n_allocators; i++) {
    gst_query_parse_nth_allocation_param (query, i, &allocator, NULL);

    if (g_strcmp0 (allocator->mem_type, GST_ALLOCATOR_DMABUF) == 0) {
      GST_DEBUG_OBJECT (space, "found a dmabuf allocator");
      dmabuf_pool_pos = i;
      gst_object_unref (allocator);
      break;
    }

    gst_object_unref (allocator);
  }

  /* Delete buffer pools registered before the pool of dmabuf in
   * the buffer pool list so that the dmabuf allocator will be selected
   * by the parent class.
   */
  for (i = 0; i < dmabuf_pool_pos; i++)
    gst_query_remove_nth_allocation_param (query, i);

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

    update_pool = TRUE;
  } else {
    GstCaps *outcaps;
    GstVideoInfo vinfo;

    gst_query_parse_allocation (query, &outcaps, NULL);
    gst_video_info_init (&vinfo);
    gst_video_info_from_caps (&vinfo, outcaps);
    size = vinfo.size;
    min = max = 0;
    update_pool = FALSE;
  }

  if (!pool)
    pool = gst_video_buffer_pool_new ();

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_buffer_pool_set_config (pool, config);

  if (update_pool)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);

  gst_object_unref (pool);

  return GST_BASE_TRANSFORM_CLASS (parent_class)->decide_allocation (trans,
      query);
}

static GstFlowReturn
gst_vsp_filter_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstVideoFilter *filter = GST_VIDEO_FILTER_CAST (trans);
  GstMemory *in_gmem[GST_VIDEO_MAX_PLANES], *out_gmem[GST_VIDEO_MAX_PLANES];
  GstVideoFrame in_frame, out_frame;
  GstVspFilterFrameInfo in_vframe_info, out_vframe_info;
  GstVideoMeta *in_meta, *out_meta;
  gint in_stride[GST_VIDEO_MAX_PLANES] = { 0 };
  gint out_stride[GST_VIDEO_MAX_PLANES] = { 0 };
  GstFlowReturn ret;
  gint in_n_mem, out_n_mem;
  gint i;

  if (G_UNLIKELY (!filter->negotiated))
    goto unknown_format;

  in_meta = gst_buffer_get_video_meta (inbuf);
  out_meta = gst_buffer_get_video_meta (outbuf);

  in_n_mem = gst_buffer_n_memory (inbuf);
  out_n_mem = gst_buffer_n_memory (outbuf);

  for (i = 0; i < in_n_mem; i++) {
    in_gmem[i] = gst_buffer_get_memory (inbuf, i);

    /* Set row stride in bytes */
    if (in_meta)
      in_stride[i] = GST_VIDEO_FORMAT_INFO_STRIDE (filter->in_info.finfo,
          in_meta->stride, i);
  }

  for (i = 0; i < out_n_mem; i++) {
    out_gmem[i] = gst_buffer_get_memory (outbuf, i);

    /* Set row stride in bytes */
    if (out_meta)
      out_stride[i] = GST_VIDEO_FORMAT_INFO_STRIDE (filter->out_info.finfo,
          out_meta->stride, i);
  }

  if (gst_is_dmabuf_memory (in_gmem[0])) {
    in_vframe_info.io = V4L2_MEMORY_DMABUF;
    for (i = 0; i < in_n_mem; i++)
      in_vframe_info.vframe.dmafd[i] = gst_dmabuf_memory_get_fd (in_gmem[i]);
  } else {
    if (!gst_video_frame_map (&in_frame, &filter->in_info, inbuf, GST_MAP_READ))
      goto invalid_buffer;

    in_vframe_info.io = V4L2_MEMORY_USERPTR;
    in_vframe_info.vframe.frame = &in_frame;
  }

  if (gst_is_dmabuf_memory (out_gmem[0])) {
    out_vframe_info.io = V4L2_MEMORY_DMABUF;
    for (i = 0; i < out_n_mem; i++)
      out_vframe_info.vframe.dmafd[i] = gst_dmabuf_memory_get_fd (out_gmem[i]);
  } else {
    if (!gst_video_frame_map (&out_frame, &filter->out_info, outbuf,
            GST_MAP_WRITE))
      goto invalid_buffer;

    out_vframe_info.io = V4L2_MEMORY_USERPTR;
    out_vframe_info.vframe.frame = &out_frame;
  }

  ret =
      gst_vsp_filter_transform_frame_process (filter, &in_vframe_info,
      &out_vframe_info, in_stride, out_stride);

  if (!gst_is_dmabuf_memory (in_gmem[0]))
    gst_video_frame_unmap (&in_frame);
  if (!gst_is_dmabuf_memory (out_gmem[0]))
    gst_video_frame_unmap (&out_frame);

  for (i = 0; i < in_n_mem; i++)
    gst_memory_unref (in_gmem[i]);
  for (i = 0; i < out_n_mem; i++)
    gst_memory_unref (out_gmem[i]);

  return ret;

  /* ERRORS */
unknown_format:
  {
    GST_ELEMENT_ERROR (filter, CORE, NOT_IMPLEMENTED, (NULL),
        ("unknown format"));
    return GST_FLOW_NOT_NEGOTIATED;
  }
invalid_buffer:
  {
    GST_ELEMENT_WARNING (filter, CORE, NOT_IMPLEMENTED, (NULL),
        ("invalid video buffer received"));
    return GST_FLOW_OK;
  }
}

static gboolean
gst_vsp_filter_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstVideoFilter *filter = GST_VIDEO_FILTER_CAST (trans);
  GstVideoFilterClass *fclass;
  GstVideoInfo in_info, out_info;
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;

  space = GST_VSP_FILTER_CAST (filter);
  fclass = GST_VIDEO_FILTER_GET_CLASS (filter);
  vsp_info = space->vsp_info;

  /* input caps */
  if (!gst_video_info_from_caps (&in_info, incaps))
    goto invalid_caps;

  /* output caps */
  if (!gst_video_info_from_caps (&out_info, outcaps))
    goto invalid_caps;

  /* these must match */
  if (in_info.fps_n != out_info.fps_n || in_info.fps_d != out_info.fps_d)
    goto format_mismatch;

  /* if present, these must match too */
  if (in_info.interlace_mode != out_info.interlace_mode)
    goto format_mismatch;

  GST_DEBUG ("reconfigured %d %d", GST_VIDEO_INFO_FORMAT (&in_info),
      GST_VIDEO_INFO_FORMAT (&out_info));

  /* For the reinitialization of entities pipeline */
  vsp_info->already_setup_info = FALSE;
  if (vsp_info->is_stream_started) {
    stop_capturing (space, vsp_info->v4lout_fd, OUT,
        V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    stop_capturing (space, vsp_info->v4lcap_fd, CAP,
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
  }

  filter->in_info = in_info;
  filter->out_info = out_info;
  GST_BASE_TRANSFORM_CLASS (fclass)->transform_ip_on_passthrough = FALSE;
  filter->negotiated = TRUE;

  return TRUE;

  /* ERRORS */
invalid_caps:
  {
    GST_ERROR_OBJECT (space, "invalid caps");
    filter->negotiated = FALSE;
    return FALSE;
  }
format_mismatch:
  {
    GST_ERROR_OBJECT (space, "input and output formats do not match");
    filter->negotiated = FALSE;
    return FALSE;
  }
}

static void
gst_vsp_filter_class_init (GstVspFilterClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstBaseTransformClass *gstbasetransform_class =
      (GstBaseTransformClass *) klass;

  gobject_class->set_property = gst_vsp_filter_set_property;
  gobject_class->get_property = gst_vsp_filter_get_property;
  gobject_class->finalize = gst_vsp_filter_finalize;

  g_object_class_install_property (gobject_class, PROP_VSP_DEVFILE_INPUT,
      g_param_spec_string ("devfile-input",
          "Device File for Input", "VSP device filename for input port",
          DEFAULT_PROP_VSP_DEVFILE_INPUT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_VSP_DEVFILE_OUTPUT,
      g_param_spec_string ("devfile-output",
          "Device File for Output", "VSP device filename for output port",
          DEFAULT_PROP_VSP_DEVFILE_OUTPUT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_vsp_filter_src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_vsp_filter_sink_template));

  gst_element_class_set_static_metadata (gstelement_class,
      "Colorspace and Video Size Converter with VSP1 V4L2",
      "Filter/Converter/Video",
      "Converts colorspace and video size from one to another",
      "Renesas Electronics Corporation");

  gstelement_class->change_state = gst_vsp_filter_change_state;

  gstbasetransform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_vsp_filter_transform_caps);
  gstbasetransform_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_vsp_filter_fixate_caps);
  gstbasetransform_class->filter_meta =
      GST_DEBUG_FUNCPTR (gst_vsp_filter_filter_meta);
  gstbasetransform_class->transform_meta =
      GST_DEBUG_FUNCPTR (gst_vsp_filter_transform_meta);
  gstbasetransform_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_vsp_filter_decide_allocation);
  gstbasetransform_class->transform =
      GST_DEBUG_FUNCPTR (gst_vsp_filter_transform);
  gstbasetransform_class->set_caps =
      GST_DEBUG_FUNCPTR (gst_vsp_filter_set_caps);

  gstbasetransform_class->passthrough_on_same_caps = TRUE;
}

static void
gst_vsp_filter_init (GstVspFilter * space)
{
  GstVspFilterVspInfo *vsp_info;

  vsp_info = g_malloc0 (sizeof (GstVspFilterVspInfo));
  if (!vsp_info) {
    GST_ELEMENT_ERROR (space, RESOURCE, NO_SPACE_LEFT,
        ("Could not allocate vsp info"), ("Could not allocate vsp info"));
    return;
  }

  vsp_info->dev_name[OUT] = g_strdup (DEFAULT_PROP_VSP_DEVFILE_INPUT);
  vsp_info->dev_name[CAP] = g_strdup (DEFAULT_PROP_VSP_DEVFILE_OUTPUT);

  vsp_info->resz_subdev_fd = -1;

  space->vsp_info = vsp_info;
}

void
gst_vsp_filter_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;

  space = GST_VSP_FILTER (object);
  vsp_info = space->vsp_info;

  switch (property_id) {
    case PROP_VSP_DEVFILE_INPUT:
      if (vsp_info->dev_name[OUT])
        g_free (vsp_info->dev_name[OUT]);
      vsp_info->dev_name[OUT] = g_value_dup_string (value);
      vsp_info->prop_dev_name[OUT] = TRUE;
      break;
    case PROP_VSP_DEVFILE_OUTPUT:
      if (vsp_info->dev_name[CAP])
        g_free (vsp_info->dev_name[CAP]);
      vsp_info->dev_name[CAP] = g_value_dup_string (value);
      vsp_info->prop_dev_name[CAP] = TRUE;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_vsp_filter_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;

  space = GST_VSP_FILTER (object);
  vsp_info = space->vsp_info;

  switch (property_id) {
    case PROP_VSP_DEVFILE_INPUT:
      g_value_set_string (value, vsp_info->dev_name[OUT]);
      break;
    case PROP_VSP_DEVFILE_OUTPUT:
      g_value_set_string (value, vsp_info->dev_name[CAP]);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static gint
queue_buffer (GstVspFilter * space, int fd, int index,
    enum v4l2_buf_type buftype, struct v4l2_plane *planes,
    enum v4l2_memory io[MAX_DEVICES])
{
  GstVspFilterVspInfo *vsp_info;
  struct v4l2_buffer buf;

  vsp_info = space->vsp_info;

  CLEAR (buf);

  buf.type = buftype;
  buf.memory = io[index];
  buf.index = 0;
  buf.m.planes = planes;
  buf.length = vsp_info->n_planes[index];

  if (-1 == xioctl (fd, VIDIOC_QBUF, &buf)) {
    GST_ERROR_OBJECT (space,
        "V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE VIDIOC_QBUF failed errno=%d", errno);
    return -1;
  }

  return 0;
}

static gint
dequeue_buffer (GstVspFilter * space, int fd, int index,
    enum v4l2_buf_type buftype, struct v4l2_plane *planes,
    enum v4l2_memory io[MAX_DEVICES])
{
  GstVspFilterVspInfo *vsp_info;
  struct v4l2_buffer buf;

  vsp_info = space->vsp_info;

  CLEAR (buf);

  buf.type = buftype;
  buf.memory = io[index];
  buf.m.planes = planes;
  buf.length = vsp_info->n_planes[index];

  if (-1 == xioctl (fd, VIDIOC_DQBUF, &buf)) {
    GST_ERROR_OBJECT (space, "VIDIOC_DQBUF failed");
    return -1;
  }

  return 0;
}

static gboolean
start_capturing (GstVspFilter * space, int fd, int index,
    enum v4l2_buf_type buftype)
{
  if (-1 == xioctl (fd, VIDIOC_STREAMON, &buftype)) {
    GST_ERROR_OBJECT (space, "VIDIOC_STREAMON failed index=%d", index);
    return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_vsp_filter_transform_frame_process (GstVideoFilter * filter,
    GstVspFilterFrameInfo * in_vframe_info,
    GstVspFilterFrameInfo * out_vframe_info,
    gint in_stride[GST_VIDEO_MAX_PLANES], gint out_stride[GST_VIDEO_MAX_PLANES])
{
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;
  struct timeval tv;
  fd_set fds;
  gint ret;
  struct v4l2_plane in_planes[VIDEO_MAX_PLANES];
  struct v4l2_plane out_planes[VIDEO_MAX_PLANES];
  GstVideoInfo *in_info;
  GstVideoInfo *out_info;
  enum v4l2_memory io[MAX_DEVICES];
  gint i;

  memset (in_planes, 0, sizeof (in_planes));
  memset (out_planes, 0, sizeof (out_planes));

  space = GST_VSP_FILTER_CAST (filter);
  vsp_info = space->vsp_info;

  GST_CAT_DEBUG_OBJECT (GST_CAT_PERFORMANCE, filter,
      "doing colorspace conversion from %s -> to %s",
      GST_VIDEO_INFO_NAME (&filter->in_info),
      GST_VIDEO_INFO_NAME (&filter->out_info));

  in_info = &filter->in_info;
  out_info = &filter->out_info;

  io[OUT] = in_vframe_info->io;
  io[CAP] = out_vframe_info->io;

  if (!set_vsp_entities (space, in_info->finfo->format, in_info->width,
          in_info->height, in_stride, out_info->finfo->format, out_info->width,
          out_info->height, out_stride, io)) {
    GST_ERROR_OBJECT (space, "set_vsp_entities failed");
    return GST_FLOW_ERROR;
  }

  /* set up planes for queuing input buffers */
  for (i = 0; i < vsp_info->n_planes[OUT]; i++) {
    switch (in_vframe_info->io) {
    case V4L2_MEMORY_USERPTR:
      in_planes[i].m.userptr =
          (unsigned long) in_vframe_info->vframe.frame->data[i];
      break;
    case V4L2_MEMORY_DMABUF:
      in_planes[i].m.fd = in_vframe_info->vframe.dmafd[i];
      break;
    default:
      GST_ERROR_OBJECT (space, "unsupported V4L2 I/O method");
      return GST_FLOW_ERROR;
    }
    in_planes[i].length = in_stride[i] *
        GST_VIDEO_INFO_COMP_HEIGHT (in_info, i);
    in_planes[i].bytesused = in_planes[i].length;
  }

  /* set up planes for queuing output buffers */
  switch (out_vframe_info->io) {
    case V4L2_MEMORY_USERPTR:
      for (i = 0; i < vsp_info->n_planes[CAP]; i++) {
        out_planes[i].m.userptr =
            (unsigned long) out_vframe_info->vframe.frame->map[i].data;
        out_planes[i].length = out_vframe_info->vframe.frame->map[i].maxsize;
      }
      break;
    case V4L2_MEMORY_DMABUF:
      for (i = 0; i < vsp_info->n_planes[CAP]; i++) {
        out_planes[i].m.fd = out_vframe_info->vframe.dmafd[i];
        /* In the kernel space, the length (memory size) is obtained from
           the dmabuf descriptor when the length is specified as 0. */
        out_planes[i].length = 0;
        out_planes[i].data_offset = 0;
      }
      break;
    default:
      GST_ERROR_OBJECT (space, "unsupported V4L2 I/O method");
      return GST_FLOW_ERROR;
  }

  queue_buffer (space, vsp_info->v4lout_fd, OUT,
      V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, in_planes, io);
  queue_buffer (space, vsp_info->v4lcap_fd, CAP,
      V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, out_planes, io);

  if (!vsp_info->is_stream_started) {
    if (!start_capturing (space, vsp_info->v4lout_fd, OUT,
            V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)) {
      GST_ERROR_OBJECT (space, "start_capturing for %s failed",
          vsp_info->dev_name[OUT]);
      return GST_FLOW_ERROR;
    }
    if (!start_capturing (space, vsp_info->v4lcap_fd, CAP,
            V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)) {
      GST_ERROR_OBJECT (space, "start_capturing for %s failed",
          vsp_info->dev_name[CAP]);
      return GST_FLOW_ERROR;
    }
    vsp_info->is_stream_started = TRUE;
  }

  FD_ZERO (&fds);
  FD_SET (vsp_info->v4lcap_fd, &fds);

  /* Timeout. */
  tv.tv_sec = 2;
  tv.tv_usec = 0;

  do
    ret = select (vsp_info->v4lcap_fd + 1, &fds, NULL, NULL, &tv);
  while (ret == -1 && errno == EINTR);

  if (ret == 0) {
    GST_ERROR_OBJECT (space, "select timeout");
    return GST_FLOW_ERROR;
  } else if (ret == -1) {
    GST_ERROR_OBJECT (space, "select for cap");
    return GST_FLOW_ERROR;
  }

  dequeue_buffer (space, vsp_info->v4lcap_fd, CAP,
      V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, out_planes, io);
  dequeue_buffer (space, vsp_info->v4lout_fd, OUT,
      V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, in_planes, io);

  return GST_FLOW_OK;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (vspfilter_debug, "vspfilter", 0,
      "Colorspace and Video Size Converter");

  return gst_element_register (plugin, "vspfilter",
      GST_RANK_NONE, GST_TYPE_VSP_FILTER);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    vspfilter, "Colorspace conversion and Video scaling with VSP1 V4L2",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
