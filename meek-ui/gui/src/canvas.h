#ifndef CANVAS_H
#define CANVAS_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

//
// canvas.h - host API for drawing into a <canvas> widget.
//
// Usage:
//   gui_node* cv = scene__find_by_id("sketch");
//   canvas__clear(cv, scene__rgb(0.06f, 0.06f, 0.08f));
//   canvas__stroke_line(cv, 10, 10, 200, 40, scene__rgb(1,1,1));
//   canvas__fill_circle(cv, 100, 100, 20, scene__rgba(1,0,0,1));
//
// All coordinates are in CANVAS PIXEL SPACE (the backing buffer's
// own resolution, set by width= / height= attributes), NOT screen
// pixels. For paint-app mouse handling, convert screen coords with
// canvas__screen_to_pixel first.
//
// Mutations flag the widget dirty; the next frame re-uploads the
// pixel buffer and renders it.
//

/** Clear the entire canvas to a solid color. */
GUI_API void canvas__clear(gui_node* n, gui_color c);

/** Write one pixel. Out-of-bounds coords are silently clipped. */
GUI_API void canvas__set_pixel(gui_node* n, int x, int y, gui_color c);

/** Filled axis-aligned rectangle. */
GUI_API void canvas__fill_rect(gui_node* n, int x, int y, int w, int h, gui_color c);

/** One-pixel-wide Bresenham line. */
GUI_API void canvas__stroke_line(gui_node* n, int x0, int y0, int x1, int y1, gui_color c);

/** Filled disc. */
GUI_API void canvas__fill_circle(gui_node* n, int cx, int cy, int radius, gui_color c);

/** Current canvas resolution in pixels. Either out may be NULL. */
GUI_API void canvas__size(gui_node* n, int* out_w, int* out_h);

/**
 * Convert a mouse/touch event's screen coordinate into canvas-local
 * pixel coords. Accounts for the canvas widget's bounds being scaled
 * (either by the style `size` or the UI scale slider) relative to
 * the backing buffer's pixel resolution.
 */
GUI_API void canvas__screen_to_pixel(gui_node* n, int64 sx, int64 sy, int* out_px, int* out_py);

#endif
