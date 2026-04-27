#ifndef MEEK_COMPOSITOR_XDG_SHELL_H
#define MEEK_COMPOSITOR_XDG_SHELL_H

//
//xdg_shell.h - xdg-shell global registration.
//
//WHAT "XDG" IS
//-------------
//"xdg" = cross-desktop group (freedesktop.org). Every Wayland
//compositor implements a tiny core protocol (wl_compositor,
//wl_surface, wl_seat, wl_output, wl_shm) -- just enough to hand
//pixel buffers around and deliver input. But the core protocol
//has NO concept of a window. No title. No min/maximize. No popup
//menus. No "this surface is the top-level application window".
//
//The xdg_* protocols fill that gap. They're the standard
//freedesktop extension that every compositor and every toolkit
//(GTK, Qt, SDL, electron, browsers) agrees on for application-
//level window management. If we don't implement xdg_shell, no
//normal application can open a window on our compositor -- they'll
//bind wl_compositor, fail to find xdg_wm_base, and exit with
//"compositor does not support xdg-shell".
//
//WHAT THIS MODULE REGISTERS
//--------------------------
//A tree of related interfaces:
//
//  xdg_wm_base     -- the entry point. Clients bind this to get
//                     into the xdg ecosystem.
//    |
//    +- xdg_surface  -- the "role layer" on a wl_surface.
//    |    |            Turns a raw pixel surface into a window.
//    |    |            An xdg_surface can only be one role.
//    |    |
//    |    +- xdg_toplevel -- the role that means "normal window".
//    |    |                  Has title, app_id, min/max/close,
//    |    |                  fullscreen, resize handles.
//    |    |
//    |    +- xdg_popup    -- the role for tooltips, dropdown
//    |                       menus, right-click context menus.
//    |                       Positioned relative to a parent.
//    |
//    +- xdg_positioner  -- helper struct used to describe WHERE a
//                          popup should appear (anchored to this
//                          rect, gravity = below, offset by 4px,
//                          ...). Passed to xdg_surface.get_popup.
//
//The wayland-scanner turns xdg-shell.xml (from the
//wayland-protocols package) into generated C code that defines
//these interfaces. This module consumes the generated code and
//provides one public entry point to hand the compositor display
//over to it.
//
//CONFIGURE DANCE
//---------------
//Unique to xdg-shell: windows don't immediately draw. Instead:
//
//  1. Client creates wl_surface + xdg_surface + xdg_toplevel.
//  2. Client does an empty commit (no buffer attached) -- "please
//     tell me how big you want me to be".
//  3. Compositor replies with xdg_toplevel.configure(w, h, states)
//     and xdg_surface.configure(serial).
//  4. Client replies with xdg_surface.ack_configure(serial).
//  5. Client attaches a buffer of the agreed size and commits.
//
//That back-and-forth exists so the compositor controls initial
//window size (tile layouts, fullscreen on small phone screens,
//etc.) rather than each app picking whatever it wants.
//
//Needs wayland-scanner to have produced xdg-shell-protocol.h /
//xdg-shell-protocol.c under build/protocols/ before the compile
//step -- see CMakeLists.txt's wl_scanner_generate() call and
//build.sh's target_scanner().
//

struct wl_display;
struct wl_resource;

#include <stdint.h>

//
//Registers the xdg_wm_base global on `display`. Call once from
//globals__register_all after the core globals. Does nothing if the
//global already exists; does not own the display.
//
void xdg_shell__register(struct wl_display* display);

//
//Look up the meek_shell_v1 handle for the xdg_toplevel attached
//to `wl_surface_resource`. Returns the handle (non-zero) if the
//surface's xdg_surface has a toplevel role AND the toplevel has
//been assigned a handle (i.e., it's NOT a shell-owned toplevel);
//returns 0 otherwise. Used by globals.c's commit hook to decide
//whether to forward a buffer via meek_shell_v1.
//
uint32_t xdg_shell__get_toplevel_handle_for_surface(struct wl_resource* wl_surface_resource);

