/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef __GST_PIPEWIRE_SINK_H__
#define __GST_PIPEWIRE_SINK_H__

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

#include <pipewire/pipewire.h>
#include <gst/gstpipewirepool.h>
#include <gst/gstpipewirecore.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_SINK \
  (gst_pipewire_sink_get_type())
#define GST_PIPEWIRE_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PIPEWIRE_SINK,GstPipeWireSink))
#define GST_PIPEWIRE_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PIPEWIRE_SINK,GstPipeWireSinkClass))
#define GST_IS_PIPEWIRE_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PIPEWIRE_SINK))
#define GST_IS_PIPEWIRE_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PIPEWIRE_SINK))
#define GST_PIPEWIRE_SINK_CAST(obj) \
  ((GstPipeWireSink *) (obj))

typedef struct _GstPipeWireSink GstPipeWireSink;
typedef struct _GstPipeWireSinkClass GstPipeWireSinkClass;


/**
 * GstPipeWireSinkMode:
 * @GST_PIPEWIRE_SINK_MODE_DEFAULT: the default mode as configured in the server
 * @GST_PIPEWIRE_SINK_MODE_RENDER: try to render the media
 * @GST_PIPEWIRE_SINK_MODE_PROVIDE: provide the media
 *
 * Different modes of operation.
 */
typedef enum
{
  GST_PIPEWIRE_SINK_MODE_DEFAULT,
  GST_PIPEWIRE_SINK_MODE_RENDER,
  GST_PIPEWIRE_SINK_MODE_PROVIDE,
} GstPipeWireSinkMode;

#define GST_TYPE_PIPEWIRE_SINK_MODE (gst_pipewire_sink_mode_get_type ())

/**
 * GstPipeWireSink:
 *
 * Opaque data structure.
 */
struct _GstPipeWireSink {
  GstBaseSink element;

  /*< private >*/
  gchar *path;
  gchar *target_object;
  gchar *client_name;
  int fd;

  /* video state */
  gboolean negotiated;

  GstPipeWireCore *core;
  struct spa_hook core_listener;
  GstStructure *client_properties;

  struct pw_stream *stream;
  struct spa_hook stream_listener;

  GstStructure *stream_properties;
  GstPipeWireSinkMode mode;

  GstPipeWirePool *pool;

  GstClock *clock;
  GstClockTime last_time;
};

struct _GstPipeWireSinkClass {
  GstBaseSinkClass parent_class;
};

GType gst_pipewire_sink_get_type (void);
GType gst_pipewire_sink_mode_get_type (void);

G_END_DECLS

#endif /* __GST_PIPEWIRE_SINK_H__ */
