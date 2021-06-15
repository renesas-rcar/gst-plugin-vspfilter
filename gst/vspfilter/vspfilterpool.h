/* GStreamer
 * Copyright (C) 2016,2019,2021 Renesas Electronics Corporation
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

#ifndef __GST_VSPFILTER_BUFFER_POOL_H__
#define __GST_VSPFILTER_BUFFER_POOL_H__

#include <gst/gstbufferpool.h>
#include <linux/videodev2.h>
#include <gst/video/video.h>

#define VSPFILTER_INDEX_INVALID G_MAXUINT

GstBufferPool * vspfilter_buffer_pool_new (gint fd, enum v4l2_buf_type buftype);
guint vspfilter_buffer_pool_get_buffer_index (GstBuffer *buffer);
gboolean setup_format (GstBufferPool * bpool, guint pix_fmt,
    enum v4l2_memory io, GstVideoInfo * vinfo, gint stride[GST_VIDEO_MAX_PLANES],
    gint size[GST_VIDEO_MAX_PLANES], enum v4l2_quantization quant);
gint * vspfilter_buffer_pool_get_size (GstBufferPool * bpool);
gboolean vspfilter_buffer_pool_orphan_pool (GstBufferPool * bpool);

#endif /*__GST_VSPFILTER_BUFFER_POOL__*/
