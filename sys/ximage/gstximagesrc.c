/* GStreamer
 *
 * Copyright (C) 2006 Zaheer Merali <zaheerabbas at merali dot org>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-ximagesrc
 *
 * This element captures your X Display and creates raw RGB video.  It uses
 * the XDamage extension if available to only capture areas of the screen that
 * have changed since the last frame.  It uses the XFixes extension if
 * available to also capture your mouse pointer.  By default it will fixate to
 * 25 frames per second.
 *
 * <refsect2>
 * <title>Example pipelines</title>
 * |[
 * gst-launch-1.0 ximagesrc ! video/x-raw,framerate=5/1 ! videoconvert ! theoraenc ! oggmux ! filesink location=desktop.ogg
 * ]| Encodes your X display to an Ogg theora video at 5 frames per second.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstximagesrc.h"

#include <string.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <gst/gst.h>
#include <gst/gst-i18n-plugin.h>
#include <gst/video/video.h>

#include "gst/glib-compat-private.h"

GST_DEBUG_CATEGORY_STATIC (gst_debug_ximage_src);
#define GST_CAT_DEFAULT gst_debug_ximage_src

static GstStaticPadTemplate t =
GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, "
        "framerate = (fraction) [ 0, MAX ], "
        "width = (int) [ 1, MAX ], " "height = (int) [ 1, MAX ], "
        "pixel-aspect-ratio = (fraction) [ 0, MAX ]"));

enum
{
  PROP_0,
  PROP_DISPLAY_NAME,
  PROP_SCREEN_NUM,
  PROP_SHOW_POINTER,
  PROP_USE_DAMAGE,
  PROP_STARTX,
  PROP_STARTY,
  PROP_ENDX,
  PROP_ENDY,
  PROP_REMOTE,
  PROP_XID,
  PROP_XNAME,
};

#define gst_ximage_src_parent_class parent_class
G_DEFINE_TYPE (GstXImageSrc, gst_ximage_src, GST_TYPE_PUSH_SRC);

static GstCaps *gst_ximage_src_fixate (GstBaseSrc * bsrc, GstCaps * caps);

static Window
gst_ximage_src_find_window (GstXImageSrc * src, Window root, const char *name)
{
  Window *children;
  Window window = 0, root_return, parent_return;
  unsigned int nchildren;
  char *tmpname;
  int n, status;

  status = XFetchName (src->xcontext->disp, root, &tmpname);
  if (status && !strcmp (name, tmpname))
    return root;

  status =
      XQueryTree (src->xcontext->disp, root, &root_return, &parent_return,
      &children, &nchildren);
  if (!status || !children)
    return (Window) 0;

  for (n = 0; n < nchildren; ++n) {
    window = gst_ximage_src_find_window (src, children[n], name);
    if (window != 0)
      break;
  }

  XFree (children);
  return window;
}

static gboolean
gst_ximage_get_pixmap_size (GstXContext * s, Drawable d, gint * width,
    gint * height)
{
  Window dummy1;
  int dummy2, status;
  unsigned int dummy3, _width, _height;

  status = XGetGeometry (s->disp, d, &dummy1,
      &dummy2, &dummy2, &_width, &_height, &dummy3, &dummy3);

  *width = _width;
  *height = _height;

  return status ? TRUE : FALSE;
}

static gboolean
gst_ximage_src_open_display (GstXImageSrc * s, const gchar * name)
{
  long event_mask = StructureNotifyMask;
  g_return_val_if_fail (GST_IS_XIMAGE_SRC (s), FALSE);

  if (s->xcontext != NULL)
    return TRUE;

  g_mutex_lock (&s->x_lock);
  s->xcontext = ximageutil_xcontext_get (GST_ELEMENT (s), name);
  if (s->xcontext == NULL) {
    g_mutex_unlock (&s->x_lock);
    GST_ELEMENT_ERROR (s, RESOURCE, OPEN_READ,
        ("Could not open X display for reading"),
        ("NULL returned from getting xcontext"));
    return FALSE;
  }
  s->width = s->xcontext->width;
  s->height = s->xcontext->height;

  s->window = s->xcontext->root;
  if (s->xid != 0 || s->xname) {
    int status;
    XWindowAttributes attrs;
    Window window;

    if (s->xid != 0) {
      status = XGetWindowAttributes (s->xcontext->disp, s->xid, &attrs);
      if (status) {
        GST_DEBUG_OBJECT (s, "Found window XID %" G_GUINT64_FORMAT, s->xid);
        s->window = s->xid;
        goto window_found;
      } else {
        GST_WARNING_OBJECT (s, "Failed to get window %" G_GUINT64_FORMAT
            " attributes", s->xid);
      }
    }

    if (s->xname) {
      GST_DEBUG_OBJECT (s, "Looking for window %s", s->xname);
      window = gst_ximage_src_find_window (s, s->xcontext->root, s->xname);
      if (window != 0) {
        GST_DEBUG_OBJECT (s, "Found window named %s, ", s->xname);
        status = XGetWindowAttributes (s->xcontext->disp, window, &attrs);
        if (status) {
          s->window = window;
          goto window_found;
        } else {
          GST_WARNING_OBJECT (s, "Failed to get window attributes for "
              "window named %s", s->xname);
        }
      }
    }

    GST_INFO_OBJECT (s, "Using root window");
    goto use_root_window;

  window_found:
    g_assert (s->window != 0);
    s->width = attrs.width;
    s->height = attrs.height;
    GST_INFO_OBJECT (s, "Using default window size of %dx%d",
        s->width, s->height);
  }
use_root_window:
  s->pixmap = s->window;

#ifdef HAVE_XFIXES
  /* check if xfixes supported */
  {
    int error_base;

    if (XFixesQueryExtension (s->xcontext->disp, &s->fixes_event_base,
            &error_base)) {
      s->have_xfixes = TRUE;
      GST_DEBUG_OBJECT (s, "X Server supports XFixes");
    } else {

      GST_DEBUG_OBJECT (s, "X Server does not support XFixes");
    }
  }