//
//Reverse lookup: find the wl_surface resource for a given
//meek_shell_v1 handle. Used by Phase 6 input routing: meek_shell_v1
//route_touch_down(handle, ...) hands the compositor a handle, and
//we need the target's wl_surface (for the wl_touch.down surface
//argument) + wl_client (to filter wl_touch resources). NULL if the
//handle isn't a live toplevel.
//
struct wl_resource* xdg_shell__find_wl_surface_by_handle(uint32_t handle);

//
//Sibling of the above for the xdg_toplevel resource itself.
//Used by meek_shell_v1.close_toplevel so the compositor can call
//xdg_toplevel_send_close(handle) on the right resource.
//
struct wl_resource* xdg_shell__find_xdg_toplevel_by_handle(uint32_t handle);

//
//Iterate every currently-live toplevel that has a meek_shell_v1
//handle (i.e. belongs to a non-shell client). For each, invokes
//`cb(handle, app_id_or_null, title_or_null, userdata)`. Used by
//meek_shell_v1 on_announce_ready to replay the current toplevel
//list to the shell (covers shell-binds-after-apps-already-
//connected and shell-crash-then-respawn cases).
//
void xdg_shell__foreach_toplevel_for_replay(
    void (*cb)(uint32_t handle, const char* app_id, const char* title, void* userdata),
    void* userdata);

//
//Clear the meek_shell_v1 handle on every live toplevel that
//belongs to `shell_client`. Used by meek_shell_v1.c when the
//shell successfully binds: any toplevels the shell process
//already created (before the bind) will have been assigned
//handles (because at creation time we didn't know yet that this
//client was the shell). Post-bind we demote them so the shell
//doesn't end up seeing itself as a regular app.
//
//No-op if shell_client is NULL.
//
void xdg_shell__demote_client_toplevels(struct wl_client* shell_client);

//
// Return the last-sent xdg_toplevel.configure(w, h) for the toplevel
// attached to `wl_surface_resource`. Writes the logical size (in
// surface coordinates) into *w and *h. Returns 1 on success, 0 if
// the surface has no toplevel role OR no configure has been sent.
//
// Input-coord translation uses this: wl_touch.down coords are in
// surface-local LOGICAL units, but the shell routes tap events in
// buffer-sample units. Dividing by logical/buffer-ratio recovers
// surface-local coords. See session/design_level2_fractional_scaling.md.
//
int xdg_shell__get_configure_size_for_surface(struct wl_resource* wl_surface_resource, int* w_out, int* h_out);

//
// Re-send xdg_toplevel.configure with scale-adjusted logical size for
// the toplevel on `wl_surface_resource`. Intended to be called from
// fractional_scale.c when a client binds wp_fractional_scale_manager_v1
// + calls get_fractional_scale (meaning: "I will render at the scale
// you told me via preferred_scale"). At that point we know it's safe
// to shrink the logical configure so UI elements become visibly
// bigger on the panel.
//
// No-op if the surface has no toplevel role, the toplevel belongs to
// the shell client (shell always uses panel-native), or the scale
// factor is <= 1.0x.
//
void xdg_shell__reconfigure_with_fractional_scale(struct wl_resource* wl_surface_resource);

//
// Record the buffer dimensions of the latest commit on a toplevel's
// surface. Called from meek_shell_v1.c's toplevel_buffer forwarders
// so route_touch_* can compute the buffer-to-logical ratio. No-op
// if the surface has no toplevel role.
//
void xdg_shell__record_buffer_size(struct wl_resource* wl_surface_resource, int buffer_w, int buffer_h);

//
// Translate tap coordinates from "shell's buffer-sample space" to
// "surface-local logical space" for the toplevel on
// `wl_surface_resource`. Intended flow: shell sends route_touch_down
// with coords scaled by buffer/widget ratio; compositor translates
// by logical/buffer ratio before calling wl_touch.send_down. Returns
// 1 on success with *out_sx / *out_sy set; 0 if data is incomplete
// (no buffer committed, no configure sent) -- caller should pass
// the original coords through in that case.
//
int xdg_shell__translate_tap_coords_for_surface(struct wl_resource* wl_surface_resource, int32_t sx_in, int32_t sy_in, int32_t* out_sx, int32_t* out_sy);

#endif
