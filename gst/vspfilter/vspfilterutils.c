/* GStreamer
 * Copyright (C) 2016-2017 Renesas Electronics Corporation
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

#include <string.h>
#include <sys/ioctl.h>

#include "vspfilterutils.h"

GST_DEBUG_CATEGORY_EXTERN (vspfilter_debug);
#define GST_CAT_DEFAULT vspfilter_debug

#define FORMAT_NEEDS_WIDTH_ALIGN(finfo) \
          GST_VIDEO_FORMAT_INFO_W_SUB (finfo,\
          GST_VIDEO_FORMAT_INFO_N_COMPONENTS(finfo) - 1)

#define FORMAT_NEEDS_HEIGHT_ALIGN(finfo) \
          GST_VIDEO_FORMAT_INFO_H_SUB (finfo,\
          GST_VIDEO_FORMAT_INFO_N_COMPONENTS(finfo) - 1)

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
  {GST_VIDEO_FORMAT_ARGB, V4L2_PIX_FMT_ARGB32, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_xRGB, V4L2_PIX_FMT_XRGB32, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_BGRA, V4L2_PIX_FMT_ABGR32, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_BGRx, V4L2_PIX_FMT_XBGR32, V4L2_MBUS_FMT_ARGB8888_1X32, 1},
  {GST_VIDEO_FORMAT_I420, V4L2_PIX_FMT_YUV420M, V4L2_MBUS_FMT_AYUV8_1X32, 3},
  {GST_VIDEO_FORMAT_NV12, V4L2_PIX_FMT_NV12M, V4L2_MBUS_FMT_AYUV8_1X32, 2},
  {GST_VIDEO_FORMAT_NV21, V4L2_PIX_FMT_NV21M, V4L2_MBUS_FMT_AYUV8_1X32, 2},
  {GST_VIDEO_FORMAT_NV16, V4L2_PIX_FMT_NV16M, V4L2_MBUS_FMT_AYUV8_1X32, 2},
  {GST_VIDEO_FORMAT_UYVY, V4L2_PIX_FMT_UYVY, V4L2_MBUS_FMT_AYUV8_1X32, 1},
  {GST_VIDEO_FORMAT_YUY2, V4L2_PIX_FMT_YUYV, V4L2_MBUS_FMT_AYUV8_1X32, 1},
};

enum v4l2_ycbcr_encoding
set_encoding (GstVideoColorMatrix matrix)
{
  switch (matrix) {
    case GST_VIDEO_COLOR_MATRIX_BT601:
      return V4L2_YCBCR_ENC_601;
    case GST_VIDEO_COLOR_MATRIX_BT709:
      return V4L2_YCBCR_ENC_709;
    default:
      return V4L2_YCBCR_ENC_DEFAULT;
  }
}

enum v4l2_quantization
set_quantization (GstVideoColorRange color_range)
{
  switch (color_range) {
    case GST_VIDEO_COLOR_RANGE_0_255:
      return V4L2_QUANTIZATION_FULL_RANGE;
    case GST_VIDEO_COLOR_RANGE_16_235:
      return V4L2_QUANTIZATION_LIM_RANGE;
    default:
      return V4L2_QUANTIZATION_DEFAULT;
  }
}

guint
round_down_width (const GstVideoFormatInfo *finfo, guint width)
{
  if (FORMAT_NEEDS_WIDTH_ALIGN (finfo))
    return GST_ROUND_DOWN_2 (width);
  else
    return width;
}

guint
round_down_height (const GstVideoFormatInfo *finfo, guint height)
{
  if (FORMAT_NEEDS_HEIGHT_ALIGN (finfo))
    return GST_ROUND_DOWN_2 (height);
  else
    return height;
}

guint
round_up_width (const GstVideoFormatInfo *finfo, guint width)
{
  if (FORMAT_NEEDS_WIDTH_ALIGN (finfo))
    return GST_ROUND_UP_2 (width);
  else
    return width;
}

guint
round_up_height (const GstVideoFormatInfo *finfo, guint height)
{
  if (FORMAT_NEEDS_HEIGHT_ALIGN (finfo))
    return GST_ROUND_UP_2 (height);
  else
    return height;
}

gint
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

gint
xioctl (gint fd, gint request, void *arg)
{
  int r;

  do
    r = ioctl (fd, request, arg);
  while (-1 == r && EINTR == errno);

  return r;
}

gboolean
request_buffers (gint fd, enum v4l2_buf_type buftype, guint * n_bufs,
    enum v4l2_memory io)
{
  struct v4l2_requestbuffers req;

  /* input buffer */
  CLEAR (req);

  req.count = *n_bufs;
  req.type = buftype;
  req.memory = io;

  if (-1 == xioctl (fd, VIDIOC_REQBUFS, &req)) {
    GST_WARNING ("VIDIOC_REQBUFS for %s errno=%d", buftype_str (buftype),
        errno);
    return FALSE;
  }

  *n_bufs = req.count;

  GST_DEBUG ("%s: req.count = %d", buftype_str (buftype), req.count);

  return TRUE;
}