#ifdef HAVE_XDAMAGE
  /* check if xdamage is supported */
  {
    int error_base;
    long evmask = NoEventMask;

    s->have_xdamage = FALSE;
    s->damage = None;
    s->damage_copy_gc = None;
    s->damage_region = None;

    if (XDamageQueryExtension (s->xcontext->disp, &s->damage_event_base,
            &error_base)) {
      s->damage =
          XDamageCreate (s->xcontext->disp, s->window, XDamageReportNonEmpty);
      if (s->damage != None) {
        s->damage_region = XFixesCreateRegion (s->xcontext->disp, NULL, 0);
        if (s->damage_region != None) {
          XGCValues values;

          GST_DEBUG_OBJECT (s, "Using XDamage extension");
          values.subwindow_mode = IncludeInferiors;
          s->damage_copy_gc = XCreateGC (s->xcontext->disp,
              s->window, GCSubwindowMode, &values);
          XSelectInput (s->xcontext->disp, s->window, evmask);

          s->have_xdamage = TRUE;
        } else {
          XDamageDestroy (s->xcontext->disp, s->damage);
          s->damage = None;
        }
      } else
        GST_DEBUG_OBJECT (s, "Could not attach to XDamage");
    } else {
      GST_DEBUG_OBJECT (s, "X Server does not have XDamage extension");
    }
  }
#endif
#ifdef HAVE_XCOMPOSITE
  {
    int error_base;

    s->have_xcomposite = FALSE;

    if (XCompositeQueryExtension (s->xcontext->disp, &s->composite_event_base,
            &error_base)) {
      s->have_xcomposite = TRUE;
      XCompositeRedirectWindow (s->xcontext->disp, s->window,
          CompositeRedirectAutomatic);
      s->pixmap = XCompositeNameWindowPixmap (s->xcontext->disp, s->window);
      if (!s->pixmap) {
        s->pixmap = s->window;
        s->have_xcomposite = TRUE;
        GST_DEBUG_OBJECT (s, "Failed to pixmap, not using XComposite");
      } else {
        GST_DEBUG_OBJECT (s, "Using XComposite extension");
      }
    } else {
      GST_DEBUG_OBJECT (s, "X Server does not have XComposite extension");
    }
  }
#endif
#endif

  XSelectInput (s->xcontext->disp, s->window, event_mask);

  g_mutex_unlock (&s->x_lock);

  if (s->xcontext == NULL)
    return FALSE;

  return TRUE;
}

static gboolean
gst_ximage_src_start (GstBaseSrc * basesrc)
{
  GstXImageSrc *s = GST_XIMAGE_SRC (basesrc);

  s->last_frame_no = -1;
#ifdef HAVE_XDAMAGE
  if (s->last_ximage)
    gst_buffer_unref (GST_BUFFER_CAST (s->last_ximage));
  s->last_ximage = NULL;
#endif
  return gst_ximage_src_open_display (s, s->display_name);
}

