/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * This file:
 * Copyright (C) 2003 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2014-2019,2021 Renesas Electronics Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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
#define MAX_ENTITIES 4
#define MAX_PADS 2
#define VSP_CONF_ITEM_INPUT "input-device-name="
#define VSP_CONF_ITEM_OUTPUT "output-device-name="
#define DEFAULT_PROP_VSP_DEVFILE_INPUT "/dev/video0"
#define DEFAULT_PROP_VSP_DEVFILE_OUTPUT "/dev/video1"
#define RESIZE_DEVICE_NAME "uds.0"
    typedef enum
{
  GST_VSPFILTER_AUTO_COLOR_RANGE = V4L2_QUANTIZATION_DEFAULT,
  GST_VSPFILTER_FULL_COLOR_RANGE = V4L2_QUANTIZATION_FULL_RANGE,
  GST_VSPFILTER_LIMITED_COLOR_RANGE = V4L2_QUANTIZATION_LIM_RANGE,
  GST_VSPFILTER_DEFAULT_COLOR_RANGE = -1
} GstVspfilterColorRange;

#define DEFAULT_PROP_COLOR_RANGE GST_VSPFILTER_DEFAULT_COLOR_RANGE

typedef enum
{
  GST_VSPFILTER_IO_AUTO = 0,    /* dmabuf or mmap */
  GST_VSPFILTER_IO_USERPTR
} GstVspfilterIOMode;

#define DEFAULT_PROP_IO_MODE GST_VSPFILTER_IO_AUTO

typedef struct _GstVspFilter GstVspFilter;
typedef struct _GstVspFilterClass GstVspFilterClass;

typedef struct _GstVspFilterVspInfo GstVspFilterVspInfo;
typedef struct _GstVspFilterEntityInfo GstVspFilterEntityInfo;
typedef struct _GstVspFilterDeviceInfo GstVspFilterDeviceInfo;

enum
{
  OUT_DEV,
  CAP_DEV,
  MAX_DEVICES
};

enum
{
  SINK,
  SRC
};

typedef struct
{
  guint left;
  guint top;
} CropInfo;

struct _GstVspFilterEntityInfo
{
  gchar *name;
  gint fd;
  struct media_entity_desc entity;
  enum v4l2_mbus_pixelcode code[MAX_PADS];
};

struct _GstVspFilterVspInfo
{
  gchar *ip_name;
  gint media_fd;
  gboolean is_stream_started;
  gboolean is_resz_device_initialized;
  GstVspFilterEntityInfo resz_ventity;
};

struct _GstVspFilterDeviceInfo
{
  gchar *name;
  gboolean prop_name;
  gint fd;

  guint format;
  guint n_planes;
  guint captype;
  enum v4l2_buf_type buftype;
  enum v4l2_memory io;
  guint strides[GST_VIDEO_MAX_PLANES];

  GstBufferPool *pool;
  GstVspfilterIOMode io_mode;

  GstVspFilterEntityInfo ventity;

  gboolean is_input_device;
  CropInfo crop;
};

/**
 * GstVspFilter:
 *
 * Opaque object data structure.
 */
struct _GstVspFilter
{
  GstVideoFilter element;

  GstVspFilterVspInfo *vsp_info;
  GstVspFilterDeviceInfo devices[MAX_DEVICES];
  GstVspfilterColorRange input_color_range;
  GHashTable *hash_t;
};

struct _GstVspFilterClass
{
  GstVideoFilterClass parent_class;
};

G_END_DECLS
#endif /* __GST_VIDEOCONVERT_H__ */
