/* GStreamer
 * Copyright (C) 2016-2020 Renesas Electronics Corporation
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

#include <gst/allocators/gstdmabuf.h>

#include <linux/videodev2.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "vspfilterpool.h"
#include "vspfilterutils.h"

GST_DEBUG_CATEGORY_EXTERN (vspfilter_debug);
#define GST_CAT_DEFAULT vspfilter_debug

static G_DEFINE_QUARK (VspfilterBufferQDataQuark, vspfilter_buffer_qdata);

typedef struct _VspfilterBufferPool VspfilterBufferPool;
typedef struct _VspfilterBufferPoolClass VspfilterBufferPoolClass;
typedef struct _VspfilterBuffer VspfilterBuffer;

struct _VspfilterBufferPool
{
  GstBufferPool bufferpool;
  gint fd;
  GstAllocator *allocator;
  enum v4l2_buf_type buftype;
  GstVideoInfo vinfo;
  guint n_planes;
  guint n_buffers;
  gint stride[GST_VIDEO_MAX_PLANES];
  gint size[GST_VIDEO_MAX_PLANES];
  gboolean *exported;
  gboolean orphaned;
};

struct _VspfilterBuffer
{
  GstBuffer *buffer;
  guint index;
};

struct _VspfilterBufferPoolClass
{
  GstBufferPoolClass parent_class;
};

#define vspfilter_buffer_pool_parent_class parent_class
G_DEFINE_TYPE (VspfilterBufferPool, vspfilter_buffer_pool,
    GST_TYPE_BUFFER_POOL);

#define GST_TYPE_VSPFILTER_BUFFER_POOL      (vspfilter_buffer_pool_get_type())
#define VSPFILTER_BUFFER_POOL_CAST(obj)          ((VspfilterBufferPool*)(obj))

gint *
vspfilter_buffer_pool_get_size (GstBufferPool * bpool)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (bpool);

  return self->size;
}

gboolean
setup_format (GstBufferPool * bpool, guint pix_fmt,
    enum v4l2_memory io, GstVideoInfo * vinfo,
    gint stride[GST_VIDEO_MAX_PLANES], gint size[GST_VIDEO_MAX_PLANES],
    enum v4l2_quantization quant)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (bpool);
  enum v4l2_ycbcr_encoding encoding;
  guint width, height;

  if (self->buftype == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
    width = round_up_width (vinfo->finfo, vinfo->width);
    height = round_up_height (vinfo->finfo, vinfo->height);
  } else {
    width = vinfo->width;
    height = vinfo->height;
  }

  encoding = set_encoding (vinfo->colorimetry.matrix);

  if (!set_format (self->fd, width, height, pix_fmt, stride, size,
          self->buftype, io, encoding, quant)) {
    GST_ERROR_OBJECT (self, "set_format for %s failed (%dx%d)",
        buftype_str (self->buftype), width, height);
    return FALSE;
  }

  return TRUE;
}

GstBufferPool *
vspfilter_buffer_pool_new (gint fd, enum v4l2_buf_type buftype)
{
  VspfilterBufferPool *pool;

  pool = g_object_new (vspfilter_buffer_pool_get_type (), NULL);

  pool->fd = fd;
  pool->buftype = buftype;

  return GST_BUFFER_POOL_CAST (pool);
}

guint
vspfilter_buffer_pool_get_buffer_index (GstBuffer * buffer)
{
  VspfilterBuffer *vf_buffer;

  vf_buffer = gst_mini_object_get_qdata ((GstMiniObject *) buffer,
      vspfilter_buffer_qdata_quark ());
  if (!vf_buffer)
    return VSPFILTER_INDEX_INVALID;

  return vf_buffer->index;
}

static gboolean
vspfilter_buffer_pool_release_buffers(VspfilterBufferPool * self)
{
  guint n_reqbufs = 0;

  if (!request_buffers (self->fd, self->buftype, &n_reqbufs, V4L2_MEMORY_MMAP)) {
    GST_ERROR_OBJECT (self, "reqbuf for %s failed (count = 0)",
        buftype_str (self->buftype));
    if (errno == EBUSY)
      GST_ERROR_OBJECT (self, "reqbuf failed for EBUSY."
          "May be a problem of videobuf2 driver");
    return FALSE;
  }

  return TRUE;
}

/* Orphan all outstanding buffers.
   Must be called while streaming is stopped */