static gboolean
gst_ximage_src_stop (GstBaseSrc * basesrc)
{
  GstXImageSrc *src = GST_XIMAGE_SRC (basesrc);

#ifdef HAVE_XDAMAGE
  if (src->last_ximage)
    gst_buffer_unref (GST_BUFFER_CAST (src->last_ximage));
  src->last_ximage = NULL;
#endif

#ifdef HAVE_XFIXES
  if (src->cursor_image)
    XFree (src->cursor_image);
  src->cursor_image = NULL;
#endif

  if (src->xcontext) {
    g_mutex_lock (&src->x_lock);

#ifdef HAVE_XDAMAGE
    if (src->damage_copy_gc != None) {
      XFreeGC (src->xcontext->disp, src->damage_copy_gc);
      src->damage_copy_gc = None;
    }
    if (src->damage_region != None) {
      XFixesDestroyRegion (src->xcontext->disp, src->damage_region);
      src->damage_region = None;
    }
    if (src->damage != None) {
      XDamageDestroy (src->xcontext->disp, src->damage);
      src->damage = None;
    }
#endif

    ximageutil_xcontext_clear (src->xcontext);
    src->xcontext = NULL;
    g_mutex_unlock (&src->x_lock);
  }

  return TRUE;
}

static gboolean
gst_ximage_src_unlock (GstBaseSrc * basesrc)
{
  GstXImageSrc *src = GST_XIMAGE_SRC (basesrc);

  /* Awaken the create() func if it's waiting on the clock */
  GST_OBJECT_LOCK (src);
  if (src->clock_id) {
    GST_DEBUG_OBJECT (src, "Waking up waiting clock");
    gst_clock_id_unschedule (src->clock_id);
  }
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}

static gboolean
gst_ximage_src_recalc (GstXImageSrc * src)
{
  gboolean res;
  GstEvent *event;
  GstCaps *caps;
  GstPad *pad;
  XWindowAttributes attrs;

  if (!src->xcontext)
    return FALSE;

#ifdef HAVE_XCOMPOSITE
  if (src->have_xcomposite) {
    Pixmap pixmap;
    XFreePixmap (src->xcontext->disp, src->pixmap);
    pixmap = XCompositeNameWindowPixmap (src->xcontext->disp, src->window);
    if (!pixmap)
      return FALSE;
    gst_ximage_get_pixmap_size (src->xcontext, pixmap, &src->width,
        &src->height);
    src->pixmap = pixmap;
  } else
#endif
  {
    if (!XGetWindowAttributes (src->xcontext->disp, src->window, &attrs)
        && src->width == attrs.width && src->height == attrs.height)
      return FALSE;
    src->width = attrs.width;
    src->height = attrs.height;
  }

  pad = GST_BASE_SRC (src)->srcpad;
  caps = gst_pad_get_current_caps (pad);
  if (!caps)
    return FALSE;

  caps = gst_caps_make_writable (caps);
  gst_caps_set_simple (caps,
      "width", G_TYPE_INT, src->width, "height", G_TYPE_INT, src->height, NULL);

  event = gst_event_new_caps (caps);
  gst_caps_unref (caps);

  res = gst_pad_push_event (pad, event);
  return res;
}

