/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * This file:
 * Copyright (C) 2003 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2014 Renesas Solutions Corporation
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

#ifndef __GST_VIDEOCONVERT_H__
#define __GST_VIDEOCONVERT_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <asm/types.h>          /* for videodev2.h */

#include <linux/media.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <linux/v4l2-mediabus.h>

G_BEGIN_DECLS

#define GST_TYPE_VSP_FILTER	          (gst_vsp_filter_get_type())
#define GST_VSP_FILTER(obj)               (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_VSP_FILTER,GstVspFilter))
#define GST_VSP_FILTER_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_VSP_FILTER,GstVspFilterClass))
#define GST_IS_VIDEO_CONVERT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_VSP_FILTER))
#define GST_IS_VIDEO_CONVERT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_VSP_FILTER))
#define GST_VSP_FILTER_CAST(obj)       ((GstVspFilter *)(obj))

#define N_BUFFERS 1

#define MAX_DEVICES 2
#define MAX_ENTITIES 4

#define VSP_CONF_ITEM_INPUT "input-device-name="
#define VSP_CONF_ITEM_OUTPUT "output-device-name="

#define DEFAULT_PROP_VSP_DEVFILE_INPUT "/dev/video0"
#define DEFAULT_PROP_VSP_DEVFILE_OUTPUT "/dev/video1"

typedef struct _GstVspFilter GstVspFilter;
typedef struct _GstVspFilterClass GstVspFilterClass;

typedef struct _GstVspFilterVspInfo GstVspFilterVspInfo;
typedef struct _GstVspFilterFrameInfo GstVspFilterFrameInfo;
typedef union _GstVspFilterFrame GstVspFilterFrame;

struct buffer {
  void *start;
  size_t length;
};

enum {
  OUT = 0,
  CAP = 1,
  RESZ = 2,
};

struct _GstVspFilterVspInfo {
  gchar *dev_name[MAX_DEVICES];
  gboolean prop_dev_name[MAX_DEVICES];
  gint v4lout_fd;
  gint v4lcap_fd;
  gchar *ip_name;
  gchar *entity_name[MAX_DEVICES];
  gint media_fd;
  gint v4lsub_fd[MAX_DEVICES];
  gint resz_subdev_fd;
  guint format[MAX_DEVICES];
  enum v4l2_mbus_pixelcode code[MAX_DEVICES];
  guint n_planes[MAX_DEVICES];
  guint  n_buffers[MAX_DEVICES];
  struct buffer buffers[MAX_DEVICES][N_BUFFERS][VIDEO_MAX_PLANES];
  struct media_entity_desc entity[MAX_ENTITIES];
  gboolean is_stream_started;
  gboolean already_device_initialized[MAX_DEVICES];
  gboolean already_setup_info;
  guint16 plane_stride[MAX_DEVICES][VIDEO_MAX_PLANES];
};

union _GstVspFilterFrame {
  GstVideoFrame *frame;
  gint dmafd[GST_VIDEO_MAX_PLANES];
};

struct _GstVspFilterFrameInfo {
  enum v4l2_memory io;
  GstVspFilterFrame vframe;
};

/**
 * GstVspFilter:
 *
 * Opaque object data structure.
 */
struct _GstVspFilter {
  GstVideoFilter element;

  GstVspFilterVspInfo *vsp_info;
};

struct _GstVspFilterClass
{
  GstVideoFilterClass parent_class;
};

G_END_DECLS

#endif /* __GST_VIDEOCONVERT_H__ */
