#ifndef MEEK_COMPOSITOR_OUTPUT_DRM_H
#define MEEK_COMPOSITOR_OUTPUT_DRM_H

//
//output_drm.h - direct DRM/KMS output on /dev/dri/card0.
//
//The production output backend. Owns the screen end-to-end: takes
//DRM master, sets the connector's preferred mode, renders each
//frame via EGL on a GBM surface, page-flips to the panel.
//
//Sibling of output_x11.h. The two are mutually exclusive at
//runtime; main.c picks one based on MEEK_COMPOSITOR_OUTPUT env var
//(default "drm" when /dev/dri/card0 is openable + $DISPLAY is
//unset, else "x11").
//
//This module OWNS:
//  * an open /dev/dri/card0 fd (DRM master for its lifetime)
//  * a struct gbm_device built from that fd
//  * a struct gbm_surface sized to the panel's preferred mode
//  * an EGL display + context + window surface (on GBM platform,
//    SEPARATE from egl_ctx.c's surfaceless display -- keeping them
//    separate so egl_ctx's dmabuf-validation role stays mode-
//    agnostic and we can swap DRM for X11 without touching it)
//  * the page-flip event fd wired into wl_event_loop
//
//REQUIREMENTS FOR startup:
//  - Must be DRM-master-capable. On pmOS that usually means running
//    as root or being the logind session owner. EBUSY at
//    drmSetMaster means another compositor (or X server) is holding
//    the screen -- stop it first.
//  - No other process should be driving the panel. Lifting DRM
//    master from someone else is not what we do -- they'd drop
//    back into an unhappy state.
//
//RENDER CONTENT (this pass, A10.1 scope):
//  - glClear to a slowly pulsing color every frame. Proves:
//      * DRM master acquisition
//      * connector + mode selection
//      * EGL/GBM setup
//      * page-flip timing (acks arriving, not stalling)
//  - No client content, no shell UI, no input. Those come next.
//
//Later passes (C6+) replace the pulsing color with meek-shell's
//committed surface and start wiring real client buffers through.
//

struct wl_display;
struct wl_resource;

//
//Opens /dev/dri/card0, becomes DRM master, picks the first
//connected connector and its preferred mode, builds the GBM
//surface, creates EGL context + surface on it, arms a render tick
//on wl_event_loop (driven by page-flip events at the panel's
//refresh rate). Returns 0 on success, nonzero on failure.
//
//On failure, leaves no persistent state: caller is free to try
//another output backend or keep running headless.
//
int  output_drm__init(struct wl_display* display);

//
//Drops DRM master, restores the saved CRTC state (so the console
//comes back intact), tears down GBM + EGL. Safe to call multiple
//times and safe from a signal handler context as long as the
//caller wraps this in wl_display_terminate + returns through
//wl_display_run.
//
void output_drm__shutdown(void);

//
//Called from globals.c's wl_surface commit dispatch when the
//committing surface belongs to the privileged shell client (as
//determined by meek_shell_v1__get_shell_client()). `buffer` is the
//wl_buffer the shell just attached; may be NULL if the shell
//committed with no buffer (which we handle by reverting to the
//"waiting for shell" fallback).
//
//This runs on the compositor's event-loop dispatch thread. We do
//the re-import into output_drm's own EGL context here (NOT
//egl_ctx's context -- textures don't cross displays) and update
//the scanout state so the next page-flip samples from the shell's
//buffer instead of the fallback pulsing gradient.
//
//Safe no-op if output_drm hasn't been init'd (non-drm output mode).
//
void output_drm__on_shell_commit(struct wl_resource* buffer);

//
//Flag a screenshot request. Safe to call from a signal handler
//(writes a single volatile sig_atomic_t). The next frame's
//_render_and_present will glReadPixels the rendered-but-not-yet-
//swapped framebuffer and write it as PPM to /tmp/meek-
//screenshot.ppm. Subsequent calls before that frame fires just
//re-raise the same flag (no queuing).
//
//Intended wiring: main.c installs a SIGUSR1 handler that calls
//this. `kill -USR1 <compositor-pid>` from SSH then captures the
//live panel into a file.
//
void output_drm__request_screenshot_from_signal(void);

//
//Writes the panel's native mode dimensions to *w_out / *h_out.
//Returns 1 on success (DRM backend is live + modeset complete),
//0 if DRM isn't the active backend or init hasn't finished -- in
//which case *w_out and *h_out are left untouched.
//
//Callers: xdg_shell's deferred-configure (so we tell the shell to
//render at the panel's native resolution rather than (0,0) = "you
//pick"), and wl_output bind (so every client sees the real panel
//size instead of a hardcoded placeholder).
//
int output_drm__get_native_size(int* w_out, int* h_out);

//
// Mark the output as needing a fresh frame. The damage gate in
// _on_page_flip checks this before scheduling another render --
// without a call here, the panel goes idle after the next flip
// and stays idle until something wakes it.
//
// Wake-up sources:
//   * surface.c: shell client commit (means the shell has a new
//     buffer to present)
//   * fallback paths that need to keep rendering even without a
//     shell (e.g. the "waiting for shell" pulsing gradient runs
//     by self-rescheduling each flip)
//
// When called outside the page-flip handler (e.g. on a fresh
// shell commit while the previous frame is still being scanned
// out), the function checks `waiting_flip`: if a flip is already
// in flight, just sets the flag (the in-progress flip's ack will
// see it and render the next frame); if no flip is pending, kicks
// the loop by calling render_and_present immediately.
//
// Safe no-op if output_drm hasn't been init'd.
//
void output_drm__schedule_frame(void);

#endif