#ifdef HAVE_XFIXES
static void
composite_pixel (GstXContext * xcontext, guchar * dest, guchar * src)
{
  guint8 r = src[2];
  guint8 g = src[1];
  guint8 b = src[0];
  guint8 a = src[3];
  guint8 dr, dg, db;
  guint32 color;
  gint r_shift, r_max, r_shift_out;
  gint g_shift, g_max, g_shift_out;
  gint b_shift, b_max, b_shift_out;

  switch (xcontext->bpp) {
    case 8:
      color = *dest;
      break;
    case 16:
      color = GUINT16_FROM_LE (*(guint16 *) (dest));
      break;
    case 32:
      color = GUINT32_FROM_LE (*(guint32 *) (dest));
      break;
    default:
      /* Should not reach here */
      g_return_if_reached ();
  }

  /* possible optimisation:
   * move the code that finds shift and max in the _link function */
  for (r_shift = 0; !(xcontext->visual->red_mask & (1 << r_shift)); r_shift++);
  for (g_shift = 0; !(xcontext->visual->green_mask & (1 << g_shift));
      g_shift++);
  for (b_shift = 0; !(xcontext->visual->blue_mask & (1 << b_shift)); b_shift++);

  for (r_shift_out = 0; !(xcontext->visual->red_mask & (1 << r_shift_out));
      r_shift_out++);
  for (g_shift_out = 0; !(xcontext->visual->green_mask & (1 << g_shift_out));
      g_shift_out++);
  for (b_shift_out = 0; !(xcontext->visual->blue_mask & (1 << b_shift_out));
      b_shift_out++);


  r_max = (xcontext->visual->red_mask >> r_shift);
  b_max = (xcontext->visual->blue_mask >> b_shift);
  g_max = (xcontext->visual->green_mask >> g_shift);

#define RGBXXX_R(x)  (((x)>>r_shift) & (r_max))
#define RGBXXX_G(x)  (((x)>>g_shift) & (g_max))
#define RGBXXX_B(x)  (((x)>>b_shift) & (b_max))

  dr = (RGBXXX_R (color) * 255) / r_max;
  dg = (RGBXXX_G (color) * 255) / g_max;
  db = (RGBXXX_B (color) * 255) / b_max;

  dr = (r * a + (0xff - a) * dr) / 0xff;
  dg = (g * a + (0xff - a) * dg) / 0xff;
  db = (b * a + (0xff - a) * db) / 0xff;

  color = (((dr * r_max) / 255) << r_shift_out) +
      (((dg * g_max) / 255) << g_shift_out) +
      (((db * b_max) / 255) << b_shift_out);

  switch (xcontext->bpp) {
    case 8:
      *dest = color;
      break;
    case 16:
      *(guint16 *) (dest) = color;
      break;
    case 32:
      *(guint32 *) (dest) = color;
      break;
    default:
      g_warning ("bpp %d not supported\n", xcontext->bpp);
  }
}
#endif

#ifdef HAVE_XDAMAGE
static void
copy_buffer (GstBuffer * dest, GstBuffer * src)
{
  GstMapInfo map;

  gst_buffer_map (src, &map, GST_MAP_READ);
  gst_buffer_fill (dest, 0, map.data, map.size);
  gst_buffer_unmap (src, &map);
}
#endif

/*
 * XDamage is rather slow, but partial
 * updates from remote x servers would
 * benefit. i doubt this is the common
 * case, but no need to remove support

gst_ximage_capture()
    list damage_areas = []
    bool resize = false
    bool destroy = false
    bool mouse_change = false
    while(event = next_event())
        switch(event)
            mouse_entered:
            mouse_moved:
                mouse_position = event.position (reletive to window)
            mouse_leave:
                mouse_position = null (no mouse)
            mouse_configure:
                mouse_change = true
            damage_event:
                damage_areas.add(event.area)
            resize_event:
                resize = true
            destroy_event:
                destroy = true
    if (resize)
        grab_pixmap()
        if (!update_caps()) //new caps not accepted
            exit
        goto capture_full

    if(!partial_capture)
        goto capture_full
    if(!previous)
        goto capture_full
capture_partial:
    if(damage_areas == 0)
        return previous
    next = dupe(previous)
    for area in damage_areas
        XGrabArea(next, area, dst)
    previous = next
    goto mouse
capture_full:
    next = new buffer
mouse:
    if(mouse_position != null)
        if(mouse_change)
            cursor_image = get_cursor_image()
        draw_mouse(cursor_image, mouse_position)
    return next
*/

/* Retrieve an XImageSrcBuffer, preferably from our
 * pool of existing images and populate it from the window */
