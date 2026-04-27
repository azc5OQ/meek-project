#ifndef MEEK_COMPOSITOR_MEEK_SHELL_V1_H
#define MEEK_COMPOSITOR_MEEK_SHELL_V1_H

//
// meek_shell_v1.h - privileged-shell extension registration.
//
// The extension is a private protocol between meek-compositor and
// meek-shell. Schema lives in protocols/meek-shell-v1.xml; scanner
// generates server-side bindings into build/protocols/. This
// module creates the wl_global, installs a bind gate that checks
// the connecting client's PID + /proc/PID/exe path against a
// hardcoded allowlist, and (in C5 scope) stubs the event + request
// dispatchers. Real event dispatch (toplevel_added fires on xdg-
// shell toplevel creation; raw input forwards from libinput; etc.)
// lands in C6 + C7.
//

struct wl_display;
struct wl_client;

//
// Register the meek_shell_v1 global. Must be called once from
// globals__register_all, after xdg_wm_base (so the compositor has
// something to track and later forward). Safe to call multiple
// times -- subsequent calls log a warning and no-op.
//
void meek_shell_v1__register(struct wl_display* display);

//
// Returns the wl_client that is currently bound to meek_shell_v1,
// or NULL if no client is bound right now. Used by the scanout
// path in output_drm.c + by globals.c's wl_surface commit hook to
// decide "is this surface the privileged shell's?" without
// introducing a circular include back into meek_shell_v1.c's
// internals.
//
// Return value is only valid until the next libwayland dispatch
// iteration -- the client can disconnect between our check and
// anything we do with the pointer. In practice callers use it
// synchronously within a single request dispatch, so that window
// is tight enough to not matter.
//
struct wl_client* meek_shell_v1__get_shell_client(void);

//
// Fire meek_shell_v1.toplevel_added(handle, app_id, title) to the
// bound shell. No-op if no shell is bound or shell hasn't
// announce_ready'd yet. app_id / title may be NULL, which the
// scanner-generated send_* replaces with the empty string.
//
// Called by xdg_shell.c from get_toplevel (for surfaces that
// belong to OTHER clients -- never the shell itself).
//
#include <stdint.h>
void meek_shell_v1__fire_toplevel_added(uint32_t handle, const char* app_id, const char* title);

//
// Fire toplevel_title_changed. Called when a tracked app updates
// its window title. Safe no-op outside shell-ready window.
//
void meek_shell_v1__fire_toplevel_title_changed(uint32_t handle, const char* title);

//
// Fire toplevel_removed. Called when the tracked xdg_toplevel
// destroys (either app-driven or via client disconnect).
//
void meek_shell_v1__fire_toplevel_removed(uint32_t handle);

//
// Fire toplevel_buffer: dup the buffer's dmabuf plane-0 fd and
// send it to the shell via SCM_RIGHTS. Shell receives, imports as
// EGLImage on its side, draws the resulting texture in its scene.
//
// Silently skips shm buffers for v1 (fallback requires pool-fd
// share logic not yet implemented).
//
// The shell takes ownership of the forwarded fd -- the dup() here
// is what the shell's `close()` eventually balances.
//
struct wl_resource;
void meek_shell_v1__fire_toplevel_buffer(uint32_t handle, struct wl_resource* buffer);

//
// Fire the three raw-touch events to the bound shell. Called from
// input.c's libinput dispatch in addition to the standard wl_touch
// events that go out via wl_seat. The shell uses these to run its
// gesture recognizer on the unified touch stream (wl_touch gives
// the shell only its own focused-surface touches; this gives the
// shell everything libinput sees, including cross-cutting edge
// swipes that might start outside any widget).
//
// Safe no-op if no shell is bound / not yet announce_ready'd.
//
// `time_ms`: CLOCK_MONOTONIC ms (libinput_event_touch_get_time).
// `id`: libinput touch slot / multi-touch finger index. 0 for
//       single-touch, 0..N-1 for multi-touch.
// `x`, `y`: screen-pixel coordinates after orientation transform.
//
void meek_shell_v1__fire_touch_down_raw  (uint32_t time_ms, int32_t id, int32_t x, int32_t y);
void meek_shell_v1__fire_touch_motion_raw(uint32_t time_ms, int32_t id, int32_t x, int32_t y);
void meek_shell_v1__fire_touch_up_raw    (uint32_t time_ms, int32_t id);

//
// Fire meek_shell_v1.ime_request_on / ime_request_off events to the
// bound shell. Called by text_input_v3.c when a third-party client
// enables or disables its zwp_text_input_v3 resource. app_handle
// identifies which app wants IME input (0 = current-focused
// fallback until we plumb per-text_input surface tracking).
//
void meek_shell_v1__fire_ime_request_on (uint32_t app_handle);
void meek_shell_v1__fire_ime_request_off(uint32_t app_handle);

#endif
