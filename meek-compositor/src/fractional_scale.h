#ifndef MEEK_COMPOSITOR_FRACTIONAL_SCALE_H
#define MEEK_COMPOSITOR_FRACTIONAL_SCALE_H

#include <stdint.h>

//
// fractional_scale.h -- wp_fractional_scale_manager_v1 global.
//
// Protocol: staging/fractional-scale/fractional-scale-v1.xml.
//
// The ingredient libadwaita / GTK4 clients expect on a high-DPI
// display before they'll render: it tells them what scale factor
// to use. Without it, GTK4 on a 450-DPI panel falls back to a
// clean-exit "can't render here" path (amberol silently quits
// after creating xdg_toplevel but before committing any buffer).
//
// Protocol shape:
//
//   interface wp_fractional_scale_manager_v1 {
//       request destroy;
//       request get_fractional_scale(new_id wp_fractional_scale_v1,
//                                     object wl_surface);
//   }
//
//   interface wp_fractional_scale_v1 {
//       request destroy;
//       event    preferred_scale(uint scale);
//           // scale = desired_scale * 120
//           // e.g. 120 = 1.0x, 150 = 1.25x, 180 = 1.5x, 240 = 2.0x
//   }
//
// Per the spec: on every get_fractional_scale, the compositor
// SHOULD send at least one preferred_scale event so the client has
// a definitive scale to render at. If we silently don't fire one,
// some clients hang waiting for it. We fire preferred_scale(120)
// (1.0x) immediately after resource creation.
//
// Level 1 (this file): single fixed scale of 1.0x for every
// surface. Level 2 would query wl_output.scale + physical_size to
// pick a context-aware scale; multi-output compositors also need
// to handle output changes and resend.
//

struct wl_display;

void fractional_scale__register(struct wl_display* display);

//
// Return the currently-resolved preferred_scale value in wire format
// (scale * 120; e.g. 240 == 2.0x). Lazy-resolves on first call if
// fractional_scale__register ran before output_drm__init (which is
// our normal startup order). Safe to call before or after
// fractional_scale__register; returns 0 ONLY if the module is in a
// broken state (panel geometry missing AND env var unset AND the
// fallback logic didn't fire -- shouldn't happen in practice).
//
// Callers that want a floating-point scale divide by 120.0.
//
// Used by xdg_shell.c to compute the logical-size scale-down for
// xdg_toplevel.configure (Level 2 fractional-scale apply). See
// session/design_level2_fractional_scaling.md for the rationale.
//
uint32_t fractional_scale__get_preferred_scale(void);

//
// Return 1 if the given wl_client has bound
// wp_fractional_scale_manager_v1 at least once during its lifetime,
// 0 otherwise.
//
// xdg_shell.c uses this to decide whether it's safe to send a
// scale-adjusted configure size: clients that haven't opted in to
// the fractional-scale protocol can't render at a fractional scale,
// so feeding them a shrunk logical configure would just produce
// a tiny 1:1-scaled window on the panel (bad for meek-shell, which
// must render at panel-native resolution, and bad for meek-ui
// clients which don't react to preferred_scale events).
//
struct wl_client;
int fractional_scale__client_has_bound_manager(struct wl_client* client);

#endif