static GstBuffer *
gst_ximage_src_ximage_get (GstXImageSrc * ximagesrc)
{
  GstBuffer *ximage = NULL;
  GstMetaXImage *meta;
  gboolean resize = FALSE, destroy = FALSE;

  g_return_val_if_fail (GST_IS_XIMAGE_SRC (ximagesrc), NULL);

  /* Drain event queue */
  g_mutex_lock (&ximagesrc->x_lock);
  while (XEventsQueued (ximagesrc->xcontext->disp, QueuedAfterReading)) {
    XEvent ev;
    XNextEvent (ximagesrc->xcontext->disp, &ev);

    switch (ev.type) {
      case ConfigureNotify:
        resize = TRUE;
        break;
      default:
        break;
    }
  }
  g_mutex_unlock (&ximagesrc->x_lock);

  if (resize)
    gst_ximage_src_recalc (ximagesrc);

  if (ximage == NULL) {
    GST_DEBUG_OBJECT (ximagesrc, "creating image (%dx%d)",
        ximagesrc->width, ximagesrc->height);

    ximage = gst_ximageutil_ximage_new (ximagesrc->xcontext,
        GST_ELEMENT (ximagesrc), ximagesrc->width, ximagesrc->height);
    if (ximage == NULL) {
      GST_ELEMENT_ERROR (ximagesrc, RESOURCE, WRITE, (NULL),
          ("could not create a %dx%d ximage", ximagesrc->width,
              ximagesrc->height));
      return FALSE;
    }
  }
  meta = GST_META_XIMAGE_GET (ximage);

#ifdef HAVE_XSHM
  if (ximagesrc->xcontext->use_xshm) {
    GST_DEBUG_OBJECT (ximagesrc, "Retrieving screen using XShm");
    XShmGetImage (ximagesrc->xcontext->disp, ximagesrc->pixmap,
        meta->ximage, ximagesrc->startx, ximagesrc->starty, AllPlanes);
  } else
#else /* HAVE_XSHM */
  {
    GST_DEBUG_OBJECT (ximagesrc, "Retrieving screen using XGetImage");
    if (ximagesrc->remote) {
      XGetSubImage (ximagesrc->xcontext->disp, ximagesrc->pixmap,
          ximagesrc->startx, ximagesrc->starty, ximagesrc->width,
          ximagesrc->height, AllPlanes, ZPixmap, meta->ximage, 0, 0);
    } else {
      meta->ximage =
          XGetImage (ximagesrc->xcontext->disp, ximagesrc->pixmap,
          ximagesrc->startx, ximagesrc->starty, ximagesrc->width,
          ximagesrc->height, AllPlanes, ZPixmap);
    }
  }
#endif

  return ximage;
}

static GstFlowReturn
gst_ximage_src_create (GstPushSrc * bs, GstBuffer ** buf)
{
  GstXImageSrc *s = GST_XIMAGE_SRC (bs);
  GstBuffer *image;
  GstClockTime base_time;
  GstClockTime next_capture_ts;
  GstClockTime dur;
  gint64 next_frame_no;

  if (!gst_ximage_src_recalc (s)) {
    GST_ELEMENT_ERROR (s, RESOURCE, FAILED,
        (_("Changing resolution at runtime is not yet supported.")), (NULL));
    return GST_FLOW_ERROR;
  }

  if (s->fps_n <= 0 || s->fps_d <= 0)
    return GST_FLOW_NOT_NEGOTIATED;     /* FPS must be > 0 */

  /* Now, we might need to wait for the next multiple of the fps
   * before capturing */

  GST_OBJECT_LOCK (s);
  if (GST_ELEMENT_CLOCK (s) == NULL) {
    GST_OBJECT_UNLOCK (s);
    GST_ELEMENT_ERROR (s, RESOURCE, FAILED,
        (_("Cannot operate without a clock")), (NULL));
    return GST_FLOW_ERROR;
  }

  base_time = GST_ELEMENT_CAST (s)->base_time;
  next_capture_ts = gst_clock_get_time (GST_ELEMENT_CLOCK (s));
  next_capture_ts -= base_time;

  /* Figure out which 'frame number' position we're at, based on the cur time
   * and frame rate */
  next_frame_no = gst_util_uint64_scale (next_capture_ts,
      s->fps_n, GST_SECOND * s->fps_d);
  if (next_frame_no == s->last_frame_no) {
    GstClockID id;
    GstClockReturn ret;

    /* Need to wait for the next frame */
    next_frame_no += 1;

    /* Figure out what the next frame time is */
    next_capture_ts = gst_util_uint64_scale (next_frame_no,
        s->fps_d * GST_SECOND, s->fps_n);

    id = gst_clock_new_single_shot_id (GST_ELEMENT_CLOCK (s),
        next_capture_ts + base_time);
    s->clock_id = id;

    /* release the object lock while waiting */
    GST_OBJECT_UNLOCK (s);

    GST_DEBUG_OBJECT (s, "Waiting for next frame time %" G_GUINT64_FORMAT,
        next_capture_ts);
    ret = gst_clock_id_wait (id, NULL);
    GST_OBJECT_LOCK (s);

    gst_clock_id_unref (id);
    s->clock_id = NULL;
    if (ret == GST_CLOCK_UNSCHEDULED) {
      /* Got woken up by the unlock function */
      GST_OBJECT_UNLOCK (s);
      return GST_FLOW_FLUSHING;
    }
    /* Duration is a complete 1/fps frame duration */
    dur = gst_util_uint64_scale_int (GST_SECOND, s->fps_d, s->fps_n);
  } else {
    GstClockTime next_frame_ts;

    GST_DEBUG_OBJECT (s, "No need to wait for next frame time %"
        G_GUINT64_FORMAT " next frame = %" G_GINT64_FORMAT " prev = %"
        G_GINT64_FORMAT, next_capture_ts, next_frame_no, s->last_frame_no);
    next_frame_ts = gst_util_uint64_scale (next_frame_no + 1,
        s->fps_d * GST_SECOND, s->fps_n);
    /* Frame duration is from now until the next expected capture time */
    dur = next_frame_ts - next_capture_ts;
  }
  s->last_frame_no = next_frame_no;
  GST_OBJECT_UNLOCK (s);

  image = gst_ximage_src_ximage_get (s);
  if (!image)
    return GST_FLOW_ERROR;

  *buf = image;
  GST_BUFFER_DTS (*buf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_PTS (*buf) = next_capture_ts;
  GST_BUFFER_DURATION (*buf) = dur;

  return GST_FLOW_OK;
}

static void
gst_ximage_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstXImageSrc *src = GST_XIMAGE_SRC (object);

  switch (prop_id) {
    case PROP_DISPLAY_NAME:

      g_free (src->display_name);
      src->display_name = g_strdup (g_value_get_string (value));
      break;
    case PROP_SCREEN_NUM:
      src->screen_num = g_value_get_uint (value);
      break;
    case PROP_SHOW_POINTER:
      src->show_pointer = g_value_get_boolean (value);
      break;
    case PROP_USE_DAMAGE:
      src->use_damage = g_value_get_boolean (value);
      break;
    case PROP_STARTX:
      src->startx = g_value_get_uint (value);
      break;
    case PROP_STARTY:
      src->starty = g_value_get_uint (value);
      break;
    case PROP_ENDX:
      src->endx = g_value_get_uint (value);
      break;
    case PROP_ENDY:
      src->endy = g_value_get_uint (value);
      break;
    case PROP_REMOTE:
      src->remote = g_value_get_boolean (value);
      break;
    case PROP_XID:
      if (src->xcontext != NULL) {
        g_warning ("ximagesrc window ID must be set before opening display");
        break;
      }
      src->xid = g_value_get_uint64 (value);
      break;
    case PROP_XNAME:
      if (src->xcontext != NULL) {
        g_warning ("ximagesrc window name must be set before opening display");
        break;
      }
      g_free (src->xname);
      src->xname = g_strdup (g_value_get_string (value));
      break;
    default:
      break;
  }
}

