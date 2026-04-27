#ifndef MEEK_COMPOSITOR_VIEWPORTER_H
#define MEEK_COMPOSITOR_VIEWPORTER_H

//
// viewporter.h -- wp_viewporter global (Wayland stable protocol).
//
// Pairs with wp_fractional_scale_v1. A client binds wp_viewporter,
// calls get_viewport(wl_surface) to get a wp_viewport per
// wl_surface, then uses wp_viewport to tell the compositor:
//
//   set_source(x, y, w, h)      -- crop rect WITHIN the attached
//                                  buffer (in BUFFER coordinates,
//                                  wl_fixed_t units)
//   set_destination(w, h)       -- DESTINATION rect on the wl_surface
//                                  (in SURFACE coordinates, int32_t)
//
// The effect: the compositor should sample pixels from the source
// rect of the buffer and paint them into the destination rect on
// the surface. GTK clients that support fractional scaling use this
// to render at physical-pixel scale into an oversized buffer and
// then declare a smaller "surface size" so the compositor can
// downscale correctly.
//
// Level 1 implementation (this file): accept all four requests,
// stash per-surface state (source + destination) on the wp_viewport
// resource's user_data. Don't apply the transform yet -- both the
// forwarder to meek-shell and the input-coordinate mapping still
// treat the buffer's pixel size as the authoritative surface size.
// That's fine for the common case where dst.w/h == buffer.w/h
// (i.e. scale == 1.0 via fractional_scale), which is what we
// advertise at preferred_scale=120.
//
// Level 2 (future): surface.c reads the viewport destination to
// know the logical surface size; input routing maps touch coords
// from tile-pixels → surface-logical (using destination, not
// buffer size); compositor scales the sampled buffer region to
// match the destination when painting.
//

struct wl_display;

void viewporter__register(struct wl_display* display);

#endif
