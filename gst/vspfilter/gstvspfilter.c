/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * This file:
 * Copyright (C) 2014-2018 Renesas Electronics Corporation
 * Based on videoconvert by Ronald Bultje <rbultje@ronald.bitfreak.net>
 *                          David Schleef <ds@schleef.org>
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
#include "vspfilterutils.h"
#include "vspfilterpool.h"

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
  PROP_INPUT_IO_MODE,
  PROP_OUTPUT_IO_MODE,
  PROP_INPUT_COLOR_RANGE
};

#define CSP_VIDEO_CAPS \
    "video/x-raw, " \
        "format = (string) {I420, NV12, NV21, NV16, UYVY, YUY2}," \
        "width = [ 1, 8190 ], " \
        "height = [ 1, 8190 ], " \
        "framerate = " GST_VIDEO_FPS_RANGE ";" \
    "video/x-raw, " \
        "format = (string) {RGB16, RGB, BGR, ARGB, xRGB, BGRA, BGRx}," \
        "width = [ 1, 8190 ], " \
        "height = [ 1, 8190 ], " \
        "framerate = " GST_VIDEO_FPS_RANGE

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

static GstFlowReturn
gst_vsp_filter_transform_frame_process (GstVideoFilter * filter,
    struct v4l2_buffer *in_v4l2_buf, struct v4l2_buffer *out_v4l2_buf);

static gboolean gst_vsp_filter_stop (GstBaseTransform * trans);

static gint
queue_buffer (GstVspFilter * space, GstVspFilterDeviceInfo * device,
    struct v4l2_buffer *buf);

static gboolean
start_capturing (GstVspFilter * space, GstVspFilterDeviceInfo * device);

static gint
dequeue_buffer (GstVspFilter * space, GstVspFilterDeviceInfo * device);
#define GST_TYPE_VSPFILTER_COLOR_RANGE (gst_vsp_filter_color_range_get_type ())
static GType
gst_vsp_filter_color_range_get_type (void)
{
  static GType vspfilter_color_range = 0;

  static const GEnumValue color_ranges[] = {
    {GST_VSPFILTER_AUTO_COLOR_RANGE,
        "GST_VSPFILTER_AUTO_COLOR_RANGE", "auto"},
    {GST_VSPFILTER_FULL_COLOR_RANGE,
        "GST_VSPFILTER_FULL_COLOR_RANGE", "full"},
    {GST_VSPFILTER_LIMITED_COLOR_RANGE,
        "GST_VSPFILTER_LIMITED_COLOR_RANGE", "limited"},
    {GST_VSPFILTER_DEFAULT_COLOR_RANGE,
        "GST_VSPFILTER_DEFAULT_COLOR_RANGE", "default"},
    {0, NULL, NULL}
  };
  vspfilter_color_range =
      g_enum_register_static ("GstVspfilterColorRange", color_ranges);

  return vspfilter_color_range;
}

#define GST_TYPE_VSPFILTER_IO_MODE (gst_vsp_filter_io_mode_get_type ())
static GType
gst_vsp_filter_io_mode_get_type (void)
{
  static GType vspfilter_io_mode = 0;

  if (!vspfilter_io_mode) {
    static const GEnumValue io_modes[] = {
      {GST_VSPFILTER_IO_AUTO, "GST_VSPFILTER_IO_AUTO", "auto (dmabuf or mmap)"},
      {GST_VSPFILTER_IO_USERPTR, "GST_VSPFILTER_IO_USERPTR", "userptr"},
      {0, NULL, NULL}
    };
    vspfilter_io_mode = g_enum_register_static ("GstVspfilterIOMode", io_modes);
  }
  return vspfilter_io_mode;
}