gboolean
vspfilter_buffer_pool_orphan_pool (GstBufferPool * bpool)
{
  gboolean ret;

  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (bpool);

  gst_buffer_pool_set_active(bpool, FALSE);

  GST_OBJECT_LOCK(bpool);

  if ((ret = vspfilter_buffer_pool_release_buffers(self)))
    self->orphaned = TRUE;

  GST_OBJECT_UNLOCK(bpool);

  return ret;
}

static gboolean
vspfilter_buffer_pool_set_config (GstBufferPool * bpool, GstStructure * config)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (bpool);
  GstVideoInfo *vinfo;
  guint min, max;
  GstCaps *caps = NULL;
  guint pix_fmt;
  guint n_reqbufs;
  gint ret;
  guint width, height;
  enum v4l2_ycbcr_encoding encoding;
  enum v4l2_quantization quant;
  GstStructure *st;
  gint i;
  guint bufsize = 0;

  if (!gst_buffer_pool_config_get_params (config, &caps, NULL, &min, &max)) {
    GST_ERROR_OBJECT (self, "Failed to get config params");
    return FALSE;
  }

  if (!caps) {
    GST_ERROR_OBJECT (self, "No caps in config");
    return FALSE;
  }

  if (max)
    min = max;

  if (!gst_video_info_from_caps (&self->vinfo, caps)) {
    GST_ERROR_OBJECT (self, "Invalid caps");
    return FALSE;
  }

  vinfo = &self->vinfo;

  st = gst_caps_get_structure (caps, 0);
  if (!find_colorimetry (gst_structure_get_value (st, "colorimetry"))) {
    vinfo->colorimetry.range = GST_VIDEO_COLOR_RANGE_UNKNOWN;
    vinfo->colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_UNKNOWN;
  }

  /* Convert a pixel format definition from GStreamer to V4L2 */
  ret = set_colorspace (vinfo->finfo->format, &pix_fmt, NULL, &self->n_planes);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "set_colorspace failed");
    return FALSE;
  }

  if (self->exported) {
    if (!vspfilter_buffer_pool_release_buffers(self))
      return FALSE;
    g_slice_free1 (sizeof (gboolean) * self->n_buffers, self->exported);
    self->exported = NULL;
  }

  memset (self->stride, 0, sizeof (self->stride));

  if (!setup_format (bpool, pix_fmt, V4L2_MEMORY_MMAP, vinfo,
          self->stride, self->size,
          set_quantization (vinfo->colorimetry.range))) {
    GST_ERROR_OBJECT (self, "Failed to setup device for %s",
        buftype_str (self->buftype));
    return FALSE;
  }

  self->n_buffers = min;

  if (!self->allocator)
    self->allocator = gst_dmabuf_allocator_new ();

  for (i = 0; i < self->n_planes; i++)
    bufsize += self->size[i];

  gst_buffer_pool_config_set_params (config, caps, bufsize, min, max);

  return GST_BUFFER_POOL_CLASS (parent_class)->set_config (bpool, config);
}

static gboolean
vspfilter_buffer_pool_start (GstBufferPool * bpool)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (bpool);

  if (!request_buffers (self->fd,
          self->buftype, &self->n_buffers, V4L2_MEMORY_MMAP)) {
    GST_ERROR_OBJECT (self, "request_buffers for %s failed.",
        buftype_str (self->buftype));
    return FALSE;
  }

  self->exported = g_slice_alloc0 (sizeof (gboolean) * self->n_buffers);

  return GST_BUFFER_POOL_CLASS (parent_class)->start (bpool);
}

static gboolean
vspfilter_buffer_pool_stop (GstBufferPool * bpool)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (bpool);
  gboolean stop_success = TRUE;


  if (!GST_BUFFER_POOL_CLASS (parent_class)->stop (bpool)) {
    GST_ERROR_OBJECT (self, "Failed to free buffer");
    stop_success = FALSE;
  }

  GST_OBJECT_LOCK(self);
  if (!self->orphaned) {

    if (-1 == xioctl (self->fd, VIDIOC_STREAMOFF, &self->buftype)) {
        GST_ERROR_OBJECT (self, "streamoff for %s failed",
            buftype_str (self->buftype));
        stop_success = FALSE;
    }

    if (!vspfilter_buffer_pool_release_buffers(self))
        stop_success = FALSE;
  }
  GST_OBJECT_UNLOCK(self);

  g_slice_free1 (sizeof (gboolean) * self->n_buffers, self->exported);
  self->exported = NULL;

  return stop_success;
}

static void
free_vf_buffer (gpointer data)
{
  g_slice_free (VspfilterBuffer, data);
}

