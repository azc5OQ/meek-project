#ifndef PLATFORM_LINUX_WAYLAND_CLIENT_H
#define PLATFORM_LINUX_WAYLAND_CLIENT_H

//
// platform_linux_wayland_client.h - Wayland-client-specific hooks.
//
// Most platform interaction goes through the uniform `platform.h`
// API (platform__init / tick / shutdown). This header is for code
// that needs the one thing the uniform API can't express: access
// to the live `wl_display*` so additional protocol extensions can
// be bound on the same connection.
//
// The canonical consumer today is meek-shell, which binds our
// private `meek_shell_v1` extension alongside the standard ones
// meek-ui's backend already handles. meek-shell doesn't need its
// own wl_display_connect -- sharing the existing one keeps us to
// a single protocol connection per process.
//
// ONLY the Wayland-client backend defines these. Code that links
// the X11 or DRM backends will see `platform_wayland__get_display`
// as an unresolved symbol at link time -- intentional; it's the
// signal that the Wayland-specific path isn't available on this
// build.
//

#include "../../gui_api.h"
#include "../../types.h"

struct wl_display;

/**
 * Return the live Wayland display connection owned by the
 * platform backend. Valid only between platform__init and
 * platform__shutdown -- NULL before / after.
 *
 * @function platform_wayland__get_display
 * @return {struct wl_display*} the connection, or NULL if not initialized.
 */
GUI_API struct wl_display* platform_wayland__get_display(void);

/**
 * Expose the backend's EGL display + context as opaque pointers so
 * downstream code (meek-shell's meek_shell_v1 client) can
 * `eglCreateImage(EGL_LINUX_DMA_BUF_EXT, ...)` dmabuf fds forwarded
 * by the compositor and bind the resulting EGLImage as a GL texture
 * via `glEGLImageTargetTexture2DOES`. Textures created against this
 * context can be sampled by the same meek-ui renderer that's running
 * the shell's scene, without any cross-display EGL surgery.
 *
 * Returned pointers are EGLDisplay / EGLContext, cast to void* to
 * avoid dragging EGL headers into every consumer of this header.
 * Caller casts back: `EGLDisplay d = (EGLDisplay)platform_wayland__get_egl_display();`
 *
 * Both are NULL before `platform__init` and after `platform__shutdown`.
 *
 * @function platform_wayland__get_egl_display
 * @return {void*} EGLDisplay, or NULL.
 *
 * @function platform_wayland__get_egl_context
 * @return {void*} EGLContext, or NULL.
 */
GUI_API void* platform_wayland__get_egl_display(void);
GUI_API void* platform_wayland__get_egl_context(void);

/**
 * Mark the platform's render loop as obliged to run a render block
 * on its next tick, regardless of the gate's normal idle decision.
 * Called by code OUTSIDE the platform file when scene-relevant
 * state has changed but isn't observable through scene__on_*
 * (which already wakes the loop). Examples:
 *   - meek-shell's meek_shell_v1_client.c receiving a foreign-app
 *     buffer event, toplevel add/remove, IME request, etc.
 *   - hot reload reloading a .ui or .style file mid-loop.
 * Without this hook, those state changes wouldn't be reflected on
 * screen until some unrelated input event woke the gate.
 *
 * Idempotent + side-effect-free except for the internal flag write.
 * Safe no-op before platform__init or after platform__shutdown.
 *
 * @function platform_wayland__request_render
 * @return {void}
 */
GUI_API void platform_wayland__request_render(void);

/**
 * Read the current wl_output mode's pixel dimensions. Both arguments
 * are populated with the latched values from the most recent
 * `wl_output.mode` event carrying the WL_OUTPUT_MODE_CURRENT flag --
 * i.e. the panel's native pixel size.
 *
 * Returns FALSE (and writes 0 to both out params) when the
 * compositor hasn't sent a current-mode event yet, when wl_output
 * isn't bound, or before platform__init / after platform__shutdown.
 * Callers that need a value before that point (e.g. shell-side
 * gesture recognizers initialised at startup) should use FALSE as
 * the cue to fall back to the cfg width/height.
 *
 * Output is in panel-native pixels, NOT logical / scale-adjusted
 * pixels. That's the right value for code that translates raw
 * libinput-derived touch coords (which the compositor forwards
 * unscaled) -- and the wrong value for code laying out widgets in
 * the meek-ui logical coordinate space.
 *
 * @function platform_wayland__get_output_pixel_size
 * @param {int*} out_w - Receives the current mode's pixel width.
 * @param {int*} out_h - Receives the current mode's pixel height.
 * @return {boole} TRUE if a real value was written, FALSE on fallback.
 */
GUI_API boole platform_wayland__get_output_pixel_size(int* out_w, int* out_h);

#endif
