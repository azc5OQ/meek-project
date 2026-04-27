#ifndef MEEK_SHELL_V1_CLIENT_H
#define MEEK_SHELL_V1_CLIENT_H

//
// meek_shell_v1_client.h - meek-shell's client side of the
// privileged extension.
//
// What this module does:
//   * Grabs the wl_display already owned by meek-ui's Wayland
//     backend (via platform_wayland__get_display).
//   * Does its own wl_registry round-trip to find the
//     meek_shell_v1 global the compositor advertises (only to us).
//   * Binds it + installs event listeners (stubs in D3 -- they
//     just log what arrives).
//   * Calls announce_ready once listeners are installed so the
//     compositor starts forwarding queued toplevel_added events.
//
// Graceful fallback: if the compositor doesn't advertise
// meek_shell_v1 (because it's not meek-compositor, or because
// meek-compositor rejected our bind on PID check), init() still
// returns success -- meek-shell runs in "shell-chrome-only" mode
// where no app windows are composited into our scene, but our
// own UI still renders. Lets us develop the shell UI against any
// standard Wayland compositor too.
//

struct wl_display;

/**
 * Find and bind meek_shell_v1 on the given display. Must be
 * called after platform__init (so meek-ui has connected) and
 * before the main tick loop.
 *
 * Returns 0 on any outcome that leaves the shell usable:
 *   * Bound + announce_ready sent                 -> fully live.
 *   * Global not advertised (non-meek-compositor) -> degraded
 *                                                    mode, no
 *                                                    app-window
 *                                                    composition.
 * Returns -1 only on catastrophic failure (NULL display,
 * wl_registry allocation failure).
 */
int meek_shell_v1_client__init(struct wl_display *display);

/**
 * Undo init. Destroy the bound resource if any. Safe to call
 * multiple times.
 */
void meek_shell_v1_client__shutdown(void);

/**
 * Returns 1 iff bound and announce_ready was sent. Useful for
 * shell logic that conditionally renders app windows vs
 * shell-chrome-only mode.
 */
int meek_shell_v1_client__is_live(void);

//
// Phase 6 — route input back to the target app. Shell's tap /
// gesture handler calls these to forward a touch event to the
// app whose toplevel has `handle`. Coordinates are SURFACE-LOCAL
// to the target app (relative to its xdg_toplevel's origin).
// Compositor resolves handle → target client → dispatches
// wl_touch.down/motion/up on that client's seat.
//
// No-op if meek_shell_v1 isn't bound (fall back to shell-only
// mode has no apps to route to).
//
#include "types.h"
void meek_shell_v1_client__route_touch_down(uint handle, uint time_ms, int64 id, int64 sx, int64 sy);
void meek_shell_v1_client__route_touch_motion(uint handle, uint time_ms, int64 id, int64 sx, int64 sy);
void meek_shell_v1_client__route_touch_up(uint handle, uint time_ms, int64 id);

//
// Polite close: compositor sends xdg_toplevel.close to the target
// client (analogous to clicking the X button). The app may show
// a save-prompt or refuse. Kept available for non-dismiss flows;
// the swipe-up-to-dismiss gesture uses kill_toplevel below.
//
void meek_shell_v1_client__close_toplevel(uint handle);

//
// Force-quit: compositor sends xdg_toplevel.close immediately, then
// SIGKILLs the client process unconditionally 2 seconds later.
// 2 seconds is enough for well-behaved apps to flush + exit cleanly;
// stragglers get terminated. Used by the swipe-up-to-dismiss
// gesture in the task switcher.
//
void meek_shell_v1_client__kill_toplevel(uint handle);

//
// Tell the client module which app should receive synthesized
// wl_keyboard events from the on-screen keyboard. Called by main.c
// on entering / leaving fullscreen on a tile. Setting to 0
// disables keyboard routing (taps fall through to the
// ime_commit_string path only, for text_input_v3 consumers).
//
// The compositor doesn't track "current focused app" -- that's the
// shell's job per DESIGN.md. We remember it here so every
// widget_keyboard tap can name its target without plumbing the
// handle through meek-ui's scene callbacks.
//
void meek_shell_v1_client__set_keyboard_focus(uint app_handle);

//
// Install / clear the char redirect that forwards widget_keyboard
// taps over meek_shell_v1. install on fullscreen entry; clear on
// exit. These two are symmetric counterparts to
// scene__show_keyboard / scene__hide_keyboard.
//
void meek_shell_v1_client__install_char_redirect(void);
void meek_shell_v1_client__clear_char_redirect(void);

#endif
