/* GStreamer
 * Copyright (C) 2016 Renesas Electronics Corporation
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

#include <gst/allocators/gstdmabuf.h>

#include <linux/videodev2.h>
#include <string.h>
#include <fcntl.h>

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
  gboolean *exported;
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
vspfilter_buffer_pool_set_config (GstBufferPool * bpool, GstStructure * config)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (bpool);
  struct v4l2_format fmt;
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  GstVideoInfo *vinfo;
  guint max_buffers;
  GstCaps *caps = NULL;
  guint pix_fmt;
  guint n_reqbufs;
  gint ret;
  gint i, j;

  if (!gst_buffer_pool_config_get_params (config, &caps, NULL, NULL,
          &max_buffers)) {
    GST_ERROR_OBJECT (self, "Failed to get config params");
    return FALSE;
  }

  if (!caps) {
    GST_ERROR_OBJECT (self, "No caps in config");
    return FALSE;
  }

  if (max_buffers == 0) {
    GST_ERROR_OBJECT (self, "Do not allow unlimited buffers");
    return FALSE;
  }

  if (!gst_video_info_from_caps (&self->vinfo, caps)) {
    GST_ERROR_OBJECT (self, "Invalid caps");
    return FALSE;
  }

  vinfo = &self->vinfo;

  /* Convert a pixel format definition from GStreamer to V4L2 */
  ret = set_colorspace (vinfo->finfo->format, &pix_fmt, NULL, &self->n_planes);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "set_colorspace failed");
    return FALSE;
  }

  if (self->exported) {
    n_reqbufs = 0;
    if (!request_buffers (self->fd, self->buftype, &n_reqbufs,
            V4L2_MEMORY_MMAP)) {
      GST_ERROR_OBJECT (self, "reqbuf for %s failed (count = 0)",
          buftype_str (self->buftype));
      return FALSE;
    }
    g_slice_free1 (sizeof (gboolean) * self->n_buffers, self->exported);
    self->exported = NULL;
  }

  memset (self->stride, 0, sizeof (self->stride));

  if (!set_format (self->fd, vinfo->width, vinfo->height, pix_fmt,
          self->stride, self->buftype, V4L2_MEMORY_MMAP)) {
    GST_ERROR_OBJECT (self, "set_format for %s failed (%dx%d)",
        buftype_str (self->buftype), vinfo->width, vinfo->height);
    return FALSE;
  }

  self->n_buffers = max_buffers;

  if (!self->allocator)
    self->allocator = gst_dmabuf_allocator_new ();

  return GST_BUFFER_POOL_CLASS (parent_class)->set_config (bpool, config);
}

static gboolean
vspfilter_buffer_pool_start (GstBufferPool * bpool)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (bpool);

  if (self->exported)
    return TRUE;

  if (!request_buffers (self->fd,
          self->buftype, &self->n_buffers, V4L2_MEMORY_MMAP)) {
    GST_ERROR_OBJECT (self, "request_buffers for %s failed.",
        buftype_str (self->buftype));
    return FALSE;
  }

  self->exported = g_slice_alloc0 (sizeof (gboolean) * self->n_buffers);

  return GST_BUFFER_POOL_CLASS (parent_class)->start (bpool);
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
  guint index = VSPFILTER_INDEX_INVALID;
  void *data;
  gsize size;
  gsize total = 0;
  gsize offset[GST_VIDEO_MAX_PLANES];
  gint ret;
  gint i;

  for (i = 0; i < self->n_buffers; i++) {
    if (!self->exported[i]) {
      index = i;
      break;
    }
  }

  if (index == VSPFILTER_INDEX_INVALID) {
    GST_ERROR_OBJECT (self, "No buffers are left");
    return GST_FLOW_ERROR;
  }

  vf_buffer = g_slice_new0 (VspfilterBuffer);
  vf_buffer->buffer = gst_buffer_new ();
  vf_buffer->index = index;

  for (i = 0; i < self->n_planes; i++) {
    memset (&expbuf, 0, sizeof (expbuf));

    expbuf.type = self->buftype;
    expbuf.index = index;
    expbuf.plane = i;
    expbuf.flags = O_CLOEXEC | O_RDWR;

    ret = ioctl (self->fd, VIDIOC_EXPBUF, &expbuf);
    if (ret < 0) {
      GST_ERROR_OBJECT (self,
          "Failed to export dmabuf for %s (index:%d, plane:%d) errno=%d",
          buftype_str (self->buftype), index, i, errno);
      gst_buffer_unref (vf_buffer->buffer);
      free_vf_buffer (vf_buffer);
      return GST_FLOW_ERROR;
    }

    size = self->stride[i] * GST_VIDEO_INFO_COMP_HEIGHT (&self->vinfo, i);

    gst_buffer_append_memory (vf_buffer->buffer,
        gst_dmabuf_allocator_alloc (self->allocator, expbuf.fd, size));

    offset[i] = total;
    total += size;
  }

  gst_buffer_add_video_meta_full (vf_buffer->buffer, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_INFO_FORMAT (&self->vinfo), GST_VIDEO_INFO_WIDTH (&self->vinfo),
      GST_VIDEO_INFO_HEIGHT (&self->vinfo), self->n_planes, offset,
      self->stride);

  self->exported[index] = TRUE;

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

  return GST_BUFFER_POOL_CLASS (parent_class)->free_buffer (bpool, buffer);
}

static void
vspfilter_buffer_pool_finalize (GObject * object)
{
  VspfilterBufferPool *self = VSPFILTER_BUFFER_POOL_CAST (object);

  g_slice_free1 (sizeof (gboolean) * self->n_buffers, self->exported);
  gst_object_unref (self->allocator);
}

static void
vspfilter_buffer_pool_class_init (VspfilterBufferPoolClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  gobject_class->finalize = vspfilter_buffer_pool_finalize;

  gstbufferpool_class->set_config = vspfilter_buffer_pool_set_config;
  gstbufferpool_class->start = vspfilter_buffer_pool_start;
  gstbufferpool_class->alloc_buffer = vspfilter_buffer_pool_alloc_buffer;
  gstbufferpool_class->free_buffer = vspfilter_buffer_pool_free_buffer;
}

static void
vspfilter_buffer_pool_init (VspfilterBufferPool * pool)
{
}
