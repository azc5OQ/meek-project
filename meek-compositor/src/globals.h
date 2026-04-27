#ifndef MEEK_COMPOSITOR_GLOBALS_H
#define MEEK_COMPOSITOR_GLOBALS_H

//
//globals.h - standard Wayland globals registration.
//
//This module owns the creation of every wl_global that the
//compositor exposes to normal (unprivileged) clients. The gated
//meek_shell_v1 global is NOT registered here -- that comes later
//in a separate meek_shell_v1.c module.
//
//Public surface is one entry point:
//
//  globals__register_all(display)
//    Called once from main.c after wl_display is created. Registers
//    every standard Wayland global on the display. Every subsequent
//    client that connects sees those globals via wl_registry.global
//    events.
//
//The individual globals (wl_compositor, wl_shm, wl_output, wl_seat)
//are hidden behind this one call. When more protocols come online
//(xdg_wm_base, zwp_linux_dmabuf_v1), add their registration inside
//globals.c; no changes needed at the call site.
//

struct wl_display;
struct wl_resource;

void globals__register_all(struct wl_display* display);

//
//Role-hook API for xdg-shell (and future role protocols) to be
//notified on wl_surface.commit.
//
//Why: xdg-shell requires the compositor to defer xdg_surface.configure
//until AFTER the client's initial empty commit (spec, section
//"Initialization"). Since commit handling lives in globals.c (on
//wl_surface) and role state lives in xdg_shell.c (on xdg_surface),
//we need a callback plumbed between them.
//
//Called as:
//    on_commit(role_data)
//after globals.c has done the pending->current buffer swap on the
//surface. Role code inspects its own state and decides what to do
//(xdg_shell fires configure on first empty commit, subsequent
//commits are ignored by the hook).
//
//Installed once per surface by the role protocol (xdg_shell,
//later wlr-layer-shell if we add one). Passing NULL detaches.
//
typedef void (*fncp_wl_surface_role_hook)(void* role_data);

void globals__wl_surface_set_role_hook(
    struct wl_resource*         wl_surface,
    fncp_wl_surface_role_hook   on_commit,
    void*                       role_data);

//
//Touch-event dispatchers called by input.c (libinput backend) to
//forward touch events to all live wl_touch resources. Coordinates
//are in screen pixels (already transformed by libinput's
//get_*_transformed). `id` is the libinput slot (multi-touch).
//Send serials are generated internally via wl_display_next_serial;
//there is no return value.
//
//No-ops when called before globals__register_all or when no
//wl_touch resources are bound.
//
void globals__seat__send_touch_down  (uint32_t time_ms, int32_t id, int32_t x, int32_t y);
void globals__seat__send_touch_up    (uint32_t time_ms, int32_t id);
void globals__seat__send_touch_motion(uint32_t time_ms, int32_t id, int32_t x, int32_t y);
void globals__seat__send_touch_frame (void);
void globals__seat__send_touch_cancel(void);

//
//Directed variants used by Phase 6 input routing. Send a touch
//event to ONLY the wl_touch resources of a specific wl_client
//(the target app the shell wants to deliver input to). Plus
//wl_touch.down needs the target wl_surface the touch is landing
//on -- not just the screen coordinates.
//
//Coordinates here are SURFACE-LOCAL (relative to the target
//surface's own origin), matching the contract of meek_shell_v1's
//route_touch_* requests.
//
//No-ops if `client` or `surface` is NULL, or if no wl_touch
//resource is bound by this client. Frame dispatch is the caller's
//responsibility (usually issued after a down+motion+up sequence).
//
struct wl_client;
struct wl_resource;
void globals__seat__route_touch_down_to_client  (struct wl_client* client, struct wl_resource* surface, uint32_t time_ms, int32_t id, int32_t sx, int32_t sy);
void globals__seat__route_touch_motion_to_client(struct wl_client* client, uint32_t time_ms, int32_t id, int32_t sx, int32_t sy);
void globals__seat__route_touch_up_to_client    (struct wl_client* client, uint32_t time_ms, int32_t id);

//
//Fire all deferred wl_surface.frame callbacks. Intended to be called
//once per display vblank by the output driver (output_drm's page-flip
//handler). Caps client commit rate to the display refresh, preventing
//unpaced clients from flooding the compositor's fd table.
//
void globals__fire_frame_callbacks(uint32_t time_ms);

//
//Directed keyboard routing. Called by meek_shell_v1.c after the
//shell sends route_keyboard_key(app_handle, keycode, state) /
//route_keyboard_modifiers(app_handle, d, l, lk, g). The shell has
//already resolved its "currently focused app" to a handle; this
//module resolves handle -> wl_surface -> wl_client (in
//meek_shell_v1.c) and calls through to here.
//
//The key helper synthesises wl_keyboard.enter on the target surface
//the first time a given (client, keyboard resource) pair receives
//a key, and synthesises leave+enter on focus change. Callers don't
//need to manage that lifecycle.
//
//`keycode` is a Linux evdev keycode (KEY_A = 30 from
//<linux/input-event-codes.h>), NOT an xkbcommon keysym -- the
//client's libxkbcommon translates using the keymap we sent at
//wl_seat.get_keyboard. `state`: 0 = released, 1 = pressed.
//
//Modifier fields match wl_keyboard.modifiers: bit 0 = shift,
//bit 1 = caps, bit 2 = ctrl, bit 3 = alt (as resolved by the
//client's keymap). Shell must send a modifiers(shift=1) before
//route_keyboard_key for a shifted character and modifiers(0) after,
//so the client's xkb state is correct at the moment of translation.
//
//No-ops if the target client has no wl_keyboard resource bound
//(e.g. it never called wl_seat.get_keyboard, or it released the
//keyboard before the shell's forward arrived).
//
void globals__seat__route_keyboard_key_to_client      (struct wl_client* client, struct wl_resource* surface, uint32_t time_ms, uint32_t keycode, uint32_t state);
void globals__seat__route_keyboard_modifiers_to_client(struct wl_client* client, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group);

//
// Fire wl_surface.enter(output) for `surface` on every wl_output
// resource the surface's client has bound. Called by surface.c
// from the commit handler on the first commit that attaches a
// buffer -- matches the Wayland-spec meaning of "surface has
// become visible on this output."
//
// Safe to call multiple times; libwayland-server doesn't dedupe
// enter events but clients are required to handle duplicates per
// protocol, so it's harmless.
//
// Fixes a GTK4 / libadwaita silent-crash path: without this,
// fractional-scale-aware clients receive wp_fractional_scale_v1.
// preferred_scale but have no wl_output to pair it against, and
// the rendering path dereferences a NULL output pointer.
//
void globals__send_output_enter_for_surface(struct wl_resource* surface);

//
// Return the panel's physical display-area dimensions in mm + pixel
// dimensions. Used by fractional_scale.c to auto-pick preferred_scale
// from actual panel DPI instead of a hardcoded constant.
//
// Pixel dims come from output_drm__get_native_size at runtime (DRM
// connector mode), falling back to the compile-time defaults. MM
// dims come from the compile-time constants (Poco F1 display area);
// future work will pull EDID via DRM where available.
//
// All four pointers must be non-NULL.
//
void globals__get_output_geometry(int* w_px, int* h_px, int* w_mm, int* h_mm);

#endif