gboolean
set_format (gint fd, guint width, guint height, guint format,
    gint stride[GST_VIDEO_MAX_PLANES], enum v4l2_buf_type buftype,
    enum v4l2_memory io, enum v4l2_ycbcr_encoding encoding,
    enum v4l2_quantization quant)
{
  struct v4l2_format fmt;
  gint i;

  CLEAR (fmt);

  fmt.type = buftype;
  fmt.fmt.pix_mp.width = width;
  fmt.fmt.pix_mp.height = height;
  fmt.fmt.pix_mp.pixelformat = format;
  fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
  fmt.fmt.pix_mp.quantization = quant;

  if (stride) {
    for (i = 0; i < GST_VIDEO_MAX_PLANES; i++) {
      if (stride[i] > 0) {
        GST_DEBUG ("%s: Set bytesperline = %d (plane = %d)\n",
            buftype_str (buftype), stride[i], i);
        fmt.fmt.pix_mp.plane_fmt[i].bytesperline = stride[i];
      }
    }
  }

  /* BT.709 full range is not supported by hardware.
     Fall back to BT.601 full range */
  if (encoding == V4L2_YCBCR_ENC_709 &&
      quant == V4L2_QUANTIZATION_FULL_RANGE) {
    fmt.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_601;
  } else {
    fmt.fmt.pix_mp.ycbcr_enc = encoding;
  }

  if (-1 == xioctl (fd, VIDIOC_S_FMT, &fmt)) {
    GST_ERROR ("VIDIOC_S_FMT for %s failed.", buftype_str (buftype));
    return FALSE;
  }

  GST_DEBUG ("%s: pixelformat = %c%c%c%c (%c%c%c%c)", buftype_str (buftype),
      (fmt.fmt.pix_mp.pixelformat >> 0) & 0xff,
      (fmt.fmt.pix_mp.pixelformat >> 8) & 0xff,
      (fmt.fmt.pix_mp.pixelformat >> 16) & 0xff,
      (fmt.fmt.pix_mp.pixelformat >> 24) & 0xff,
      (format >> 0) & 0xff,
      (format >> 8) & 0xff, (format >> 16) & 0xff, (format >> 24) & 0xff);
  GST_DEBUG ("%s: num_planes = %d", buftype_str (buftype),
      fmt.fmt.pix_mp.num_planes);

  if (stride) {
    for (i = 0; i < fmt.fmt.pix_mp.num_planes; i++) {
      GST_DEBUG ("plane_fmt[%d].sizeimage = %d",
          i, fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
      GST_DEBUG ("plane_fmt[%d].bytesperline = %d",
          i, fmt.fmt.pix_mp.plane_fmt[i].bytesperline);
      stride[i] = fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
    }
  }

  return TRUE;
}