static void
gst_ximage_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstXImageSrc *src = GST_XIMAGE_SRC (object);

  switch (prop_id) {
    case PROP_DISPLAY_NAME:
      if (src->xcontext)
        g_value_set_string (value, DisplayString (src->xcontext->disp));
      else
        g_value_set_string (value, src->display_name);

      break;
    case PROP_SCREEN_NUM:
      g_value_set_uint (value, src->screen_num);
      break;
    case PROP_SHOW_POINTER:
      g_value_set_boolean (value, src->show_pointer);
      break;
    case PROP_USE_DAMAGE:
      g_value_set_boolean (value, src->use_damage);
      break;
    case PROP_STARTX:
      g_value_set_uint (value, src->startx);
      break;
    case PROP_STARTY:
      g_value_set_uint (value, src->starty);
      break;
    case PROP_ENDX:
      g_value_set_uint (value, src->endx);
      break;
    case PROP_ENDY:
      g_value_set_uint (value, src->endy);
      break;
    case PROP_REMOTE:
      g_value_set_boolean (value, src->remote);
      break;
    case PROP_XID:
      g_value_set_uint64 (value, src->xid);
      break;
    case PROP_XNAME:
      g_value_set_string (value, src->xname);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_ximage_src_dispose (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_ximage_src_finalize (GObject * object)
{
  GstXImageSrc *src = GST_XIMAGE_SRC (object);

  if (src->xcontext)
    ximageutil_xcontext_clear (src->xcontext);

  g_free (src->xname);
  g_mutex_clear (&src->x_lock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstCaps *
gst_ximage_src_get_caps (GstBaseSrc * bs, GstCaps * filter)
{
  GstXImageSrc *s = GST_XIMAGE_SRC (bs);
  GstXContext *xcontext;
  gint width, height;
  GstVideoFormat format;

  if ((!s->xcontext) && (!gst_ximage_src_open_display (s, s->display_name)))
    return gst_pad_get_pad_template_caps (GST_BASE_SRC (s)->srcpad);

  xcontext = s->xcontext;
  if (!gst_ximage_get_pixmap_size (s->xcontext, s->pixmap, &width, &height)) {
    width = s->xcontext->width;
    height = s->xcontext->height;
  }

  /* property comments say 0 means right/bottom, means we can't capture
     the top left pixel alone */
  if (s->endx == 0)
    s->endx = width - 1;
  if (s->endy == 0)
    s->endy = height - 1;

  if (s->endx >= s->startx && s->endy >= s->starty) {
    /* this means user has put in values */
    if (s->startx < xcontext->width && s->endx < xcontext->width &&
        s->starty < xcontext->height && s->endy < xcontext->height) {
      /* values are fine */
      s->width = width = s->endx - s->startx + 1;
      s->height = height = s->endy - s->starty + 1;
    } else {
      GST_WARNING
          ("User put in co-ordinates overshooting the X resolution, setting to full screen");
      s->startx = 0;
      s->starty = 0;
      s->endx = width - 1;
      s->endy = height - 1;
    }
  } else {
    GST_WARNING ("User put in bogus co-ordinates, setting to full screen");
    s->startx = 0;
    s->starty = 0;
    s->endx = width - 1;
    s->endy = height - 1;
  }
  GST_DEBUG ("width = %d, height=%d", width, height);

  format =
      gst_video_format_from_masks (xcontext->depth, xcontext->bpp,
      xcontext->endianness, xcontext->r_mask_output, xcontext->g_mask_output,
      xcontext->b_mask_output, 0);

  return gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING, gst_video_format_to_string (format),
      "width", G_TYPE_INT, width,
      "height", G_TYPE_INT, height,
      "framerate", GST_TYPE_FRACTION_RANGE, 1, G_MAXINT, G_MAXINT, 1,
      "pixel-aspect-ratio", GST_TYPE_FRACTION, xcontext->par_n, xcontext->par_d,
      NULL);
}

static gboolean
gst_ximage_src_set_caps (GstBaseSrc * bs, GstCaps * caps)
{
  GstXImageSrc *s = GST_XIMAGE_SRC (bs);
  GstStructure *structure;
  const GValue *new_fps;

  /* If not yet opened, disallow setcaps until later */
  if (!s->xcontext)
    return FALSE;

  /* The only thing that can change is the framerate downstream wants */
  structure = gst_caps_get_structure (caps, 0);
  new_fps = gst_structure_get_value (structure, "framerate");
  if (!new_fps)
    return FALSE;

  /* Store this FPS for use when generating buffers */
  s->fps_n = gst_value_get_fraction_numerator (new_fps);
  s->fps_d = gst_value_get_fraction_denominator (new_fps);

  GST_DEBUG_OBJECT (s, "peer wants %d/%d fps", s->fps_n, s->fps_d);

  return TRUE;
}

static GstCaps *
gst_ximage_src_fixate (GstBaseSrc * bsrc, GstCaps * caps)
{
  gint i;
  GstStructure *structure;

  caps = gst_caps_make_writable (caps);

  for (i = 0; i < gst_caps_get_size (caps); ++i) {
    structure = gst_caps_get_structure (caps, i);

    gst_structure_fixate_field_nearest_fraction (structure, "framerate", 25, 1);
  }
  caps = GST_BASE_SRC_CLASS (parent_class)->fixate (bsrc, caps);

  return caps;
}

static void
gst_ximage_src_class_init (GstXImageSrcClass * klass)
{
  GObjectClass *gc = G_OBJECT_CLASS (klass);
  GstElementClass *ec = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *bc = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *push_class = GST_PUSH_SRC_CLASS (klass);

  gc->set_property = gst_ximage_src_set_property;
  gc->get_property = gst_ximage_src_get_property;
  gc->dispose = gst_ximage_src_dispose;
  gc->finalize = gst_ximage_src_finalize;

  g_object_class_install_property (gc, PROP_DISPLAY_NAME,
      g_param_spec_string ("display-name", "Display", "X Display Name", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gc, PROP_SCREEN_NUM,
      g_param_spec_uint ("screen-num", "Screen number", "X Screen Number",
          0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gc, PROP_SHOW_POINTER,
      g_param_spec_boolean ("show-pointer", "Show Mouse Pointer",
          "Show mouse pointer (if XFixes extension enabled)", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstXImageSrc:use-damage
   *
   * Use XDamage (if the XDamage extension is enabled)
   *
   * Since: 0.10.4
   **/
  g_object_class_install_property (gc, PROP_USE_DAMAGE,
      g_param_spec_boolean ("use-damage", "Use XDamage",
          "Use XDamage (if XDamage extension enabled)", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstXImageSrc:startx
   *
   * X coordinate of top left corner of area to be recorded
   * (0 for top left of screen)
   *
   * Since: 0.10.4
   **/
  g_object_class_install_property (gc, PROP_STARTX,
      g_param_spec_uint ("startx", "Start X co-ordinate",
          "X coordinate of top left corner of area to be recorded (0 for top left of screen)",
          0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstXImageSrc:starty
   *
   * Y coordinate of top left corner of area to be recorded
   * (0 for top left of screen)
   *
   * Since: 0.10.4
   **/
  g_object_class_install_property (gc, PROP_STARTY,
      g_param_spec_uint ("starty", "Start Y co-ordinate",
          "Y coordinate of top left corner of area to be recorded (0 for top left of screen)",
          0, G_MAXINT, 0, G_PARAM_READWRITE));
  /**
   * GstXImageSrc:endx
   *
   * X coordinate of bottom right corner of area to be recorded
   * (0 for bottom right of screen)
   *
   * Since: 0.10.4
   **/
  g_object_class_install_property (gc, PROP_ENDX,
      g_param_spec_uint ("endx", "End X",
          "X coordinate of bottom right corner of area to be recorded (0 for bottom right of screen)",
          0, G_MAXINT, 0, G_PARAM_READWRITE));
  /**
   * GstXImageSrc:endy
   *
   * Y coordinate of bottom right corner of area to be recorded
   * (0 for bottom right of screen)
   *
   * Since: 0.10.4
   **/
  g_object_class_install_property (gc, PROP_ENDY,
      g_param_spec_uint ("endy", "End Y",
          "Y coordinate of bottom right corner of area to be recorded (0 for bottom right of screen)",
          0, G_MAXINT, 0, G_PARAM_READWRITE));

  /**
   * GstXImageSrc:remote
   *
   * Whether the X display is remote. The element will try to use alternate calls
   * known to work better with remote displays.
   *
   * Since: 0.10.26
   **/
  g_object_class_install_property (gc, PROP_REMOTE,
      g_param_spec_boolean ("remote", "Remote dispay",
          "Whether the display is remote", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstXImageSrc:xid
   *
   * The XID of the window to capture. 0 for the root window (default).
   *
   * Since: 0.10.31
   **/
  g_object_class_install_property (gc, PROP_XID,
      g_param_spec_uint64 ("xid", "Window XID",
          "Window XID to capture from", 0, G_MAXUINT64, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstXImageSrc:xname
   *
   * The name of the window to capture, if any.
   *
   * Since: 0.10.31
   **/
  g_object_class_install_property (gc, PROP_XNAME,
      g_param_spec_string ("xname", "Window name",
          "Window name to capture from", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (ec, "Ximage video source",
      "Source/Video",
      "Creates a screenshot video stream",
      "Lutz Mueller <lutz@users.sourceforge.net>, "
      "Jan Schmidt <thaytan@mad.scientist.com>, "
      "Zaheer Merali <zaheerabbas at merali dot org>");
  gst_element_class_add_pad_template (ec, gst_static_pad_template_get (&t));

  bc->fixate = gst_ximage_src_fixate;
  bc->get_caps = gst_ximage_src_get_caps;
  bc->set_caps = gst_ximage_src_set_caps;
  bc->start = gst_ximage_src_start;
  bc->stop = gst_ximage_src_stop;
  bc->unlock = gst_ximage_src_unlock;
  push_class->create = gst_ximage_src_create;
}

static void
gst_ximage_src_init (GstXImageSrc * ximagesrc)
{
  gst_base_src_set_format (GST_BASE_SRC (ximagesrc), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (ximagesrc), TRUE);

  g_mutex_init (&ximagesrc->x_lock);
  ximagesrc->show_pointer = TRUE;
  ximagesrc->use_damage = TRUE;
  ximagesrc->startx = 0;
  ximagesrc->starty = 0;
  ximagesrc->endx = 0;
  ximagesrc->endy = 0;
  ximagesrc->remote = FALSE;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ret;

  GST_DEBUG_CATEGORY_INIT (gst_debug_ximage_src, "ximagesrc", 0,
      "ximagesrc element debug");

  ret = gst_element_register (plugin, "ximagesrc", GST_RANK_NONE,
      GST_TYPE_XIMAGE_SRC);

  return ret;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    ximagesrc,
    "X11 video input plugin using standard Xlib calls",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN);