static gboolean
gst_vsp_filter_is_caps_format_supported_for_vsp (GstVspFilter * space,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstStructure *st[MAX_DEVICES];
  gint i;
  GstVspFilterVspInfo *vsp_info = space->vsp_info;

  if (direction == GST_PAD_SRC) {
    st[OUT_DEV] = gst_caps_get_structure (othercaps, 0);
    st[CAP_DEV] = gst_caps_get_structure (caps, 0);
  } else {
    st[OUT_DEV] = gst_caps_get_structure (caps, 0);
    st[CAP_DEV] = gst_caps_get_structure (othercaps, 0);
  }

  for (i = 0; i < MAX_DEVICES; i++) {
    GstVideoFormat fmt;
    gint ret;
    guint v4l2pix;
    gint w = 0, h = 0;
    struct v4l2_format v4l2fmt;

    fmt =
        gst_video_format_from_string (gst_structure_get_string (st[i],
            "format"));
    if (fmt == GST_VIDEO_FORMAT_UNKNOWN) {
      GST_ERROR_OBJECT (space, "failed to convert video format");
      return FALSE;
    }

    ret = set_colorspace (fmt, &v4l2pix, NULL, NULL);
    if (ret < 0) {
      GST_ERROR_OBJECT (space, "set_colorspace() failed");
      return FALSE;
    }

    gst_structure_get_int (st[i], "width", &w);
    gst_structure_get_int (st[i], "height", &h);

    CLEAR (v4l2fmt);
    v4l2fmt.type = space->devices[i].buftype;
    v4l2fmt.fmt.pix_mp.width = w;
    v4l2fmt.fmt.pix_mp.height = h;
    v4l2fmt.fmt.pix_mp.pixelformat = v4l2pix;
    v4l2fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

    ret = xioctl (space->devices[i].fd, VIDIOC_TRY_FMT, &v4l2fmt);
    if (ret < 0) {
      GST_ERROR_OBJECT (space,
          "VIDIOC_TRY_FMT failed. (%dx%d pixelformat=%d)", w, h, v4l2pix);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
intersect_format (GstCapsFeatures * features, GstStructure * structure,
    gpointer user_data)
{
  const GValue *in_format = user_data;
  GValue out_format = { 0 };

  if (!gst_value_intersect (&out_format, in_format,
          gst_structure_get_value (structure, "format"))) {
    return FALSE;
  }

  gst_structure_fixate_field_string (structure, "format",
      g_value_get_string (&out_format));

  g_value_unset (&out_format);

  return TRUE;
}

static GstCaps *
gst_vsp_filter_fixate_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstCaps *result;
  GstCaps *outcaps;
  gint from_w, from_h;
  gint w = 0, h = 0;
  GstStructure *ins, *outs;
  GstVspFilter *space;
  const GValue *in_format;

  space = GST_VSP_FILTER_CAST (trans);

  GST_DEBUG_OBJECT (trans, "caps %" GST_PTR_FORMAT, caps);
  GST_DEBUG_OBJECT (trans, "othercaps %" GST_PTR_FORMAT, othercaps);

  othercaps = gst_caps_make_writable (othercaps);

  ins = gst_caps_get_structure (caps, 0);
  in_format = gst_structure_get_value (ins, "format");

  outcaps = gst_caps_copy (othercaps);
  gst_caps_filter_and_map_in_place (outcaps, intersect_format,
      (gpointer) in_format);

  if (gst_caps_is_empty (outcaps))
    gst_caps_replace (&outcaps, othercaps);

  gst_caps_unref (othercaps);

  outcaps = gst_caps_truncate (outcaps);
  outs = gst_caps_get_structure (outcaps, 0);

  gst_structure_get_int (ins, "width", &from_w);
  gst_structure_get_int (ins, "height", &from_h);

  gst_structure_get_int (outs, "width", &w);
  gst_structure_get_int (outs, "height", &h);

  if (!w || !h) {
    gst_structure_fixate_field_nearest_int (outs, "height", from_h);
    gst_structure_fixate_field_nearest_int (outs, "width", from_w);
  }

  result = gst_caps_intersect (outcaps, caps);
  if (gst_caps_is_empty (result)) {
    gst_caps_unref (result);
    result = outcaps;
  } else {
    gst_caps_unref (outcaps);
  }

  /* fixate remaining fields */
  result = gst_caps_fixate (result);

  if (direction == GST_PAD_SINK) {
    GstVideoInfo in_info, out_info;

    gst_video_info_from_caps (&in_info, caps);
    gst_video_info_from_caps (&out_info, result);
    if (!w) {
      gint out_width = MIN (round_down_width (in_info.finfo, from_w),
          round_down_width (out_info.finfo, from_w));
      gst_caps_set_simple (result, "width", G_TYPE_INT, out_width, NULL);
    }
    if (!h) {
      gint out_height = MIN (round_down_height (in_info.finfo, from_h),
          round_down_height (out_info.finfo, from_h));
      gst_caps_set_simple (result, "height", G_TYPE_INT, out_height, NULL);
    }
  }
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

void
gst_vspfilter_set_colorimetry (GstCaps * caps, GstCaps * caps_intersected)
{
  gint i;
  gint n_struct;
  GstStructure *st_src, *st_dest;
  const GValue *src_fmt, *dest_fmt;
  const GValue *src_cimetry;

  n_struct = gst_caps_get_size (caps_intersected);

  if (!GST_CAPS_IS_SIMPLE (caps))
    return;

  st_src = gst_caps_get_structure (caps, 0);

  if (!gst_structure_has_field (st_src, "colorimetry"))
    return;

  src_fmt = gst_structure_get_value (st_src, "format");
  src_cimetry = gst_structure_get_value (st_src, "colorimetry");

  for (i = 0; i < n_struct; i++) {
    st_dest = gst_caps_get_structure (caps_intersected, i);
    dest_fmt = gst_structure_get_value (st_dest, "format");
    if (gst_value_is_fixed (src_cimetry) &&
        !gst_value_is_subset (src_fmt, dest_fmt)) {
      struct colorimetry *color = NULL;
      color = find_colorimetry (src_cimetry);
      if (color)
        gst_structure_set_value (st_dest, "colorimetry", &color->dest_value);
    }
  }
}

static gboolean
remove_format_info (GstCapsFeatures * features, GstStructure * structure,
    gpointer user_data)
{
  gst_structure_remove_fields (structure, "format",
      "colorimetry", "chroma-site", "width", "height", NULL);
}

/* The caps can be transformed into any other caps with format info removed.
 * However, we should prefer passthrough, so if passthrough is possible,
 * put it first in the list. */
static GstCaps *
gst_vsp_filter_transform_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *result;
  GstCaps *caps_copy;
  GstCaps *caps_intersected;
  GstCaps *template;
  gint i, n;

  caps_copy = gst_caps_copy (caps);

  gst_caps_map_in_place (caps_copy, remove_format_info, NULL);
  caps_copy = gst_caps_simplify (caps_copy);

  /*Src and sink templates are same */
  template = gst_static_pad_template_get_caps (&gst_vsp_filter_src_template);

  caps_intersected = gst_caps_intersect (caps_copy, template);
  gst_caps_unref (caps_copy);
  gst_caps_unref (template);

  if (direction == GST_PAD_SINK)
    gst_vspfilter_set_colorimetry (caps, caps_intersected);

  if (filter) {
    result = gst_caps_intersect_full (filter, caps_intersected,
        GST_CAPS_INTERSECT_FIRST);

    gst_caps_unref (caps_intersected);
  } else {
    result = caps_intersected;
  }

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
open_v4lsubdev (GstVspFilter * space, gchar * prefix, const gchar * target)
{
  gchar path[256];
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

  GST_ERROR_OBJECT (space, "Cannot open '%s': %d, %s",
      path, errno, strerror (errno));

  return -1;
}

static gint
get_symlink_target_name (gchar * filename, gchar ** link_target)
{
  struct stat lst;
  gint str_size = -1;

  if (lstat (filename, &lst) == -1)
    return -1;

  if ((lst.st_mode & S_IFMT) == S_IFLNK) {
    str_size = lst.st_size + 1;
    *link_target = g_slice_alloc0 (str_size);
    if (readlink (filename, *link_target, str_size) == -1) {
      g_slice_free1 (str_size, *link_target);
      return -1;
    }
  }

  return str_size;
}

static gint
open_media_device (GstVspFilter * space)
{
  GstVspFilterVspInfo *vsp_info;
  struct stat st;
  gchar path[256];
  gchar *dev;
  gchar *link_target = NULL;
  gint str_size;
  gint ret;
  gint i;

  vsp_info = space->vsp_info;

  str_size =
      get_symlink_target_name (space->devices[CAP_DEV].name, &link_target);

  dev =
      g_path_get_basename ((link_target) ? link_target :
      space->devices[CAP_DEV].name);

  for (i = 0; i < 256; i++) {
    snprintf (path, sizeof (path), "/sys/class/video4linux/%s/device/media%d",
        dev, i);
    if (0 == stat (path, &st)) {
      snprintf (path, sizeof (path), "/dev/media%d", i);
      GST_DEBUG_OBJECT (space, "media device = %s", path);
      ret = open (path, O_RDWR);
      goto leave;
    }
  }

  GST_ERROR_OBJECT (space, "No media device for %s", vsp_info->ip_name);

  ret = -1;

leave:
  if (link_target)
    g_slice_free1 (str_size, link_target);

  g_free (dev);

  return ret;
}

static void
get_media_entities (GstVspFilter * space)
{
  gint i, ret;
  struct media_entity_desc entity;
  struct media_entity_desc *ent_d;

  for (i = 0; i < 256; i++) {
    CLEAR (entity);
    entity.id = i | MEDIA_ENT_ID_FLAG_NEXT;
    ret = ioctl (space->vsp_info->media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity);
    if ((ret < 0) && (errno == EINVAL))
      break;

    ent_d = g_memdup (&entity, sizeof (struct media_entity_desc));
    g_hash_table_insert (space->hash_t, ent_d->name, ent_d);
  }
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
  links.pads = g_malloc0 (sizeof (struct media_pad_desc) * src->pads);
  links.links = g_malloc0 (sizeof (struct media_link_desc) * src->links);

  links.entity = src->id;
  ret = ioctl (vsp_info->media_fd, MEDIA_IOC_ENUM_LINKS, &links);
  if (ret) {
    GST_ERROR_OBJECT (space, "MEDIA_IOC_ENUM_LINKS failed");
    goto leave;
  }

  for (i = 0; i < src->links; i++) {
    if (links.links[i].sink.entity == sink->id) {
      target_link = &links.links[i];
    } else if (links.links[i].flags & MEDIA_LNK_FL_ENABLED) {
      GST_WARNING_OBJECT (space, "An active link to %02x found.",
          links.links[i].sink.entity);
      ret = -1;
      goto leave;
    }
  }

  if (!target_link) {
    ret = -1;
    goto leave;
  }

  target_link->flags |= MEDIA_LNK_FL_ENABLED;
  ret = ioctl (vsp_info->media_fd, MEDIA_IOC_SETUP_LINK, target_link);

leave:
  g_free (links.pads);
  g_free (links.links);

  return ret;
}

static gint
deactivate_link (GstVspFilter * space, struct media_entity_desc *src)
{
  struct media_links_enum links;
  struct media_link_desc *target_link;
  gint ret, i;
  GstVspFilterVspInfo *vsp_info;

  vsp_info = space->vsp_info;

  CLEAR (links);
  links.pads = g_malloc0 (sizeof (struct media_pad_desc) * src->pads);
  links.links = g_malloc0 (sizeof (struct media_link_desc) * src->links);

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
link_entities (GstVspFilter * space, GstVspFilterEntityInfo * out_ent,
    GstVspFilterEntityInfo * cap_ent)
{
  if (activate_link (space, &out_ent->entity, &cap_ent->entity)) {
    GST_ERROR_OBJECT (space, "Cannot enable a link from %s to %s",
        out_ent->name, cap_ent->name);
    return FALSE;
  }

  GST_DEBUG_OBJECT (space, "A link from %s to %s enabled.",
      out_ent->name, cap_ent->name);

  return TRUE;
}

static gboolean
set_crop (GstVspFilter * space, gint fd, guint * width, guint * height)
{
  struct v4l2_subdev_selection sel = {
    .pad = 0,
    .which = V4L2_SUBDEV_FORMAT_ACTIVE,
    .target = V4L2_SEL_TGT_CROP,
    .flags = V4L2_SEL_FLAG_LE,
  };

  sel.r.top = 0;
  sel.r.left = 0;
  sel.r.width = *width;
  sel.r.height = *height;

  if (-1 == xioctl (fd, VIDIOC_SUBDEV_S_SELECTION, &sel)) {
    GST_ERROR_OBJECT (space, "V4L2_SEL_TGT_CROP failed.");
    return FALSE;
  }

  /* crop size may have changed */
  *width = sel.r.width;
  *height = sel.r.height;

  return TRUE;
}

static gboolean
init_entity_pad (GstVspFilter * space, gint fd, guint pad,
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
    GST_ERROR_OBJECT (space, "VIDIOC_SUBDEV_S_FMT failed");
    return FALSE;
  }

  return TRUE;
}

static gboolean
set_vsp_entity (GstVspFilter * space, GstVspFilterEntityInfo * ventity,
    gint sink_width, gint sink_height, gint src_width, gint src_height)
{
  if (!init_entity_pad (space, ventity->fd, SINK, sink_width,
          sink_height, ventity->code[SINK])) {
    GST_ERROR_OBJECT (space, "init_entity_pad for %s failed", ventity->name);
    return FALSE;
  }

  if (!init_entity_pad (space, ventity->fd, SRC, src_width,
          src_height, ventity->code[SRC])) {
    GST_ERROR_OBJECT (space, "init_entity_pad for %s failed", ventity->name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
lookup_entity (GstVspFilter * space, gchar * ent_name,
    struct media_entity_desc *ent)
{
  gchar tmp[256];
  struct media_entity_desc *ent_t;

  snprintf (tmp, sizeof (tmp), "%s %s", space->vsp_info->ip_name, ent_name);

  ent_t = (struct media_entity_desc *) g_hash_table_lookup (space->hash_t, tmp);

  if (!ent_t)
    return FALSE;

  *ent = *ent_t;

  return TRUE;
}

static gboolean
init_resize_device (GstVspFilter * space, GstVspFilterEntityInfo * resz_ent)
{
  GstVspFilterVspInfo *vsp_info = space->vsp_info;

  resz_ent->name = RESIZE_DEVICE_NAME;
  /* input src code is always the same as output sink code */
  resz_ent->code[SINK] = resz_ent->code[SRC]
      = space->devices[CAP_DEV].ventity.code[SINK];

  resz_ent->fd = open_v4lsubdev (space, vsp_info->ip_name, resz_ent->name);
  if (resz_ent->fd < 0) {
    GST_ERROR_OBJECT (space, "cannot open a subdev file for %s",
        resz_ent->name);
    return FALSE;
  }

  if (!lookup_entity (space, resz_ent->name, &resz_ent->entity)) {
    GST_ERROR_OBJECT (space, "lookup_entity for %s failed", resz_ent->name);
    return FALSE;
  }

  space->vsp_info->is_resz_device_initialized = TRUE;

  return TRUE;
}

static void
deinit_resize_device (GstVspFilter * space)
{
  GstVspFilterEntityInfo *resz_ent = &space->vsp_info->resz_ventity;

  close (resz_ent->fd);
  resz_ent->fd = -1;

  space->vsp_info->is_resz_device_initialized = FALSE;
}

static gboolean
setup_resize_device (GstVspFilter * space, GstVspFilterEntityInfo * out_ent,
    GstVspFilterEntityInfo * cap_ent, guint in_src_width, guint in_src_height,
    guint out_width, guint out_height)
{
  GstVspFilterEntityInfo *resz_ent = &space->vsp_info->resz_ventity;
  gint ret;

  if (!space->vsp_info->is_resz_device_initialized &&
      !init_resize_device (space, resz_ent)) {
    GST_ERROR_OBJECT (space, "Cannot init resize entity");
    return FALSE;
  }

  if (!link_entities (space, out_ent, resz_ent))
    return FALSE;

  if (!link_entities (space, resz_ent, cap_ent))
    return FALSE;

  if (!set_vsp_entity (space, resz_ent, in_src_width,
          in_src_height, out_width, out_height)) {
    GST_ERROR_OBJECT (space, "Failed to set_vsp_entity for %s", resz_ent->name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
set_vsp_entities (GstVspFilter * space, GstVideoInfo * in_info,
    GstVideoInfo * out_info)
{
  GstVspFilterVspInfo *vsp_info;
  const GstVideoFormatInfo *in_finfo;
  gint ret;
  guint n_bufs;
  gint in_width, in_height, out_width, out_height;
  guint in_sink_width, in_sink_height;
  guint in_src_width, in_src_height;
  GstVspFilterEntityInfo *out_ent, *cap_ent;
  struct v4l2_format fmt;

  vsp_info = space->vsp_info;

  in_width = in_info->width;
  in_height = in_info->height;
  out_width = out_info->width;
  out_height = out_info->height;

  in_finfo = gst_video_format_get_info (in_info->finfo->format);

  out_ent = &space->devices[OUT_DEV].ventity;
  cap_ent = &space->devices[CAP_DEV].ventity;

  CLEAR (fmt);

  fmt.type = space->devices[OUT_DEV].buftype;

  if (-1 == xioctl (space->devices[OUT_DEV].fd, VIDIOC_G_FMT, &fmt)) {
    GST_ERROR ("VIDIOC_G_FMT for %s failed.",
        buftype_str (space->devices[OUT_DEV].buftype));
    return FALSE;
  }

  in_sink_width = fmt.fmt.pix_mp.width;
  in_sink_height = fmt.fmt.pix_mp.height;

  /*in case odd size of yuv buffer, separate buffer and image size */
  in_src_width = round_down_width (in_finfo, in_width);
  in_src_height = round_down_height (in_finfo, in_height);

  if (!set_vsp_entity (space, out_ent, in_sink_width,
          in_sink_height, in_src_width, in_src_height)) {
    GST_ERROR_OBJECT (space, "Failed to set_vsp_entity for %s", out_ent->name);
    return FALSE;
  }

  if ((in_sink_width != in_src_width || in_sink_height != in_src_height) &&
      !set_crop (space, out_ent->fd, &in_src_width, &in_src_height)) {
    GST_ERROR_OBJECT (space, "needs crop but set_crop failed");
    return FALSE;
  }

  if (!set_vsp_entity (space, cap_ent, out_width,
          out_height, out_width, out_height)) {
    GST_ERROR_OBJECT (space, "Failed to set_vsp_entity for %s", cap_ent->name);
    return FALSE;
  }

  /* Deactivate the current pipeline. */
  deactivate_link (space, &out_ent->entity);

  /* link up entities for VSP1 V4L2 */
  if ((in_src_width != out_width) || (in_src_height != out_height)) {
    if (!setup_resize_device (space, out_ent, cap_ent, in_src_width,
            in_src_height, out_width, out_height))
      return FALSE;
  } else {
    if (vsp_info->is_resz_device_initialized)
      deinit_resize_device (space);

    if (!link_entities (space, out_ent, cap_ent))
      return FALSE;
  }

  return TRUE;
}

static gboolean
stop_capturing (GstVspFilter * space, GstVspFilterDeviceInfo * device)
{
  GstVspFilterVspInfo *vsp_info;

  vsp_info = space->vsp_info;

  GST_DEBUG_OBJECT (space, "stop streaming... ");

  if (-1 == xioctl (device->fd, VIDIOC_STREAMOFF, &device->buftype)) {
    GST_ERROR_OBJECT (space, "VIDIOC_STREAMOFF for %s failed", device->name);
    return FALSE;
  }

  return TRUE;
}

static void
gst_vsp_filter_finalize (GObject * obj)
{
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;

  space = GST_VSP_FILTER (obj);
  vsp_info = space->vsp_info;

  g_free (space->vsp_info);

  g_hash_table_destroy (space->hash_t);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static gboolean
init_device (GstVspFilter * space, GstVspFilterDeviceInfo * device)
{
  GstVspFilterVspInfo *vsp_info;
  struct v4l2_capability cap;
  gchar *p;
  gint fd = device->fd;
  GstVspFilterEntityInfo *ventity = &device->ventity;
  enum v4l2_buf_type buftype = device->buftype;

  vsp_info = space->vsp_info;

  if (-1 == xioctl (fd, VIDIOC_QUERYCAP, &cap)) {
    GST_ERROR_OBJECT (space, "VIDIOC_QUERYCAP for %s errno=%d",
        buftype_str (buftype), errno);
    return FALSE;
  }

  if (!(cap.capabilities & device->captype)) {
    GST_ERROR_OBJECT (space, "not suitable device (%08x != %08x)",
        cap.capabilities, device->captype);
    return FALSE;
  }

  if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
    GST_ERROR_OBJECT (space, "does not support streaming i/o");
    return FALSE;
  }

  /* look for a counterpart */
  p = strtok (cap.card, " ");
  if (vsp_info->ip_name == NULL) {
    vsp_info->ip_name = g_strdup (p);
    GST_DEBUG_OBJECT (space, "ip_name = %s", vsp_info->ip_name);
  } else if (strcmp (vsp_info->ip_name, p) != 0) {
    GST_ERROR_OBJECT (space, "ip name mismatch vsp_info->ip_name=%s p=%s",
        vsp_info->ip_name, p);
    return FALSE;
  }

  ventity->name = g_strdup (strtok (NULL, " "));
  if (ventity->name == NULL) {
    GST_ERROR_OBJECT (space, "entity name not found. in %s", cap.card);
    return FALSE;
  }

  if (!lookup_entity (space, ventity->name, &ventity->entity)) {
    GST_ERROR_OBJECT (space, "lookup_entity for %s failed", ventity->name);
    return FALSE;
  }

  GST_DEBUG_OBJECT (space, "ENTITY NAME = %s", ventity->name);

  ventity->fd = open_v4lsubdev (space, vsp_info->ip_name,
      (const char *) ventity->name);
  if (ventity->fd < 0) {
    GST_ERROR_OBJECT (space, "cannot open a subdev file for %s", ventity->name);
    return FALSE;
  }

  GST_DEBUG_OBJECT (space, "Device initialization has suceeded");

  return TRUE;
}

static gint
open_device (GstVspFilter * space, gchar * dev_name)
{
  GstVspFilterVspInfo *vsp_info;
  struct stat st;
  gint fd;
  const gchar *name;

  vsp_info = space->vsp_info;

  name = dev_name;

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
start_device (GstVspFilter * space, GstVspFilterDeviceInfo * device)
{
  device->fd = open_device (space, device->name);

  if (!init_device (space, device)) {
    GST_ERROR_OBJECT (space, "init_device for %s failed", device->name);
    return FALSE;
  }

  return TRUE;
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
  gint i;

  vsp_info = space->vsp_info;

  /* Set the default path of gstvspfilter.conf */
  g_setenv (env_config_name, "/etc", FALSE);

  snprintf (filename, sizeof (filename), "%s/%s", g_getenv (env_config_name),
      config_name);

  GST_DEBUG_OBJECT (space, "Configuration scanning: read from %s", filename);

  fp = fopen (filename, "r");
  if (!fp) {
    GST_WARNING_OBJECT (space, "failed to read gstvspfilter.conf");
  } else {
    while (fgets (str, sizeof (str), fp) != NULL) {
      if (strncmp (str, VSP_CONF_ITEM_INPUT,
              strlen (VSP_CONF_ITEM_INPUT)) == 0 &&
          !space->devices[OUT_DEV].prop_name) {
        /* remove line feed code */
        str[strlen (str) - 1] = '\0';
        /* need to free default name */
        g_free (space->devices[OUT_DEV].name);
        space->devices[OUT_DEV].name =
            g_strdup (str + strlen (VSP_CONF_ITEM_INPUT));
        continue;
      }

      if (strncmp (str, VSP_CONF_ITEM_OUTPUT,
              strlen (VSP_CONF_ITEM_OUTPUT)) == 0
          && !space->devices[CAP_DEV].prop_name) {
        /* remove line feed code */
        str[strlen (str) - 1] = '\0';
        /* need to free default name */
        g_free (space->devices[CAP_DEV].name);
        space->devices[CAP_DEV].name =
            g_strdup (str + strlen (VSP_CONF_ITEM_OUTPUT));
        continue;
      }
    }

    fclose (fp);
  }

  GST_DEBUG_OBJECT (space, "input device=%s output device=%s",
      space->devices[OUT_DEV].name, space->devices[CAP_DEV].name);

  vsp_info->media_fd = open_media_device (space);
  if (vsp_info->media_fd < 0) {
    GST_ERROR_OBJECT (space, "cannot open a media file for %s",
        vsp_info->ip_name);
    return FALSE;
  }

  get_media_entities (space);

  for (i = 0; i < MAX_DEVICES; i++) {
    if (!start_device (space, &space->devices[i])) {
      GST_ERROR_OBJECT (space, "init_device for %s failed",
          space->devices[i].name);
      return FALSE;
    }
  }

  return TRUE;
}

static void
gst_vsp_filter_vsp_device_deinit (GstVspFilter * space)
{
  GstVspFilterVspInfo *vsp_info = space->vsp_info;
  gint i;

  for (i = 0; i < MAX_DEVICES; i++) {
    if (vsp_info->is_stream_started)
      stop_capturing (space, &space->devices[i]);

    close (space->devices[i].ventity.fd);
    close (space->devices[i].fd);

    g_free (space->devices[i].ventity.name);
    g_free (space->devices[i].name);
  }

  vsp_info->is_stream_started = FALSE;

  if (vsp_info->resz_ventity.fd >= 0) {
    close (vsp_info->resz_ventity.fd);
    vsp_info->resz_ventity.fd = -1;
  }

  vsp_info->is_resz_device_initialized = FALSE;

  g_free (vsp_info->ip_name);
  vsp_info->ip_name = NULL;

  close (vsp_info->media_fd);
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
      g_clear_object (&space->devices[OUT_DEV].pool);
      g_clear_object (&space->devices[CAP_DEV].pool);
      gst_vsp_filter_vsp_device_deinit (space);
      break;
    default:
      break;
  }

  return ret;
}

static GstBufferPool *
gst_vsp_filter_setup_pool (GstVspFilterDeviceInfo * device, GstCaps * caps,
    gsize size, guint num_buf)
{
  GstBufferPool *pool;
  GstStructure *structure;
  guint buf_cnt = MAX (3, num_buf);

  pool = vspfilter_buffer_pool_new (device->fd, device->buftype);

  structure = gst_buffer_pool_get_config (pool);
  /*We don't support dynamically allocating buffers, so set the max buffer
     count to be the same as the min buffer count */
  gst_buffer_pool_config_set_params (structure, caps, size, buf_cnt, buf_cnt);
  if (!gst_buffer_pool_set_config (pool, structure)) {
    gst_object_unref (pool);
    return NULL;
  }

  return pool;
}

/* configure the allocation query that was answered downstream, we can configure
 * some properties on it. Only called when not in passthrough mode. */
static gboolean
gst_vsp_filter_decide_allocation (GstBaseTransform * trans, GstQuery * query)
{
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;
  GstBufferPool *pool = NULL;
  GstAllocator *allocator;
  guint n_allocators;
  guint n_pools;
  guint dmabuf_pool_pos = 0;
  gboolean have_dmabuf = FALSE;
  GstStructure *config;
  guint min = 0;
  guint max = 0;
  guint size = 0;
  guint i;

  space = GST_VSP_FILTER_CAST (trans);
  vsp_info = space->vsp_info;

  n_allocators = gst_query_get_n_allocation_params (query);
  for (i = 0; i < n_allocators; i++) {
    gst_query_parse_nth_allocation_param (query, i, &allocator, NULL);

    if (!allocator)
      continue;

    if (g_strcmp0 (allocator->mem_type, GST_ALLOCATOR_DMABUF) == 0) {
      GST_DEBUG_OBJECT (space, "found a dmabuf allocator");
      dmabuf_pool_pos = i;
      have_dmabuf = TRUE;
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

  n_pools = gst_query_get_n_allocation_pools (query);
  if (n_pools > 0)
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

  if (space->devices[CAP_DEV].io_mode == GST_VSPFILTER_IO_AUTO &&
      !have_dmabuf && !space->devices[CAP_DEV].pool) {
    GstCaps *caps;
    GstVideoInfo vinfo;

    gst_query_parse_allocation (query, &caps, NULL);
    gst_video_info_init (&vinfo);
    gst_video_info_from_caps (&vinfo, caps);

    GST_DEBUG_OBJECT (space, "create new pool, min buffers=%d, max buffers=%d",
        min, max);
    size = MAX (vinfo.size, size);
    space->devices[CAP_DEV].pool =
        gst_vsp_filter_setup_pool (&space->devices[CAP_DEV], caps, size, min);
    if (!space->devices[CAP_DEV].pool) {
      GST_ERROR_OBJECT (space, "failed to setup pool");
      return FALSE;
    }
  }

  if (space->devices[CAP_DEV].pool) {
    gst_object_replace ((GstObject **) & pool,
        (GstObject *) space->devices[CAP_DEV].pool);
    GST_DEBUG_OBJECT (space, "use our pool %p", pool);
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_params (config, NULL, &size, &min, &max);
    gst_structure_free (config);
  }

  /* We need a bufferpool for userptr. */
  if (!pool) {
    GST_ERROR_OBJECT (space, "no pool");
    return FALSE;
  }

  if (pool != space->devices[CAP_DEV].pool) {
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
    gst_buffer_pool_set_config (pool, config);
  }

  if (n_pools > 0)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);

  gst_object_unref (pool);

  return GST_BASE_TRANSFORM_CLASS (parent_class)->decide_allocation (trans,
      query);
}

static inline gint
get_stride (GstBuffer * buffer, GstVideoInfo * vinfo, gint plane_index)
{
  GstVideoMeta *meta;

  meta = gst_buffer_get_video_meta (buffer);
  return (meta) ?
      GST_VIDEO_FORMAT_INFO_STRIDE (vinfo->finfo, meta->stride, plane_index) :
      vinfo->stride[plane_index];
}

static void
gst_vsp_filter_copy_frame (GstVideoFrame * dest_frame,
    GstVideoFrame * src_frame, GstVideoInfo * vinfo)
{
  gint width, height;
  guint8 *sp, *dp;
  gint ss, ds;
  gint i, j;

  for (i = 0; i < GST_VIDEO_FRAME_N_PLANES (src_frame); i++) {
    sp = src_frame->data[i];
    dp = dest_frame->data[i];

    width = GST_VIDEO_FRAME_COMP_WIDTH (dest_frame, i) *
        GST_VIDEO_FRAME_COMP_PSTRIDE (dest_frame, i);
    height = GST_VIDEO_FRAME_COMP_HEIGHT (dest_frame, i);

    ss = get_stride (src_frame->buffer, vinfo, i);
    ds = get_stride (dest_frame->buffer, vinfo, i);

    for (j = 0; j < height; j++) {
      memcpy (dp, sp, width);
      dp += ds;
      sp += ss;
    }
  }
}

static void
set_v4l2_buf_mmap (struct v4l2_buffer *v4l2_buf, GstBuffer * buffer)
{
  v4l2_buf->memory = V4L2_MEMORY_MMAP;
  v4l2_buf->index = vspfilter_buffer_pool_get_buffer_index (buffer);
}

static void
set_v4l2_input_plane_mmap (GstBuffer * buffer, guint n_planes,
    struct v4l2_plane *planes)
{
  gint i;
  gint *size = vspfilter_buffer_pool_get_size (buffer->pool);

  for (i = 0; i < n_planes; i++)
    planes[i].length = planes[i].bytesused = size[i];
}

static void
set_v4l2_input_plane_dmabuf (GstVideoInfo * vinfo, struct v4l2_plane *planes,
    guint n_planes, guint * strides)
{
  guint height;
  gint i;

  /* When import dmabuf, device size setting can be rounded down */
  height = round_down_height (vinfo->finfo, vinfo->height);

  /* set up planes for queuing input buffers */
  for (i = 0; i < n_planes; i++) {
    guint plane_height;

    plane_height =
        GST_VIDEO_SUB_SCALE (GST_VIDEO_FORMAT_INFO_H_SUB (vinfo->finfo, i),
        height);

    planes[i].length = strides[i] * plane_height;
    planes[i].bytesused = planes[i].length;
  }
}

static void
set_v4l2_buf (struct v4l2_buffer *v4l2_buf, GstVspFilterDeviceInfo * device)
{
  v4l2_buf->length = device->n_planes;
  v4l2_buf->type = device->buftype;
}

static GstFlowReturn
copy_frame (GstVspFilter * space, GstBufferPool * pool, GstBuffer * src,
    GstBuffer ** dst, GstVideoInfo * vinfo, GstVideoFrame * dest_frame)
{
  GstFlowReturn ret;
  GstVideoFrame frame;

  /* Only input buffers can pass this route. */
  GST_LOG_OBJECT (space, "Copy buffer %p to MMAP memory", src);

  if (!gst_buffer_pool_set_active (pool, TRUE))
    goto activate_failed;

  /* Get a buffer from our MMAP buffer pool */
  ret = gst_buffer_pool_acquire_buffer (pool, dst, NULL);
  if (ret != GST_FLOW_OK)
    goto no_buffer;

  if (!gst_video_frame_map (&frame, vinfo, src, GST_MAP_READ))
    goto invalid_buffer;
  if (!gst_video_frame_map (dest_frame, vinfo, *dst, GST_MAP_WRITE)) {
    gst_video_frame_unmap (&frame);
    goto invalid_buffer;
  }
  gst_buffer_unref (*dst);

  gst_vsp_filter_copy_frame (dest_frame, &frame, vinfo);
  gst_video_frame_unmap (&frame);

  return GST_FLOW_OK;

activate_failed:
  {
    GST_ERROR_OBJECT (space, "Failed to activate bufferpool");
    return GST_FLOW_ERROR;
  }
no_buffer:
  {
    GST_ERROR_OBJECT (space, "Could not acquire a buffer from our pool");
    return ret;
  }
invalid_buffer:
  {
    GST_ELEMENT_WARNING (space, CORE, NOT_IMPLEMENTED, (NULL),
        ("invalid video buffer received"));
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
prepare_transform_device_copy (GstVspFilter * space, GstBuffer * src,
    GstBufferPool * pool, GstVideoInfo * vinfo, GstBuffer ** dst,
    GstVideoFrame * dest_frame, struct v4l2_buffer *v4l2_buf)
{
  GstFlowReturn ret;
  guint strides[VIDEO_MAX_PLANES];
  gint i;

  ret = copy_frame (space, pool, src, dst, vinfo, dest_frame);
  if (ret != GST_FLOW_OK)
    return ret;

  set_v4l2_buf_mmap (v4l2_buf, *dst);

  return GST_FLOW_OK;
}

static void
set_v4l2_buf_dmabuf (GstVspFilter * space, GstBuffer * buffer,
    struct v4l2_buffer *v4l2_buf)
{
  gint i;
  gint n_mem = gst_buffer_n_memory (buffer);

  for (i = 0; i < n_mem; i++) {
    GstMemory *mem = gst_buffer_get_memory (buffer, i);

    v4l2_buf->m.planes[i].m.fd = gst_dmabuf_memory_get_fd (mem);
    v4l2_buf->m.planes[i].data_offset += mem->offset;
    gst_memory_unref (mem);
  }

  v4l2_buf->memory = V4L2_MEMORY_DMABUF;
}

static gboolean
setup_device (GstVspFilter * space, GstBufferPool * pool, GstVideoInfo * vinfo,
    GstVspFilterDeviceInfo * device, gint stride[VIDEO_MAX_PLANES],
    enum v4l2_memory io)
{
  enum v4l2_quantization quant;
  guint n_bufs = N_BUFFERS;
  guint width, height;

  if (device->is_input_device &&
      space->input_color_range != GST_VSPFILTER_DEFAULT_COLOR_RANGE)
    quant = space->input_color_range;
  else
    quant = set_quantization (vinfo->colorimetry.range);

  /* When import external buffer, device size setting can be rounded down */
  if (device->is_input_device) {
    width = round_down_width (vinfo->finfo, vinfo->width);
    height = round_down_height (vinfo->finfo, vinfo->height);
  } else {
    width = vinfo->width;
    height = vinfo->height;
  }

  if (!set_format (device->fd, width, height, device->format, stride, NULL,
          device->buftype, io, set_encoding (vinfo->colorimetry.matrix),
          quant)) {
    GST_ERROR_OBJECT (space, "set_format for %s failed (%dx%d)",
        buftype_str (device->buftype), width, height);
    return FALSE;
  }

  if (!request_buffers (device->fd, device->buftype, &n_bufs, io)) {
    GST_ERROR_OBJECT (space, "request_buffers for %s failed.", device->name);
    return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
prepare_transform_device_userptr (GstVspFilter * space,
    GstBuffer * buffer, GstVideoInfo * vinfo,
    guint n_planes, GstVideoFrame * dest_frame, struct v4l2_buffer *v4l2_buf)
{
  gint i;

  if (!gst_video_frame_map (dest_frame, vinfo, buffer, GST_MAP_READ)) {
    GST_ERROR_OBJECT (space, "Failed to gst_video_frame_map");
    return GST_FLOW_ERROR;
  }

  v4l2_buf->memory = V4L2_MEMORY_USERPTR;

  return GST_FLOW_OK;
}

static gboolean
get_offset_from_meta (GstVspFilter * space, GstBuffer * buffer,
    GstVideoMeta * vmeta, struct v4l2_plane *planes)
{
  gint i;
  gint n_mem = gst_buffer_n_memory (buffer);

  for (i = 0; i < n_mem; i++) {
    guint mem_idx, length;
    gsize skip;

    if (gst_buffer_find_memory (buffer, vmeta->offset[i],
            1, &mem_idx, &length, &skip)) {
      planes[i].data_offset += skip;
    } else {
      GST_ERROR_OBJECT (space, "buffer meta is invalid");
      return FALSE;
    }
  }

  return TRUE;
}

static GstFlowReturn
prepare_transform_device (GstVspFilter * space,
    GstBuffer * buffer, GstBufferPool * pool, struct v4l2_buffer *v4l2_buf)
{
  GstMemory *mem = gst_buffer_peek_memory (buffer, 0);

  if (buffer->pool == pool)
    set_v4l2_buf_mmap (v4l2_buf, buffer);
  else if (gst_is_dmabuf_memory (mem))
    set_v4l2_buf_dmabuf (space, buffer, v4l2_buf);
  else
    g_assert_not_reached ();

  return GST_FLOW_OK;
}

static GstFlowReturn
wait_output_ready (GstVspFilter * space)
{
  struct timeval tv;
  fd_set fds;
  gint ret;

  FD_ZERO (&fds);
  FD_SET (space->devices[CAP_DEV].fd, &fds);

  /* Timeout. */
  tv.tv_sec = 2;
  tv.tv_usec = 0;

  do
    ret = select (space->devices[CAP_DEV].fd + 1, &fds, NULL, NULL, &tv);
  while (ret == -1 && errno == EINTR);

  if (ret == 0) {
    GST_ERROR_OBJECT (space, "select timeout");
    return GST_FLOW_ERROR;
  } else if (ret == -1) {
    GST_ERROR_OBJECT (space, "select for cap");
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static gboolean
start_transform_device (GstVspFilter * space, GstBufferPool * pool,
    GstVspFilterDeviceInfo * device, struct v4l2_buffer *v4l2_buf)
{
  set_v4l2_buf (v4l2_buf, device);

  if (queue_buffer (space, device, v4l2_buf)) {
    GST_ERROR_OBJECT (space, "Failed to queue_buffer for %s", device->name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
init_transform_device (GstVspFilter * space, GstVspFilterDeviceInfo * dev,
    GstBuffer * buf, GstVideoInfo * vinfo, enum v4l2_memory io,
    GstBufferPool * pool)
{
  gint i;

  for (i = 0; i < dev->n_planes; i++)
    dev->strides[i] = get_stride (buf, vinfo, i);

  dev->io = io;

  if (!gst_buffer_pool_is_active (pool) &&
      !setup_device (space, pool, vinfo, dev, dev->strides, io))
    return FALSE;

  return TRUE;
}

static void
setup_v4l2_plane_userptr (GstBuffer * buf, GstVideoFrame * dest_frame,
    guint n_planes, struct v4l2_plane *planes)
{
  const unsigned long page_size = getpagesize();
  const unsigned long page_align_mask = ~(page_size - 1);
  gint i;

  for (i = 0; i < n_planes; i++) {
    struct v4l2_plane *plane = &planes[i];
    guint comp_stride = GST_VIDEO_FRAME_COMP_STRIDE (dest_frame, i);
    guint comp_height = GST_VIDEO_FRAME_COMP_HEIGHT (dest_frame, i);

    plane->m.userptr = ((unsigned long) dest_frame->data[i]) & page_align_mask;
    plane->data_offset = (unsigned long) dest_frame->data[i] - plane->m.userptr;
    plane->bytesused = comp_stride * comp_height;
    plane->length = (plane->bytesused + page_size - 1) & page_align_mask;
  }
}

static GstFlowReturn
gst_vsp_filter_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstVideoFilter *filter = GST_VIDEO_FILTER_CAST (trans);
  GstVspFilter *space = GST_VSP_FILTER_CAST (filter);
  GstBuffer *bufs[MAX_DEVICES];
  GstVideoInfo *vinfos[MAX_DEVICES];
  GstVideoFrame dest_frame;
  GstFlowReturn ret;
  gint i;

  if (G_UNLIKELY (!filter->negotiated))
    goto unknown_format;

  bufs[OUT_DEV] = inbuf;
  bufs[CAP_DEV] = outbuf;
  vinfos[OUT_DEV] = &filter->in_info;
  vinfos[CAP_DEV] = &filter->out_info;

  for (i = 0; i < MAX_DEVICES; i++) {
    GstVspFilterDeviceInfo *dev = &space->devices[i];
    GstBufferPool *pool = space->devices[i].pool;
    GstBuffer *buf = bufs[i];
    GstVideoInfo *vinfo = vinfos[i];
    struct v4l2_buffer v4l2_buf = { 0, };
    struct v4l2_plane planes[GST_VIDEO_MAX_PLANES] = { 0, };

    v4l2_buf.m.planes = planes;

    CLEAR (dest_frame);

    if (dev->io_mode == GST_VSPFILTER_IO_USERPTR) {
      ret = prepare_transform_device_userptr (space, buf,
          vinfo, dev->n_planes, &dest_frame, &v4l2_buf);
    } else if (buf->pool != pool &&
        !gst_is_dmabuf_memory (gst_buffer_peek_memory (buf, 0))) {
      GstBuffer *mmap_buf;

      ret = prepare_transform_device_copy (space, buf, pool, vinfo, &mmap_buf,
          &dest_frame, &v4l2_buf);
      buf = mmap_buf;
    } else {
      ret = prepare_transform_device (space, buf, pool, &v4l2_buf);
    }
    if (ret != GST_FLOW_OK)
      goto transform_exit;

    if (!space->vsp_info->is_stream_started &&
        !init_transform_device (space, dev, buf, vinfo, v4l2_buf.memory, pool))
      goto transform_exit;

    switch (dev->io) {
      case V4L2_MEMORY_USERPTR:
        setup_v4l2_plane_userptr (buf, &dest_frame, dev->n_planes, planes);
        break;
      case V4L2_MEMORY_MMAP:
        if (dev->is_input_device)
          set_v4l2_input_plane_mmap (buf, dev->n_planes, planes);
        break;
      case V4L2_MEMORY_DMABUF:
        if (dev->is_input_device)
          set_v4l2_input_plane_dmabuf (vinfo, planes, dev->n_planes,
              dev->strides);
        break;
    }
    if (dev->io != V4L2_MEMORY_USERPTR) {
      GstVideoMeta *vmeta = gst_buffer_get_video_meta (buf);

      if (vmeta && !get_offset_from_meta (space, buf, vmeta, planes)) {
        ret = GST_FLOW_ERROR;
        goto transform_exit;
      }
    }

    if (!start_transform_device (space, pool, dev, &v4l2_buf)) {
      GST_ERROR_OBJECT (space, "start_transform_device for %s failed",
          dev->name);
      goto transform_exit;
    }

    if (dest_frame.buffer)
      gst_video_frame_unmap (&dest_frame);
  }

  if (!space->vsp_info->is_stream_started) {
    if (!set_vsp_entities (space, vinfos[OUT_DEV], vinfos[CAP_DEV])) {
      GST_ERROR_OBJECT (space, "set_vsp_entities failed");
      ret = GST_FLOW_ERROR;
      goto transform_exit;
    }
    for (i = 0; i < MAX_DEVICES; i++)
      if (!start_capturing (space, &space->devices[i]))
        return FALSE;
  }

  space->vsp_info->is_stream_started = TRUE;

  ret = wait_output_ready (space);
  if (ret != GST_FLOW_OK)
    goto transform_exit;

  for (i = 0; i < MAX_DEVICES; i++) {
    if (dequeue_buffer (space, &space->devices[i])) {
      ret = GST_FLOW_ERROR;
      goto transform_exit;
    }
  }

transform_exit:
  if (dest_frame.buffer)
    gst_video_frame_unmap (&dest_frame);

  return ret;

  /* ERRORS */
unknown_format:
  {
    GST_ELEMENT_ERROR (filter, CORE, NOT_IMPLEMENTED, (NULL),
        ("unknown format"));
    return GST_FLOW_NOT_NEGOTIATED;
  }
}

void
decide_pixelcode (GstVspFilterDeviceInfo * devices,
    enum v4l2_mbus_pixelcode code[MAX_DEVICES])
{
  /* color conversion is done in rpf */
  devices[OUT_DEV].ventity.code[SINK] = code[OUT_DEV];
  devices[OUT_DEV].ventity.code[SRC] = code[CAP_DEV];

  devices[CAP_DEV].ventity.code[SINK] = code[CAP_DEV];
  devices[CAP_DEV].ventity.code[SRC] = code[CAP_DEV];
}

static gboolean
gst_vsp_filter_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstVideoFilter *filter = GST_VIDEO_FILTER_CAST (trans);
  GstVideoFilterClass *fclass;
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;
  guint buf_min = 0, buf_max = 0;
  GstStructure *ins, *outs;
  GstCaps *caps[MAX_DEVICES];
  gint ret;
  enum v4l2_mbus_pixelcode code[MAX_DEVICES];
  GstVideoInfo vinfo[MAX_DEVICES];
  gint i;

  space = GST_VSP_FILTER_CAST (filter);
  fclass = GST_VIDEO_FILTER_GET_CLASS (filter);
  vsp_info = space->vsp_info;

  caps[OUT_DEV] = incaps;
  caps[CAP_DEV] = outcaps;

  for (i = 0; i < MAX_DEVICES; i++) {
    GstStructure *structure;
    GstBufferPool *newpool;

    if (!gst_video_info_from_caps (&vinfo[i], caps[i]))
      goto invalid_caps;

    structure = gst_caps_get_structure (caps[i], 0);
    if (!find_colorimetry (gst_structure_get_value (structure, "colorimetry"))) {
      vinfo[i].colorimetry.range = GST_VIDEO_COLOR_RANGE_UNKNOWN;
      vinfo[i].colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_UNKNOWN;
    }

    if (vsp_info->is_stream_started &&
        !stop_capturing (space, &space->devices[i]))
      return FALSE;

    if (space->devices[i].pool) {
      guint n_reqbufs = 0;

      gst_buffer_pool_set_active (space->devices[i].pool, FALSE);
      if (!request_buffers (space->devices[i].fd, space->devices[i].buftype,
              &n_reqbufs, V4L2_MEMORY_MMAP)) {
        GST_ERROR_OBJECT (space, "reqbuf for %s failed (count = 0)",
            space->devices[i].name);
        return FALSE;
      }
    }

    newpool = gst_vsp_filter_setup_pool (&space->devices[i],
        caps[i], vinfo[i].size, 0);
    if (!newpool)
      goto pool_setup_failed;

    gst_object_replace ((GstObject **) & space->devices[i].pool,
        (GstObject *) newpool);
    gst_object_unref (newpool);

    ret = set_colorspace (vinfo[i].finfo->format, &space->devices[i].format,
        &code[i], &space->devices[i].n_planes);
    if (ret < 0) {
      GST_ERROR_OBJECT (space, "set_colorspace() failed");
      return FALSE;
    }
  }

  GST_DEBUG ("reconfigured %d %d", GST_VIDEO_INFO_FORMAT (&vinfo[OUT_DEV]),
      GST_VIDEO_INFO_FORMAT (&vinfo[CAP_DEV]));

  /* these must match */
  if (vinfo[OUT_DEV].fps_n != vinfo[CAP_DEV].fps_n ||
      vinfo[OUT_DEV].fps_d != vinfo[CAP_DEV].fps_d)
    goto format_mismatch;

  /* if present, these must match too */
  if (vinfo[OUT_DEV].interlace_mode != vinfo[CAP_DEV].interlace_mode)
    goto format_mismatch;

  /* For the reinitialization of entities pipeline */
  if (vsp_info->is_stream_started)
    vsp_info->is_stream_started = FALSE;

  decide_pixelcode (space->devices, code);

  filter->in_info = vinfo[OUT_DEV];
  filter->out_info = vinfo[CAP_DEV];
  GST_BASE_TRANSFORM_CLASS (fclass)->transform_ip_on_passthrough = FALSE;
  filter->negotiated = TRUE;

  return TRUE;

  /* ERRORS */
pool_setup_failed:
  {
    GST_ERROR_OBJECT (space, "failed to setup pool");
    filter->negotiated = FALSE;
    return FALSE;
  }
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

static gboolean
gst_vsp_filter_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query)
{
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;
  GstBufferPool *pool;
  GstStructure *config;
  guint min, size;

  space = GST_VSP_FILTER_CAST (trans);
  vsp_info = space->vsp_info;

  if (!GST_BASE_TRANSFORM_CLASS (parent_class)->propose_allocation (trans,
          decide_query, query))
    return FALSE;

  /* passthrough, we're done */
  if (decide_query == NULL)
    return TRUE;

  if (!space->devices[OUT_DEV].pool) {
    GstCaps *caps;
    GstVideoInfo vinfo;

    gst_query_parse_allocation (query, &caps, NULL);
    gst_video_info_init (&vinfo);
    gst_video_info_from_caps (&vinfo, caps);

    GST_DEBUG_OBJECT (space, "create new pool");
    space->devices[OUT_DEV].pool =
        gst_vsp_filter_setup_pool (&space->devices[OUT_DEV],
        caps, vinfo.size, 0);
    if (!space->devices[OUT_DEV].pool) {
      GST_ERROR_OBJECT (space, "failed to setup pool");
      return FALSE;
    }
  }

  pool = gst_object_ref (space->devices[OUT_DEV].pool);

  GST_DEBUG_OBJECT (space, "propose our pool %p", pool);
  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_get_params (config, NULL, &size, &min, NULL);
  gst_structure_free (config);

  if (gst_query_get_n_allocation_pools (query) > 0)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, 0);
  else
    gst_query_add_allocation_pool (query, pool, size, min, 0);

  gst_object_unref (pool);

  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  return TRUE;
}

static gboolean
gst_vsp_filter_stop (GstBaseTransform * trans)
{
  GstVspFilter *space;
  gboolean ret = TRUE;

  space = GST_VSP_FILTER_CAST (trans);
  if (space->devices[OUT_DEV].pool)
    ret = gst_buffer_pool_set_active (space->devices[OUT_DEV].pool, FALSE);
  return ret;
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

  g_object_class_install_property (gobject_class, PROP_INPUT_IO_MODE,
      g_param_spec_enum ("input-io-mode", "Input IO mode",
          "Input I/O mode",
          GST_TYPE_VSPFILTER_IO_MODE, DEFAULT_PROP_IO_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_OUTPUT_IO_MODE,
      g_param_spec_enum ("output-io-mode", "Output IO mode",
          "Output I/O mode",
          GST_TYPE_VSPFILTER_IO_MODE, DEFAULT_PROP_IO_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_INPUT_COLOR_RANGE,
      g_param_spec_enum ("input-color-range", "Input color range",
          "Color range of incoming video buffer",
          GST_TYPE_VSPFILTER_COLOR_RANGE, DEFAULT_PROP_COLOR_RANGE,
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
  gstbasetransform_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_vsp_filter_propose_allocation);
  gstbasetransform_class->transform =
      GST_DEBUG_FUNCPTR (gst_vsp_filter_transform);
  gstbasetransform_class->set_caps =
      GST_DEBUG_FUNCPTR (gst_vsp_filter_set_caps);
  gstbasetransform_class->stop = GST_DEBUG_FUNCPTR (gst_vsp_filter_stop);

  gstbasetransform_class->passthrough_on_same_caps = TRUE;
}

void
entity_destroy (gpointer data)
{
  g_free ((struct media_entity_desc *) data);
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

  space->devices[OUT_DEV].is_input_device = TRUE;
  space->devices[OUT_DEV].name = g_strdup (DEFAULT_PROP_VSP_DEVFILE_INPUT);
  space->devices[CAP_DEV].name = g_strdup (DEFAULT_PROP_VSP_DEVFILE_OUTPUT);
  space->devices[OUT_DEV].buftype = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  space->devices[CAP_DEV].buftype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  space->devices[OUT_DEV].captype = V4L2_CAP_VIDEO_OUTPUT_MPLANE;
  space->devices[CAP_DEV].captype = V4L2_CAP_VIDEO_CAPTURE_MPLANE;

  space->vsp_info = vsp_info;
  space->input_color_range = DEFAULT_PROP_COLOR_RANGE;

  vsp_info->resz_ventity.fd = -1;

  init_colorimetry_table ();

  space->hash_t = g_hash_table_new_full (g_str_hash, g_str_equal,
      NULL, entity_destroy);
}

static void
gst_vsp_filter_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;

  space = GST_VSP_FILTER (object);
  vsp_info = space->vsp_info;

  switch (property_id) {
    case PROP_VSP_DEVFILE_INPUT:
      if (space->devices[OUT_DEV].name)
        g_free (space->devices[OUT_DEV].name);
      space->devices[OUT_DEV].name = g_value_dup_string (value);
      space->devices[OUT_DEV].prop_name = TRUE;
      break;
    case PROP_VSP_DEVFILE_OUTPUT:
      if (space->devices[CAP_DEV].name)
        g_free (space->devices[CAP_DEV].name);
      space->devices[CAP_DEV].name = g_value_dup_string (value);
      space->devices[CAP_DEV].prop_name = TRUE;
      break;
    case PROP_INPUT_IO_MODE:
      space->devices[OUT_DEV].io_mode = g_value_get_enum (value);
      break;
    case PROP_OUTPUT_IO_MODE:
      space->devices[CAP_DEV].io_mode = g_value_get_enum (value);
      break;
    case PROP_INPUT_COLOR_RANGE:
      space->input_color_range = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_vsp_filter_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstVspFilter *space;
  GstVspFilterVspInfo *vsp_info;

  space = GST_VSP_FILTER (object);
  vsp_info = space->vsp_info;

  switch (property_id) {
    case PROP_VSP_DEVFILE_INPUT:
      g_value_set_string (value, space->devices[OUT_DEV].name);
      break;
    case PROP_VSP_DEVFILE_OUTPUT:
      g_value_set_string (value, space->devices[CAP_DEV].name);
      break;
    case PROP_INPUT_IO_MODE:
      g_value_set_enum (value, space->devices[OUT_DEV].io_mode);
      break;
    case PROP_OUTPUT_IO_MODE:
      g_value_set_enum (value, space->devices[CAP_DEV].io_mode);
      break;
    case PROP_INPUT_COLOR_RANGE:
      g_value_set_enum (value, space->input_color_range);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static gint
queue_buffer (GstVspFilter * space, GstVspFilterDeviceInfo * device,
    struct v4l2_buffer *buf)
{
  if (-1 == xioctl (device->fd, VIDIOC_QBUF, buf)) {
    GST_ERROR_OBJECT (space,
        "VIDIOC_QBUF for %s failed errno=%d", device->name, errno);
    return -1;
  }

  return 0;
}

static gint
dequeue_buffer (GstVspFilter * space, GstVspFilterDeviceInfo * device)
{
  struct v4l2_buffer buf = { 0, };
  struct v4l2_plane planes[GST_VIDEO_MAX_PLANES] = { 0, };

  buf.type = device->buftype;
  buf.memory = device->io;
  buf.length = device->n_planes;
  buf.m.planes = planes;

  if (-1 == xioctl (device->fd, VIDIOC_DQBUF, &buf)) {
    GST_ERROR_OBJECT (space,
        "VIDIOC_DQBUF for %s failed errno=%d", device->name, errno);
    return -1;
  }

  return 0;
}

static gboolean
start_capturing (GstVspFilter * space, GstVspFilterDeviceInfo * device)
{
  if (-1 == xioctl (device->fd, VIDIOC_STREAMON, &device->buftype)) {
    GST_ERROR_OBJECT (space, "VIDIOC_STREAMON for %s failed", device->name);
    return FALSE;
  }

  return TRUE;
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
