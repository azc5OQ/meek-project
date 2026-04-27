#ifndef ANIMATOR_H
#define ANIMATOR_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

//
// animator.h - per-frame animation engine.
//
// Walks the scene tree once per frame, advancing per-node animation
// timers (scroll-momentum-style integration on top of resolved.* values
// that scene__resolve_styles wrote first). Currently supports one
// animation kind:
//
//   :appear opacity fade
//     If a node's resolved.appear_ms > 0, the animator multiplies
//     the node's color alphas (bg.a, fg.a, color.a) by an eased
//     fade factor that ramps 0 -> 1 over appear_ms milliseconds.
//     The factor is computed from the node's own appear_age_ms,
//     which the animator increments by scene__frame_delta_ms each
//     tick.
//
// Future additions plug in here:
//   - :disappear (needs deferred destruction so the animation can play
//     before the node is freed).
//   - transition: <prop> <ms> <easing> for state changes (hover/press).
//     Would track a "from-value" per animatable property + a target,
//     interpolating each frame.
//
// CALL ORDER (each frame):
//   1. platform__tick stamps scene__begin_frame_time(now_ms).
//   2. scene__resolve_styles() writes resolved.* from the per-state
//      style slot.
//   3. animator__tick() walks the tree, advances appear_age_ms by
//      scene__frame_delta_ms, multiplies resolved color alphas by
//      the appear-fade factor.
//   4. scene__layout() computes bounds.
//   5. renderer__begin_frame() / scene__emit_draws() / __end_frame().
//
// Step 3 must come AFTER step 2 (resolve overwrites resolved.*) and
// BEFORE step 5 (emit reads resolved.*).
//

/**
 * Run one frame of animation. Reads scene__frame_delta_ms for the
 * time advance and walks the active scene tree, updating per-node
 * animation state and mutating resolved.* fields in place.
 *
 * @function animator__tick
 * @return {void}
 *
 * Safe to call when the scene root is NULL (no-op).
 */
GUI_API void animator__tick(void);

/**
 * Check whether the scene currently has any in-progress animation
 * (appear-fade shortcut OR :appear pseudo-state per-property
 * interpolation). Walks the tree, returns 1 on the first match,
 * returns 0 if everything is steady-state.
 *
 * Used by render-gating platform backends (e.g.
 * platform_linux_wayland_client.c) to keep rendering at vblank
 * rate while animations are running, even when no input has
 * arrived to flag the scene dirty. Without this, a skipped render
 * would freeze any in-progress fade halfway.
 *
 * @function animator__has_active
 * @return {int} 1 if at least one node is mid-animation, 0 otherwise.
 *
 * Safe to call when the scene root is NULL (returns 0).
 */
GUI_API int animator__has_active(void);

/**
 * Runtime toggle for the size_w / size_h / font_size transition
 * extension. When OFF (default) the animator behaves identically to
 * pre-extension behaviour: only background_color / accent_color /
 * font_color / radius interpolate across `transition: ...` declarations.
 * When ON, the same transition declaration also interpolates explicit
 * px-valued width / height / font-size between rule-defined endpoints.
 *
 * The lerp is gated PER-PROPERTY on style.size_w_explicit /
 * size_h_explicit / font_size_explicit -- a transition only fires
 * when both the from-side and the cur-side are flagged as set by an
 * explicit rule. Stops phantom zero endpoints from corrupting layout.
 *
 * Off by default because flipping it on retroactively can change the
 * appearance of any host with `transition: ...` declarations, which
 * is a behaviour change. Hosts that want size animation opt in.
 *
 * @function animator__set_size_transitions_enabled
 * @param {boole} on - TRUE to enable size/font interpolation.
 * @return {void}
 */
GUI_API void  animator__set_size_transitions_enabled(boole on);

/**
 * Companion getter; returns the current toggle state.
 *
 * @function animator__size_transitions_enabled
 * @return {boole}
 */
GUI_API boole animator__size_transitions_enabled(void);

#endif