static GstFlowReturn
vspfilter_buffer_pool_alloc_buffer (GstBufferPool * bpool, GstBuffer ** buffer,
    GstBufferPoolAcquireParams * params)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (bpool);
  VspfilterBuffer *vf_buffer;
  struct v4l2_exportbuffer expbuf;
  guint buf_index = VSPFILTER_INDEX_INVALID;
  gsize size;
  gsize total = 0;
  gsize offset[GST_VIDEO_MAX_PLANES];
  gint ret;
  gint i;

  /* Orphaned pools can't allocate new buffers. They can only free already
   * allocated ones and shut down */

  GST_OBJECT_LOCK(self);
  if (self->orphaned) {
    GST_OBJECT_UNLOCK(self);
    return GST_FLOW_ERROR;
  }
  GST_OBJECT_UNLOCK(self);

  for (i = 0; i < self->n_buffers; i++) {
    if (!self->exported[i]) {
      buf_index = i;
      break;
    }
  }

  if (buf_index == VSPFILTER_INDEX_INVALID) {
    GST_ERROR_OBJECT (self, "No buffers are left");
    return GST_FLOW_ERROR;
  }

  vf_buffer = g_slice_new0 (VspfilterBuffer);
  vf_buffer->buffer = gst_buffer_new ();
  vf_buffer->index = buf_index;

  for (i = 0; i < self->n_planes; i++) {
    memset (&expbuf, 0, sizeof (expbuf));

    expbuf.type = self->buftype;
    expbuf.index = buf_index;
    expbuf.plane = i;
    expbuf.flags = O_CLOEXEC | O_RDWR;

    ret = ioctl (self->fd, VIDIOC_EXPBUF, &expbuf);
    if (ret < 0) {
      GST_ERROR_OBJECT (self,
          "Failed to export dmabuf for %s (index:%d, plane:%d) errno=%d",
          buftype_str (self->buftype), buf_index, i, errno);
      gst_buffer_unref (vf_buffer->buffer);
      free_vf_buffer (vf_buffer);
      return GST_FLOW_ERROR;
    }

    gst_buffer_append_memory (vf_buffer->buffer,
        gst_dmabuf_allocator_alloc (self->allocator, expbuf.fd, self->size[i]));

    offset[i] = total;
    total += self->size[i];
  }

  gst_buffer_add_video_meta_full (vf_buffer->buffer, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_INFO_FORMAT (&self->vinfo), GST_VIDEO_INFO_WIDTH (&self->vinfo),
      GST_VIDEO_INFO_HEIGHT (&self->vinfo), self->n_planes, offset,
      self->stride);

  self->exported[buf_index] = TRUE;

  gst_mini_object_set_qdata ((GstMiniObject *) vf_buffer->buffer,
      vspfilter_buffer_qdata_quark (), vf_buffer, free_vf_buffer);

  *buffer = vf_buffer->buffer;

  return GST_FLOW_OK;
}

static void
vspfilter_buffer_pool_free_buffer (GstBufferPool * bpool, GstBuffer * buffer)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (bpool);
  VspfilterBuffer *vf_buffer;
  GstMemory *mem;
  gint i;

  for (i = 0; i < self->n_planes; i++) {
    mem = gst_buffer_peek_memory (buffer, i);
    close (gst_dmabuf_memory_get_fd (mem));
  }

  vf_buffer = gst_mini_object_steal_qdata ((GstMiniObject *) buffer,
      vspfilter_buffer_qdata_quark ());
  if (vf_buffer)
    self->exported[vf_buffer->index] = FALSE;
  free_vf_buffer (vf_buffer);

  GST_BUFFER_POOL_CLASS (parent_class)->free_buffer (bpool, buffer);
}

static void
vspfilter_buffer_pool_finalize (GObject * object)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (object);

  gst_object_unref (self->allocator);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
vspfilter_buffer_pool_class_init (VspfilterBufferPoolClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  gobject_class->finalize = vspfilter_buffer_pool_finalize;

  gstbufferpool_class->set_config = vspfilter_buffer_pool_set_config;
  gstbufferpool_class->start = vspfilter_buffer_pool_start;
  gstbufferpool_class->stop = vspfilter_buffer_pool_stop;
  gstbufferpool_class->alloc_buffer = vspfilter_buffer_pool_alloc_buffer;
  gstbufferpool_class->free_buffer = vspfilter_buffer_pool_free_buffer;
}

static void
vspfilter_buffer_pool_init (VspfilterBufferPool * pool)
{
}
