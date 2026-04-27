//
// platforms/linux/platform_linux_wayland_client.c - Wayland-client backend.
//
// Third Linux backend alongside platform_linux_x11.c (nested in an
// X session) and platform_linux_drm.c (kiosk / no display manager).
// This one runs the gui toolkit as a regular Wayland CLIENT: it
// connects to whatever compositor is already up (any standard
// Wayland compositor, including meek-compositor), asks for a window
// via xdg-shell, and renders into an EGL window surface.
//
// WHY THIS EXISTS:
//   Most modern Linux desktops run Wayland compositors natively.
//   Going through XWayland's X11 translation (which is what
//   platform_linux_x11.c does under Wayland) means an extra bounce
//   + loses features the compositor only exposes natively (fractional
//   scaling, proper fullscreen, xdg_toplevel state events). It also
//   unblocks meek-shell: meek-shell uses meek-ui as a library and
//   needs to present through Wayland to talk to meek-compositor.
//
// WHAT IS xdg-shell?
//   "xdg" = cross-desktop group (freedesktop.org). The base Wayland
//   protocol has no notion of a window with a title, min/maximize,
//   fullscreen, etc. xdg-shell is the standard extension that every
//   compositor implements for application-level window management.
//   Without binding xdg_wm_base we'd have a surface that the
//   compositor doesn't know what to do with.
//
// RENDERER CHOICE:
//   Same gles3_renderer.c every other Linux / Android backend uses.
//   Context-neutral so we just hand it the current EGL context.
//
// WHAT'S IN THIS PASS (B1 MVP):
//   - Connect, bind wl_compositor + xdg_wm_base + wl_output.
//   - Create a wl_surface, xdg_surface, xdg_toplevel.
//   - Initial empty commit -> wait for configure -> ack.
//   - wl_egl_window + eglCreateWindowSurface on it (Mesa handles
//     buffer allocation + attach+commit internally via
//     eglSwapBuffers, which is why we don't hand-roll dmabuf
//     submission here).
//   - Main loop: dispatch + render + swap.
//   - xdg_toplevel.close -> should_close. Ping/pong for liveness.
//
// DEFERRED (B1.2+):
//   - Input: wl_seat / wl_pointer / wl_keyboard / wl_touch.
//   - Keyboard layout via libxkbcommon.
//   - Fractional scaling via wp_fractional_scale_v1.
//   - Explicit dmabuf submission + linux-dmabuf v1 bind. Not needed
//     as long as Mesa's wl_egl_window path works (it does on every
//     compositor we care about).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>       //non-blocking wl_display_read_events drain in tick loop.
#include <dlfcn.h>      //dlsym(RTLD_DEFAULT, ...) for UI_HANDLER symbol resolution.
#include <sys/mman.h>   //mmap the keymap fd the compositor sends us.

#include <xkbcommon/xkbcommon.h>  //translate wl_keyboard.key events to codepoints.

#include <wayland-client.h>
#include <wayland-egl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

//
// scanner-generated client-side bindings. CMakeLists.txt runs
// wayland-scanner on /usr/share/wayland-protocols/stable/xdg-shell/
// xdg-shell.xml with `client-header` + `private-code`, depositing
// these under the demo's build dir. The demo's include path
// exposes them.
//
#include "xdg-shell-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "animator.h"
#include "renderer.h"
#include "widget_registry.h"
#include "widgets/widget_image_cache.h"
#include "font.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"

#define _PLATFORM_INTERNAL
#include "platform.h"
#undef _PLATFORM_INTERNAL

#include "platforms/linux/platform_linux_wayland_client.h"

//
// Emit the real int main() that forwards to the host's (renamed)
// app_main. Without this the linker errors with "undefined reference
// to main" when musl's Scrt1.o tries to call it. Matches the pattern
// in platform_linux_drm.c / platform_linux_x11.c.
//
#include "platforms/_main_trampoline.h"
GUI_DEFINE_MAIN_TRAMPOLINE()

//============================================================================
// state
//============================================================================

typedef struct _platform_linux_wayland_client_internal__state
{
    //
    // Wayland-level handles. wl_display owns the socket; the rest
    // are per-global references obtained during the registry burst.
    //
    struct wl_display*    display;
    struct wl_registry*   registry;
    struct wl_compositor* compositor;
    struct xdg_wm_base*   wm_base;
    struct wl_output*     output; //optional; advertised size / scale.

    //
    // Captured wl_output.mode pixel dimensions. Phones / single-output
    // setups latch the active mode here. Both 0 until the compositor
    // sends the first wl_output.mode event with the WL_OUTPUT_MODE_CURRENT
    // bit set; consumers (e.g. meek-shell's gesture_recognizer__init,
    // which needs panel-native pixels for edge-zone classification)
    // should fall back to cfg width/height when these are still 0.
    //
    int output_pixel_w;
    int output_pixel_h;

    //
    // zwp_idle_inhibit_manager_v1: tells the compositor "don't
    // blank the screen while my surface is visible". Optional --
    // if the compositor doesn't advertise it we fall through
    // silently (the user's session will just auto-blank). The
    // manager is bound at registry time; the actual inhibitor
    // object is created per-surface once the xdg_surface is up.
    //
    struct zwp_idle_inhibit_manager_v1* idle_inhibit_manager;
    struct zwp_idle_inhibitor_v1*       idle_inhibitor;

    //
    // Input: wl_seat wraps pointer + keyboard + touch. Each
    // capability is optional; we grab each one when the seat
    // advertises it. On a typical phone only touch + keyboard
    // are present (phone has no physical mouse); on dev desktop
    // pointer + keyboard. Tracking the last pointer/touch
    // position here so we can synthesise release events at the
    // correct spot when wl_touch.up fires (which carries no
    // coordinates of its own).
    //
    struct wl_seat*     seat;
    struct wl_pointer*  pointer;
    struct wl_touch*    touch;
    struct wl_keyboard* keyboard;
    int64               last_x;
    int64               last_y;
    int32_t             touch_active_id; // -1 when no finger down
    int64               touch_x;
    int64               touch_y;

    //
    // xkbcommon state. Compositor sends a keymap via
    // wl_keyboard.keymap(XKB_V1, fd, size); we mmap the fd, parse
    // with xkb_keymap_new_from_string, and use the resulting
    // keymap + state to translate incoming wl_keyboard.key
    // keycodes into unicode codepoints + VK_ values.
    //
    // Lifecycle: xkb_context is created once at platform init, kept
    // until shutdown. xkb_keymap + xkb_state are re-created on
    // every wl_keyboard.keymap event (the compositor may swap
    // layouts at runtime). NULL when no keymap has arrived yet.
    //
    struct xkb_context* xkb_ctx;
    struct xkb_keymap*  xkb_keymap;
    struct xkb_state*   xkb_state;

    //
    // Our window. xdg-shell is a "role" layered on top of
    // wl_surface: you create a surface first, then wrap it as an
    // xdg_surface, then pick a role (toplevel here). Closing
    // happens via xdg_toplevel.close arriving; destroying the
    // toplevel also destroys its children down the tree.
    //
    struct wl_surface*    wl_surface;
    struct xdg_surface*   xdg_surface;
    struct xdg_toplevel*  xdg_toplevel;

    //
    // EGL backing. wl_egl_window wraps our wl_surface so EGL can
    // hand Mesa a buffer to draw into; Mesa then attach+commits
    // to the compositor inside eglSwapBuffers. We never manually
    // call wl_surface_attach -- that would duplicate what EGL
    // already did.
    //
    struct wl_egl_window* egl_window;
    EGLDisplay            egl_display;
    EGLContext            egl_context;
    EGLSurface            egl_surface;
    EGLConfig             egl_config;

    //
    // Lifecycle flags. `configured` flips TRUE on the first
    // xdg_surface.configure -- until then we don't render. Before
    // configure, rendering would attach a buffer to a surface
    // whose role hasn't been confirmed yet, which is a protocol
    // error on strict compositors.
    //
    int  viewport_w;
    int  viewport_h;
    boole should_close;
    boole configured;

    gui_color clear_color;

    //
    // Render-gating state (see comment block above platform__tick
    // for the design). force_render is 1 when something has
    // happened that obliges us to render the next tick (input
    // event, configure resize, hot reload, meek_shell_v1 event
    // arriving via platform_wayland__request_render). The gate
    // also keeps rendering while animator__has_active() returns 1
    // (a fade / transition is in flight) -- that's checked at
    // the gate inline rather than via this struct.
    //
    // last_activity_ms is the CLOCK_MONOTONIC ms timestamp of the
    // most recent activity (input event, animator-driven render,
    // foreign-app buffer arrival, hot reload). The poll timeout
    // uses this for hysteresis: until ACTIVITY_TIMEOUT_MS has
    // passed with no activity, we treat the system as ACTIVE and
    // poll non-blockingly so animations + rapid input bursts run
    // at full vblank rate AND the render block runs on every tick
    // (because some animator types -- transitions, auto-repeat --
    // aren't caught by animator__has_active and need an
    // unconditional tick to advance). After the activity window
    // ages out we drop into IDLE mode and may skip both the render
    // block and block in poll for IDLE_TIMEOUT_MS so the loop
    // doesn't busy-spin while waiting for input.
    //
    // gating_disabled mirrors the MEEK_RENDER_GATING env var; if
    // "off", we render every tick like before Phase 2 (kill switch).
    //
    int force_render;
    int gating_disabled;
    int64 last_activity_ms;
} _platform_linux_wayland_client_internal__state;

static _platform_linux_wayland_client_internal__state _platform_linux_wayland_client_internal__g;

//============================================================================
// forward decls
//============================================================================

static boole _platform_linux_wayland_client_internal__open_display(void);
static void  _platform_linux_wayland_client_internal__close_display(void);
static boole _platform_linux_wayland_client_internal__bind_globals(void);
static boole _platform_linux_wayland_client_internal__make_toplevel(const gui_app_config* cfg);
static boole _platform_linux_wayland_client_internal__init_egl(void);
static void  _platform_linux_wayland_client_internal__term_egl(void);
static gui_handler_fn _platform_linux_wayland_client_internal__resolve_host_symbol(char* name);

//-- render gating (Phase 2): force_render setter
static void _platform_linux_wayland_client_internal__force_render_now(void);

//-- wl_output listener (current-mode pixel dimensions capture).
static void _platform_linux_wayland_client_internal__on_output_geometry(
    void* data, struct wl_output* o, int32_t x, int32_t y,
    int32_t physical_width, int32_t physical_height, int32_t subpixel,
    const char* make, const char* model, int32_t transform);
static void _platform_linux_wayland_client_internal__on_output_mode(
    void* data, struct wl_output* o, uint32_t flags,
    int32_t width, int32_t height, int32_t refresh);
static void _platform_linux_wayland_client_internal__on_output_done(
    void* data, struct wl_output* o);
static void _platform_linux_wayland_client_internal__on_output_scale(
    void* data, struct wl_output* o, int32_t factor);

//
// Listener callbacks. All Wayland protocol events are dispatched
// through registered listener structs of function pointers --
// similar in shape to wl_resource vtables on the compositor side.
//
static void _platform_linux_wayland_client_internal__on_registry_global(
    void* data, struct wl_registry* r, uint32_t name, const char* interface, uint32_t version);
static void _platform_linux_wayland_client_internal__on_registry_global_remove(
    void* data, struct wl_registry* r, uint32_t name);

static void _platform_linux_wayland_client_internal__on_wm_base_ping(
    void* data, struct xdg_wm_base* wm, uint32_t serial);

static void _platform_linux_wayland_client_internal__on_xdg_surface_configure(
    void* data, struct xdg_surface* s, uint32_t serial);

static void _platform_linux_wayland_client_internal__on_toplevel_configure(
    void* data, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states);
static void _platform_linux_wayland_client_internal__on_toplevel_close(
    void* data, struct xdg_toplevel* t);
static void _platform_linux_wayland_client_internal__on_toplevel_configure_bounds(
    void* data, struct xdg_toplevel* t, int32_t w, int32_t h);
static void _platform_linux_wayland_client_internal__on_toplevel_wm_capabilities(
    void* data, struct xdg_toplevel* t, struct wl_array* caps);

//-- seat/pointer/touch
static void _platform_linux_wayland_client_internal__on_seat_capabilities(void* data, struct wl_seat* s, uint32_t caps);
static void _platform_linux_wayland_client_internal__on_seat_name(void* data, struct wl_seat* s, const char* name);

static void _platform_linux_wayland_client_internal__on_pointer_enter(void* data, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y);
static void _platform_linux_wayland_client_internal__on_pointer_leave(void* data, struct wl_pointer* p, uint32_t serial, struct wl_surface* s);
static void _platform_linux_wayland_client_internal__on_pointer_motion(void* data, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void _platform_linux_wayland_client_internal__on_pointer_button(void* data, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
static void _platform_linux_wayland_client_internal__on_pointer_axis(void* data, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value);
static void _platform_linux_wayland_client_internal__on_pointer_frame(void* data, struct wl_pointer* p);
static void _platform_linux_wayland_client_internal__on_pointer_axis_source(void* data, struct wl_pointer* p, uint32_t source);
static void _platform_linux_wayland_client_internal__on_pointer_axis_stop(void* data, struct wl_pointer* p, uint32_t time, uint32_t axis);
static void _platform_linux_wayland_client_internal__on_pointer_axis_discrete(void* data, struct wl_pointer* p, uint32_t axis, int32_t discrete);

static void _platform_linux_wayland_client_internal__on_touch_down(void* data, struct wl_touch* t, uint32_t serial, uint32_t time, struct wl_surface* s, int32_t id, wl_fixed_t x, wl_fixed_t y);
static void _platform_linux_wayland_client_internal__on_touch_up(void* data, struct wl_touch* t, uint32_t serial, uint32_t time, int32_t id);
static void _platform_linux_wayland_client_internal__on_touch_motion(void* data, struct wl_touch* t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y);
static void _platform_linux_wayland_client_internal__on_touch_frame(void* data, struct wl_touch* t);
static void _platform_linux_wayland_client_internal__on_touch_cancel(void* data, struct wl_touch* t);
static void _platform_linux_wayland_client_internal__on_touch_shape(void* data, struct wl_touch* t, int32_t id, wl_fixed_t major, wl_fixed_t minor);
static void _platform_linux_wayland_client_internal__on_touch_orientation(void* data, struct wl_touch* t, int32_t id, wl_fixed_t orientation);

static void _platform_linux_wayland_client_internal__on_keyboard_keymap(void* data, struct wl_keyboard* k, uint32_t format, int32_t fd, uint32_t size);
static void _platform_linux_wayland_client_internal__on_keyboard_enter(void* data, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys);
static void _platform_linux_wayland_client_internal__on_keyboard_leave(void* data, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s);
static void _platform_linux_wayland_client_internal__on_keyboard_key(void* data, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void _platform_linux_wayland_client_internal__on_keyboard_modifiers(void* data, struct wl_keyboard* k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
static void _platform_linux_wayland_client_internal__on_keyboard_repeat_info(void* data, struct wl_keyboard* k, int32_t rate, int32_t delay);

//============================================================================
// listener tables
//============================================================================

static const struct wl_registry_listener _platform_linux_wayland_client_internal__registry_listener = {
    .global        = _platform_linux_wayland_client_internal__on_registry_global,
    .global_remove = _platform_linux_wayland_client_internal__on_registry_global_remove,
};

static const struct xdg_wm_base_listener _platform_linux_wayland_client_internal__wm_base_listener = {
    .ping = _platform_linux_wayland_client_internal__on_wm_base_ping,
};

static const struct xdg_surface_listener _platform_linux_wayland_client_internal__xdg_surface_listener = {
    .configure = _platform_linux_wayland_client_internal__on_xdg_surface_configure,
};

static const struct xdg_toplevel_listener _platform_linux_wayland_client_internal__toplevel_listener = {
    .configure         = _platform_linux_wayland_client_internal__on_toplevel_configure,
    .close             = _platform_linux_wayland_client_internal__on_toplevel_close,
    .configure_bounds  = _platform_linux_wayland_client_internal__on_toplevel_configure_bounds,
    .wm_capabilities   = _platform_linux_wayland_client_internal__on_toplevel_wm_capabilities,
};

static const struct wl_seat_listener _platform_linux_wayland_client_internal__seat_listener = {
    .capabilities = _platform_linux_wayland_client_internal__on_seat_capabilities,
    .name         = _platform_linux_wayland_client_internal__on_seat_name,
};

static const struct wl_pointer_listener _platform_linux_wayland_client_internal__pointer_listener = {
    .enter         = _platform_linux_wayland_client_internal__on_pointer_enter,
    .leave         = _platform_linux_wayland_client_internal__on_pointer_leave,
    .motion        = _platform_linux_wayland_client_internal__on_pointer_motion,
    .button        = _platform_linux_wayland_client_internal__on_pointer_button,
    .axis          = _platform_linux_wayland_client_internal__on_pointer_axis,
    .frame         = _platform_linux_wayland_client_internal__on_pointer_frame,
    .axis_source   = _platform_linux_wayland_client_internal__on_pointer_axis_source,
    .axis_stop     = _platform_linux_wayland_client_internal__on_pointer_axis_stop,
    .axis_discrete = _platform_linux_wayland_client_internal__on_pointer_axis_discrete,
};

static const struct wl_touch_listener _platform_linux_wayland_client_internal__touch_listener = {
    .down        = _platform_linux_wayland_client_internal__on_touch_down,
    .up          = _platform_linux_wayland_client_internal__on_touch_up,
    .motion      = _platform_linux_wayland_client_internal__on_touch_motion,
    .frame       = _platform_linux_wayland_client_internal__on_touch_frame,
    .cancel      = _platform_linux_wayland_client_internal__on_touch_cancel,
    .shape       = _platform_linux_wayland_client_internal__on_touch_shape,
    .orientation = _platform_linux_wayland_client_internal__on_touch_orientation,
};

static const struct wl_output_listener _platform_linux_wayland_client_internal__output_listener = {
    .geometry = _platform_linux_wayland_client_internal__on_output_geometry,
    .mode     = _platform_linux_wayland_client_internal__on_output_mode,
    .done     = _platform_linux_wayland_client_internal__on_output_done,
    .scale    = _platform_linux_wayland_client_internal__on_output_scale,
};

static const struct wl_keyboard_listener _platform_linux_wayland_client_internal__keyboard_listener = {
    .keymap      = _platform_linux_wayland_client_internal__on_keyboard_keymap,
    .enter       = _platform_linux_wayland_client_internal__on_keyboard_enter,
    .leave       = _platform_linux_wayland_client_internal__on_keyboard_leave,
    .key         = _platform_linux_wayland_client_internal__on_keyboard_key,
    .modifiers   = _platform_linux_wayland_client_internal__on_keyboard_modifiers,
    .repeat_info = _platform_linux_wayland_client_internal__on_keyboard_repeat_info,
};

//============================================================================
// registry handlers
//============================================================================

//
// Fires once per global the compositor advertises. We look for
// the names we care about and bind them. Everything else is
// ignored -- compositors advertise dozens of extensions we don't
// use.
//
static void _platform_linux_wayland_client_internal__on_registry_global(
    void* data, struct wl_registry* r, uint32_t name, const char* interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
        //
        // Cap at 4: versions 5+ add wl_surface.offset + damage_buffer
        // behavior we don't depend on yet.
        //
        uint32_t v = version < 4 ? version : 4;
        _platform_linux_wayland_client_internal__g.compositor =
            wl_registry_bind(r, name, &wl_compositor_interface, v);
    }
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
    {
        //
        // Cap at 5 to match meek-compositor; most compositors in
        // the wild are on 5 or 6.
        //
        uint32_t v = version < 5 ? version : 5;
        _platform_linux_wayland_client_internal__g.wm_base =
            wl_registry_bind(r, name, &xdg_wm_base_interface, v);
        xdg_wm_base_add_listener(_platform_linux_wayland_client_internal__g.wm_base,
                                 &_platform_linux_wayland_client_internal__wm_base_listener,
                                 NULL);
    }
    else if (strcmp(interface, wl_output_interface.name) == 0)
    {
        //
        // Cap at 2 (first version with scale events; newer versions
        // add name/description we don't consume). Listener latches
        // the current mode's pixel dimensions for downstream
        // consumers via platform_wayland__get_output_pixel_size.
        //
        uint32_t v = version < 2 ? version : 2;
        _platform_linux_wayland_client_internal__g.output =
            wl_registry_bind(r, name, &wl_output_interface, v);
        wl_output_add_listener(_platform_linux_wayland_client_internal__g.output,
                               &_platform_linux_wayland_client_internal__output_listener,
                               NULL);
    }
    else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0)
    {
        //
        // Idle-inhibit v1 has only ever been v1. Capping anyway in
        // case the compositor advertises a future revision -- we
        // implement v1's surface, so bind v1.
        //
        uint32_t v = version < 1 ? version : 1;
        _platform_linux_wayland_client_internal__g.idle_inhibit_manager =
            wl_registry_bind(r, name, &zwp_idle_inhibit_manager_v1_interface, v);
        log_info("platform_linux_wayland_client: zwp_idle_inhibit_manager_v1 available (screen-blank inhibit supported)");
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
        //
        // wl_seat v7 adds the modern axis_value120 event for
        // smooth scrolling. We only need v5 (axis_discrete) but
        // bind up to v7 if offered; compositor sends us what it
        // supports, client handlers ignore events we didn't
        // register for.
        //
        uint32_t v = version < 7 ? version : 7;
        _platform_linux_wayland_client_internal__g.seat =
            wl_registry_bind(r, name, &wl_seat_interface, v);
        wl_seat_add_listener(_platform_linux_wayland_client_internal__g.seat,
                             &_platform_linux_wayland_client_internal__seat_listener,
                             NULL);
        _platform_linux_wayland_client_internal__g.touch_active_id = -1;
        log_info("platform_linux_wayland_client: wl_seat v%u bound", v);
    }
}

static void _platform_linux_wayland_client_internal__on_registry_global_remove(
    void* data, struct wl_registry* r, uint32_t name)
{
    (void)data; (void)r; (void)name;
    //
    // Compositor tore down a global. If it's wl_output that means
    // a monitor was unplugged; we'd want to re-query size. Not
    // handled in this MVP.
    //
}

//============================================================================
// ping / pong liveness
//============================================================================

//
// Compositor pings every few seconds to check we haven't frozen.
// Reply immediately; if we don't, the compositor may kill us.
//
static void _platform_linux_wayland_client_internal__on_wm_base_ping(
    void* data, struct xdg_wm_base* wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

//============================================================================
// xdg_surface configure (always ack immediately)
//============================================================================

//
// Compositor sends xdg_surface.configure(serial) after we commit
// the xdg_surface, to say "here's the state you should be in;
// acknowledge then commit an actual buffer." We always ack right
// away; toplevel-specific state (size) was already processed via
// xdg_toplevel.configure which fires right before this one.
//
static void _platform_linux_wayland_client_internal__on_xdg_surface_configure(
    void* data, struct xdg_surface* s, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(s, serial);
    _platform_linux_wayland_client_internal__g.configured = TRUE;
}

//============================================================================
// xdg_toplevel events
//============================================================================

//
// Size hint from the compositor. w == 0 || h == 0 means "pick
// your own" (typical on first configure before we've told it
// anything). Resize our EGL backing buffer if the size changed.
//
static void _platform_linux_wayland_client_internal__on_toplevel_configure(
    void* data, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states)
{
    (void)data; (void)t; (void)states;

    if (w > 0 && h > 0 &&
        (w != _platform_linux_wayland_client_internal__g.viewport_w ||
         h != _platform_linux_wayland_client_internal__g.viewport_h))
    {
        _platform_linux_wayland_client_internal__g.viewport_w = w;
        _platform_linux_wayland_client_internal__g.viewport_h = h;
        if (_platform_linux_wayland_client_internal__g.egl_window != NULL)
        {
            //
            // wl_egl_window_resize nudges Mesa to allocate new
            // backing buffers at the next eglSwapBuffers. Offsets
            // are for sub-surface positioning which we don't use.
            //
            wl_egl_window_resize(_platform_linux_wayland_client_internal__g.egl_window,
                                 w, h, /*dx*/ 0, /*dy*/ 0);
        }
        //
        // Wake the render loop -- a resize means layout will change
        // and we owe the compositor a frame at the new dimensions
        // even if nothing else marked us dirty.
        //
        _platform_linux_wayland_client_internal__g.force_render = 1;
    }
}

static void _platform_linux_wayland_client_internal__on_toplevel_close(
    void* data, struct xdg_toplevel* t)
{
    (void)data; (void)t;
    log_info("platform_linux_wayland_client: compositor requested close");
    _platform_linux_wayland_client_internal__g.should_close = TRUE;
}

//
// configure_bounds was added in v4 -- the compositor's hint about
// the maximum size we could sanely request (output size minus
// decorations / panels). We ignore it for now.
//
static void _platform_linux_wayland_client_internal__on_toplevel_configure_bounds(
    void* data, struct xdg_toplevel* t, int32_t w, int32_t h)
{
    (void)data; (void)t; (void)w; (void)h;
}

//
// wm_capabilities (v5+) tells us which title-bar-menu actions the
// compositor supports (maximize / minimize / fullscreen / menu).
// We don't render our own decorations yet; ignore.
//
static void _platform_linux_wayland_client_internal__on_toplevel_wm_capabilities(
    void* data, struct xdg_toplevel* t, struct wl_array* caps)
{
    (void)data; (void)t; (void)caps;
}

//============================================================================
// connection + registry
//============================================================================

static boole _platform_linux_wayland_client_internal__open_display(void)
{
    //
    // NULL passes $WAYLAND_DISPLAY / $XDG_RUNTIME_DIR to libwayland.
    //
    _platform_linux_wayland_client_internal__g.display = wl_display_connect(NULL);
    if (_platform_linux_wayland_client_internal__g.display == NULL)
    {
        log_error("wl_display_connect failed (is WAYLAND_DISPLAY set? "
                  "XDG_RUNTIME_DIR=%s)",
                  getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)");
        return FALSE;
    }

    _platform_linux_wayland_client_internal__g.registry =
        wl_display_get_registry(_platform_linux_wayland_client_internal__g.display);
    wl_registry_add_listener(_platform_linux_wayland_client_internal__g.registry,
                             &_platform_linux_wayland_client_internal__registry_listener,
                             NULL);

    //
    // Two round-trips: first to fetch the list of globals, second
    // to process any events that binding them triggered (e.g.
    // wl_output geometry events firing right after we bound the
    // output). Without the second roundtrip our global list is
    // complete but no state has settled.
    //
    if (wl_display_roundtrip(_platform_linux_wayland_client_internal__g.display) < 0 ||
        wl_display_roundtrip(_platform_linux_wayland_client_internal__g.display) < 0)
    {
        log_error("wl_display_roundtrip failed during registry sweep");
        return FALSE;
    }

    return TRUE;
}

static boole _platform_linux_wayland_client_internal__bind_globals(void)
{
    if (_platform_linux_wayland_client_internal__g.compositor == NULL)
    {
        log_error("compositor did not advertise wl_compositor");
        return FALSE;
    }
    if (_platform_linux_wayland_client_internal__g.wm_base == NULL)
    {
        log_error("compositor did not advertise xdg_wm_base "
                  "(not an xdg-shell-capable compositor)");
        return FALSE;
    }
    //
    // wl_output is advisory -- we can run without knowing the
    // output geometry, the compositor will size us via toplevel
    // configure.
    //
    return TRUE;
}

static boole _platform_linux_wayland_client_internal__make_toplevel(const gui_app_config* cfg)
{
    _platform_linux_wayland_client_internal__g.wl_surface =
        wl_compositor_create_surface(_platform_linux_wayland_client_internal__g.compositor);
    if (_platform_linux_wayland_client_internal__g.wl_surface == NULL)
    {
        log_error("wl_compositor_create_surface failed");
        return FALSE;
    }

    _platform_linux_wayland_client_internal__g.xdg_surface =
        xdg_wm_base_get_xdg_surface(_platform_linux_wayland_client_internal__g.wm_base,
                                    _platform_linux_wayland_client_internal__g.wl_surface);
    if (_platform_linux_wayland_client_internal__g.xdg_surface == NULL)
    {
        log_error("xdg_wm_base_get_xdg_surface failed");
        return FALSE;
    }
    xdg_surface_add_listener(_platform_linux_wayland_client_internal__g.xdg_surface,
                             &_platform_linux_wayland_client_internal__xdg_surface_listener,
                             NULL);

    _platform_linux_wayland_client_internal__g.xdg_toplevel =
        xdg_surface_get_toplevel(_platform_linux_wayland_client_internal__g.xdg_surface);
    if (_platform_linux_wayland_client_internal__g.xdg_toplevel == NULL)
    {
        log_error("xdg_surface_get_toplevel failed");
        return FALSE;
    }
    xdg_toplevel_add_listener(_platform_linux_wayland_client_internal__g.xdg_toplevel,
                              &_platform_linux_wayland_client_internal__toplevel_listener,
                              NULL);

    //
    // Title + app_id. app_id is how compositors group windows
    // (taskbars, alt-tab lists) -- a stable reverse-DNS-looking
    // string is conventional. Cast cfg->title from wchar_t* to a
    // UTF-8 buffer when set; for now we only have a placeholder.
    //
    (void)cfg; //TODO: wchar_t -> UTF-8 conversion for xdg_toplevel_set_title.
    xdg_toplevel_set_title(_platform_linux_wayland_client_internal__g.xdg_toplevel, "meek-ui");
    xdg_toplevel_set_app_id(_platform_linux_wayland_client_internal__g.xdg_toplevel, "com.meek.ui");

    //
    // Request fullscreen unless MEEK_NO_FULLSCREEN=1 is in the env.
    // Passing NULL for the output lets the compositor pick. Shells
    // that act as a phone's primary UI want all the screen they can
    // get; tiling compositors honour this and give us the whole
    // output. The escape hatch is there because nested-dev on a
    // desktop sometimes wants a tile-sized window, not a fullscreen
    // takeover of the dev machine's display.
    //
    char* no_fs = getenv("MEEK_NO_FULLSCREEN");
    if (no_fs == NULL || no_fs[0] == 0 || no_fs[0] == '0')
    {
        xdg_toplevel_set_fullscreen(_platform_linux_wayland_client_internal__g.xdg_toplevel, NULL);
        log_info("platform_linux_wayland_client: requested fullscreen (set MEEK_NO_FULLSCREEN=1 to skip)");
    }

    //
    // Idle-inhibit: if the compositor advertised the manager in the
    // registry burst, create a per-surface inhibitor now. The
    // inhibitor is automatically active as long as the surface is
    // visible; closing the surface (or destroying the inhibitor)
    // releases the inhibit. Envvar MEEK_NO_IDLE_INHIBIT=1 opts out
    // -- useful when running on a dev desktop where you DO want
    // your screensaver to fire during testing.
    //
    char* no_inhibit = getenv("MEEK_NO_IDLE_INHIBIT");
    if (_platform_linux_wayland_client_internal__g.idle_inhibit_manager != NULL &&
        (no_inhibit == NULL || no_inhibit[0] == 0 || no_inhibit[0] == '0'))
    {
        _platform_linux_wayland_client_internal__g.idle_inhibitor =
            zwp_idle_inhibit_manager_v1_create_inhibitor(
                _platform_linux_wayland_client_internal__g.idle_inhibit_manager,
                _platform_linux_wayland_client_internal__g.wl_surface);
        if (_platform_linux_wayland_client_internal__g.idle_inhibitor != NULL)
        {
            log_info("platform_linux_wayland_client: idle-inhibit active (screen stays on while surface visible; set MEEK_NO_IDLE_INHIBIT=1 to skip)");
        }
        else
        {
            log_warn("platform_linux_wayland_client: idle-inhibit manager advertised but create_inhibitor failed");
        }
    }
    else if (_platform_linux_wayland_client_internal__g.idle_inhibit_manager == NULL)
    {
        log_info("platform_linux_wayland_client: compositor does not advertise idle-inhibit; screen may auto-blank");
    }

    //
    // Initial default size before the compositor configures.
    // Needed so wl_egl_window_create has concrete dimensions.
    //
    _platform_linux_wayland_client_internal__g.viewport_w = (int)cfg->width;
    _platform_linux_wayland_client_internal__g.viewport_h = (int)cfg->height;

    //
    // Empty initial commit: "please configure me." The xdg-shell
    // protocol requires this before we attach a real buffer.
    //
    wl_surface_commit(_platform_linux_wayland_client_internal__g.wl_surface);

    //
    // Wait for the compositor's configure + our ack to complete.
    // After this roundtrip, `configured` is TRUE and we can begin
    // rendering.
    //
    while (!_platform_linux_wayland_client_internal__g.configured)
    {
        if (wl_display_dispatch(_platform_linux_wayland_client_internal__g.display) < 0)
        {
            log_error("wl_display_dispatch failed waiting for configure");
            return FALSE;
        }
    }

    return TRUE;
}

//============================================================================
// EGL
//============================================================================

static boole _platform_linux_wayland_client_internal__init_egl(void)
{
    _platform_linux_wayland_client_internal__g.egl_display =
        eglGetDisplay((EGLNativeDisplayType)_platform_linux_wayland_client_internal__g.display);
    if (_platform_linux_wayland_client_internal__g.egl_display == EGL_NO_DISPLAY)
    {
        log_error("eglGetDisplay returned EGL_NO_DISPLAY");
        return FALSE;
    }

    EGLint egl_major = 0, egl_minor = 0;
    if (!eglInitialize(_platform_linux_wayland_client_internal__g.egl_display,
                       &egl_major, &egl_minor))
    {
        log_error("eglInitialize failed (0x%x)", eglGetError());
        return FALSE;
    }
    log_info("platform_linux_wayland_client: EGL %d.%d", egl_major, egl_minor);

    if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
        log_error("eglBindAPI(GLES) failed");
        return FALSE;
    }

    //
    // Ask for a standard RGBA8 window-surface config with depth
    // (the renderer may not use depth, but requesting it doesn't
    // cost anything and saves us if gles3_renderer evolves to
    // need it).
    //
    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(_platform_linux_wayland_client_internal__g.egl_display,
                         cfg_attrs,
                         &_platform_linux_wayland_client_internal__g.egl_config,
                         1, &n) || n == 0)
    {
        log_error("eglChooseConfig found no matching configs");
        return FALSE;
    }

    EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE,
    };
    _platform_linux_wayland_client_internal__g.egl_context = eglCreateContext(
        _platform_linux_wayland_client_internal__g.egl_display,
        _platform_linux_wayland_client_internal__g.egl_config,
        EGL_NO_CONTEXT,
        ctx_attrs);
    if (_platform_linux_wayland_client_internal__g.egl_context == EGL_NO_CONTEXT)
    {
        log_error("eglCreateContext failed (0x%x)", eglGetError());
        return FALSE;
    }

    //
    // wl_egl_window adapts our wl_surface into something EGL's
    // platform layer can make a window surface against. Mesa's
    // wayland-egl backend handles buffer allocation + dmabuf
    // attach on eglSwapBuffers.
    //
    _platform_linux_wayland_client_internal__g.egl_window = wl_egl_window_create(
        _platform_linux_wayland_client_internal__g.wl_surface,
        _platform_linux_wayland_client_internal__g.viewport_w,
        _platform_linux_wayland_client_internal__g.viewport_h);
    if (_platform_linux_wayland_client_internal__g.egl_window == NULL)
    {
        log_error("wl_egl_window_create failed");
        return FALSE;
    }

    _platform_linux_wayland_client_internal__g.egl_surface = eglCreateWindowSurface(
        _platform_linux_wayland_client_internal__g.egl_display,
        _platform_linux_wayland_client_internal__g.egl_config,
        (EGLNativeWindowType)_platform_linux_wayland_client_internal__g.egl_window,
        NULL);
    if (_platform_linux_wayland_client_internal__g.egl_surface == EGL_NO_SURFACE)
    {
        log_error("eglCreateWindowSurface failed (0x%x)", eglGetError());
        return FALSE;
    }

    if (!eglMakeCurrent(_platform_linux_wayland_client_internal__g.egl_display,
                        _platform_linux_wayland_client_internal__g.egl_surface,
                        _platform_linux_wayland_client_internal__g.egl_surface,
                        _platform_linux_wayland_client_internal__g.egl_context))
    {
        log_error("eglMakeCurrent failed (0x%x)", eglGetError());
        return FALSE;
    }

    return TRUE;
}

static void _platform_linux_wayland_client_internal__term_egl(void)
{
    if (_platform_linux_wayland_client_internal__g.egl_display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(_platform_linux_wayland_client_internal__g.egl_display,
                       EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (_platform_linux_wayland_client_internal__g.egl_surface != EGL_NO_SURFACE)
    {
        eglDestroySurface(_platform_linux_wayland_client_internal__g.egl_display,
                          _platform_linux_wayland_client_internal__g.egl_surface);
        _platform_linux_wayland_client_internal__g.egl_surface = EGL_NO_SURFACE;
    }
    if (_platform_linux_wayland_client_internal__g.egl_context != EGL_NO_CONTEXT)
    {
        eglDestroyContext(_platform_linux_wayland_client_internal__g.egl_display,
                          _platform_linux_wayland_client_internal__g.egl_context);
        _platform_linux_wayland_client_internal__g.egl_context = EGL_NO_CONTEXT;
    }
    if (_platform_linux_wayland_client_internal__g.egl_display != EGL_NO_DISPLAY)
    {
        eglTerminate(_platform_linux_wayland_client_internal__g.egl_display);
        _platform_linux_wayland_client_internal__g.egl_display = EGL_NO_DISPLAY;
    }
    if (_platform_linux_wayland_client_internal__g.egl_window != NULL)
    {
        wl_egl_window_destroy(_platform_linux_wayland_client_internal__g.egl_window);
        _platform_linux_wayland_client_internal__g.egl_window = NULL;
    }
}

//============================================================================
// connection teardown
//============================================================================

static void _platform_linux_wayland_client_internal__close_display(void)
{
    //
    // Release idle-inhibit first so the compositor is allowed to
    // blank the screen again after we go away. Destroying the
    // inhibitor object is the "release" operation; the manager
    // itself is just a factory and gets torn down at the end.
    //
    if (_platform_linux_wayland_client_internal__g.idle_inhibitor != NULL)
    {
        zwp_idle_inhibitor_v1_destroy(_platform_linux_wayland_client_internal__g.idle_inhibitor);
        _platform_linux_wayland_client_internal__g.idle_inhibitor = NULL;
    }
    if (_platform_linux_wayland_client_internal__g.idle_inhibit_manager != NULL)
    {
        zwp_idle_inhibit_manager_v1_destroy(_platform_linux_wayland_client_internal__g.idle_inhibit_manager);
        _platform_linux_wayland_client_internal__g.idle_inhibit_manager = NULL;
    }

    //
    // Seat input objects. wl_pointer_release + wl_touch_release
    // are the "graceful" destructors (added in seat v3+); they
    // tell the compositor we won't receive any more events before
    // the seat object itself dies.
    //
    if (_platform_linux_wayland_client_internal__g.pointer != NULL)
    {
        wl_pointer_release(_platform_linux_wayland_client_internal__g.pointer);
        _platform_linux_wayland_client_internal__g.pointer = NULL;
    }
    if (_platform_linux_wayland_client_internal__g.touch != NULL)
    {
        wl_touch_release(_platform_linux_wayland_client_internal__g.touch);
        _platform_linux_wayland_client_internal__g.touch = NULL;
    }
    if (_platform_linux_wayland_client_internal__g.seat != NULL)
    {
        wl_seat_release(_platform_linux_wayland_client_internal__g.seat);
        _platform_linux_wayland_client_internal__g.seat = NULL;
    }

    if (_platform_linux_wayland_client_internal__g.xdg_toplevel != NULL)
    {
        xdg_toplevel_destroy(_platform_linux_wayland_client_internal__g.xdg_toplevel);
        _platform_linux_wayland_client_internal__g.xdg_toplevel = NULL;
    }
    if (_platform_linux_wayland_client_internal__g.xdg_surface != NULL)
    {
        xdg_surface_destroy(_platform_linux_wayland_client_internal__g.xdg_surface);
        _platform_linux_wayland_client_internal__g.xdg_surface = NULL;
    }
    if (_platform_linux_wayland_client_internal__g.wl_surface != NULL)
    {
        wl_surface_destroy(_platform_linux_wayland_client_internal__g.wl_surface);
        _platform_linux_wayland_client_internal__g.wl_surface = NULL;
    }
    if (_platform_linux_wayland_client_internal__g.output != NULL)
    {
        wl_output_destroy(_platform_linux_wayland_client_internal__g.output);
        _platform_linux_wayland_client_internal__g.output = NULL;
    }
    if (_platform_linux_wayland_client_internal__g.wm_base != NULL)
    {
        xdg_wm_base_destroy(_platform_linux_wayland_client_internal__g.wm_base);
        _platform_linux_wayland_client_internal__g.wm_base = NULL;
    }
    if (_platform_linux_wayland_client_internal__g.compositor != NULL)
    {
        wl_compositor_destroy(_platform_linux_wayland_client_internal__g.compositor);
        _platform_linux_wayland_client_internal__g.compositor = NULL;
    }
    if (_platform_linux_wayland_client_internal__g.registry != NULL)
    {
        wl_registry_destroy(_platform_linux_wayland_client_internal__g.registry);
        _platform_linux_wayland_client_internal__g.registry = NULL;
    }
    if (_platform_linux_wayland_client_internal__g.display != NULL)
    {
        wl_display_disconnect(_platform_linux_wayland_client_internal__g.display);
        _platform_linux_wayland_client_internal__g.display = NULL;
    }
}

//============================================================================
// host symbol resolver (UI_HANDLER bindings)
//============================================================================

static gui_handler_fn _platform_linux_wayland_client_internal__resolve_host_symbol(char* name)
{
    //
    // Same dlsym(RTLD_DEFAULT, name) pattern the X11 + DRM
    // backends use. Handlers are UI_HANDLER-marked in the host's
    // main.c, which keeps them exported even under
    // -fvisibility=hidden.
    //
    void* sym = NULL;
#if defined(__linux__)
    sym = dlsym(RTLD_DEFAULT, name);
#endif
    return (gui_handler_fn)sym;
}

//============================================================================
// public API
//============================================================================

boole platform__init(const gui_app_config* cfg)
{
    memory_manager__init();

    memset(&_platform_linux_wayland_client_internal__g, 0,
           sizeof(_platform_linux_wayland_client_internal__g));
    _platform_linux_wayland_client_internal__g.egl_display = EGL_NO_DISPLAY;
    _platform_linux_wayland_client_internal__g.egl_context = EGL_NO_CONTEXT;
    _platform_linux_wayland_client_internal__g.egl_surface = EGL_NO_SURFACE;

    //
    // Render-gating defaults: force_render=1 so the very first
    // platform__tick after init produces a frame regardless of
    // anything else. The gating_disabled flag mirrors the
    // MEEK_RENDER_GATING env var as a hard kill switch -- set
    // "off" / "0" / "no" to fall back to pre-Phase-2 always-render
    // behaviour without rebuilding.
    //
    _platform_linux_wayland_client_internal__g.force_render = 1;
    {
        const char* gating = getenv("MEEK_RENDER_GATING");
        if (gating != NULL && (
                strcmp(gating, "off") == 0 ||
                strcmp(gating, "0")   == 0 ||
                strcmp(gating, "no")  == 0))
        {
            _platform_linux_wayland_client_internal__g.gating_disabled = 1;
            log_info("platform_linux_wayland_client: render gating DISABLED via MEEK_RENDER_GATING=%s", gating);
        }
    }

    if (cfg == NULL)
    {
        log_error("platform__init: cfg is NULL");
        memory_manager__shutdown();
        return FALSE;
    }
    _platform_linux_wayland_client_internal__g.clear_color = cfg->clear_color;

    if (!_platform_linux_wayland_client_internal__open_display())       { goto fail; }
    if (!_platform_linux_wayland_client_internal__bind_globals())       { goto fail; }
    if (!_platform_linux_wayland_client_internal__make_toplevel(cfg))   { goto fail; }
    if (!_platform_linux_wayland_client_internal__init_egl())           { goto fail; }

    if (!renderer__init(NULL))
    {
        log_error("platform__init: renderer__init failed");
        goto fail;
    }

    widget_registry__bootstrap_builtins();
    if (!font__init())
    {
        log_error("platform__init: font__init failed");
        renderer__shutdown();
        goto fail;
    }

    scene__set_symbol_resolver(_platform_linux_wayland_client_internal__resolve_host_symbol);

    log_info("platform_linux_wayland_client: up (%dx%d)",
             _platform_linux_wayland_client_internal__g.viewport_w,
             _platform_linux_wayland_client_internal__g.viewport_h);
    return TRUE;

fail:
    _platform_linux_wayland_client_internal__term_egl();
    _platform_linux_wayland_client_internal__close_display();
    memory_manager__shutdown();
    return FALSE;
}

//
// Internal force-render setter. Called from this file's wl_seat
// listeners (after every scene__on_*) and from the configure
// resize path. The PUBLIC equivalent for cross-module callers
// (meek-shell's meek_shell_v1_client.c, hot reload, etc.) is
// platform_wayland__request_render below.
//
static void _platform_linux_wayland_client_internal__force_render_now(void)
{
    _platform_linux_wayland_client_internal__g.force_render = 1;
}

//
// Public wake hook. See header docstring. Used by code outside
// this file to mark scene state as dirty when the change isn't
// observable through the standard scene__on_* path.
//
void platform_wayland__request_render(void)
{
    _platform_linux_wayland_client_internal__g.force_render = 1;
}

boole platform_wayland__get_output_pixel_size(int* out_w, int* out_h)
{
    int w = _platform_linux_wayland_client_internal__g.output_pixel_w;
    int h = _platform_linux_wayland_client_internal__g.output_pixel_h;
    if (out_w != NULL) { *out_w = w; }
    if (out_h != NULL) { *out_h = h; }
    return (w > 0 && h > 0) ? TRUE : FALSE;
}

//
// === Render gating (Phase 2) ===
//
// platform__tick used to run resolve_styles + animator + layout +
// renderer + eglSwapBuffers on EVERY tick at whatever rate the
// host's loop pumped it (in meek-shell that's a tight non-blocking
// while). The gate skips that whole block when nothing has changed
// since the last render. The poll() timeout below is the second
// half of the same fix: when the gate would skip, we also block in
// poll() so the host's loop doesn't busy-spin between skipped ticks.
//
// Wake sources (set force_render = 1):
//   * scene__on_* input dispatch from this file's wl_seat listeners
//   * xdg_toplevel.configure size change
//   * platform_wayland__request_render() from outside
//   * animator__has_active() returning 1 (override; checked inline)
//
// Idle behaviour: when ACTIVITY_TIMEOUT_MS has passed with no
// actual rendering AND the gate would otherwise skip the render
// block, the tick blocks in poll() for up to IDLE_TIMEOUT_MS so
// the loop doesn't busy-spin. CPU in that state is near zero --
// the kernel's poll() is what we're sleeping in, not a userspace
// loop.
//
// While ACTIVE (within ACTIVITY_TIMEOUT_MS of the last render),
// poll uses a 0-timeout: animations + rapid input bursts get
// full vblank-rate rendering without any extra wait. The
// distinction matters: a 250 ms poll-sleep mid-animation makes
// the animation look like 4 fps. We only allow the sleep when
// the system has been visually quiet for ACTIVITY_TIMEOUT_MS,
// which means any frame we're about to draw is a wake-from-idle
// frame, not part of a continuous animation.
//
// 5 s ACTIVITY window matches what the user picked: long enough
// that a brief pause mid-animation doesn't drop us into idle, short
// enough that battery-savings kick in within seconds of the user
// stopping interacting.
//
#define _PLATFORM_LINUX_WAYLAND_CLIENT_INTERNAL__ACTIVITY_TIMEOUT_MS 10000
#define _PLATFORM_LINUX_WAYLAND_CLIENT_INTERNAL__IDLE_TIMEOUT_MS       250

boole platform__tick(void)
{
    if (_platform_linux_wayland_client_internal__g.should_close) { return FALSE; }

    //
    // Pump any queued Wayland events. dispatch_pending() processes
    // events ALREADY in the read queue; we follow it with a read
    // so new events land there for next tick. Combined with the
    // flush-after-render below this gives a tick shape that
    // neither starves nor busy-waits.
    //
    // -1 return means the connection went away (compositor died or
    // our protocol misuse closed us). Fail fast; the next tick
    // would just keep erroring.
    //
    //
    // CRITICAL: wl_display_dispatch_pending ONLY processes events
    // already read into the client queue. It does NOT read new
    // events from the socket. Without an explicit read pass, the
    // compositor's events (including wl_buffer.release for every
    // frame our eglSwapBuffers committed) pile up in the kernel
    // socket buffer. Mesa's wl_egl_window allocates new wl_buffers
    // on every swap when its pool is "empty" from Mesa's POV --
    // because it never sees the release events that would return
    // buffers to the pool. After ~4000 frames Mesa hits its
    // internal VkImage budget with
    //   `CreateSwapchainKHR: VK_ERROR_OUT_OF_HOST_MEMORY`.
    //
    // Fix: non-blocking read of any pending socket data before
    // dispatch. The prepare/read/dispatch trio is libwayland's
    // canonical drain pattern; we poll with a 0-timeout so it's
    // still non-blocking (a render-driven loop shouldn't block
    // waiting for events).
    //
    struct wl_display* dpy = _platform_linux_wayland_client_internal__g.display;
    while (wl_display_prepare_read(dpy) != 0)
    {
        if (wl_display_dispatch_pending(dpy) < 0)
        {
            log_error("wl_display_dispatch_pending: connection lost (errno=%d); terminating", errno);
            _platform_linux_wayland_client_internal__g.should_close = TRUE;
            return FALSE;
        }
    }
    wl_display_flush(dpy);

    //
    // Phase-2 poll-timeout selection.
    //
    // Three cases:
    //   1. gate disabled OR force_render set OR animator running:
    //      We KNOW we'll render this tick -> timeout = 0 (just
    //      drain any events that arrived since last poll, no wait).
    //   2. Gate would skip BUT we rendered recently (within
    //      ACTIVITY_TIMEOUT_MS): also timeout = 0. Animations + rapid
    //      input bursts produce frames at vblank rate; an extra
    //      250 ms wait between them looks like 4 fps. We only allow
    //      the sleep when the system has been visually quiet for
    //      ACTIVITY_TIMEOUT_MS.
    //   3. Gate would skip AND nothing's rendered for >= ACTIVITY_
    //      TIMEOUT_MS: timeout = IDLE_TIMEOUT_MS (true idle, sleep
    //      and let the kernel wake us on the next event).
    //
    // Any Wayland event arriving during the block wakes poll
    // immediately. The handlers we register (wl_seat listeners here,
    // meek_shell_v1 listeners in the shell calling
    // platform_wayland__request_render) set force_render which the
    // gate re-evaluates below.
    //
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    int64 now_ms = (int64)ts_now.tv_sec * 1000 + (int64)(ts_now.tv_nsec / 1000000);

    //
    // Pre-poll timeout. Block only if nothing has happened for at
    // least ACTIVITY_TIMEOUT_MS AND nothing's forcing us to render
    // right now. ANY of the following counts as "something happening":
    //   * force_render set (input, configure, foreign-buffer, etc.)
    //   * animator__has_active (appear/disappear in flight)
    //   * last_activity_ms was bumped within the window (recent
    //     prior activity that hasn't aged out yet)
    //
    int64 since_activity =
        now_ms - _platform_linux_wayland_client_internal__g.last_activity_ms;
    if (_platform_linux_wayland_client_internal__g.force_render
        || animator__has_active())
    {
        _platform_linux_wayland_client_internal__g.last_activity_ms = now_ms;
        since_activity = 0;
    }

    int poll_timeout_ms = 0;
    if (_platform_linux_wayland_client_internal__g.gating_disabled == 0 &&
        since_activity >= _PLATFORM_LINUX_WAYLAND_CLIENT_INTERNAL__ACTIVITY_TIMEOUT_MS)
    {
        poll_timeout_ms = _PLATFORM_LINUX_WAYLAND_CLIENT_INTERNAL__IDLE_TIMEOUT_MS;
    }

    struct pollfd pfd = { .fd = wl_display_get_fd(dpy), .events = POLLIN, .revents = 0 };
    int pr = poll(&pfd, 1, poll_timeout_ms);
    if (pr > 0 && (pfd.revents & POLLIN))
    {
        if (wl_display_read_events(dpy) < 0)
        {
            log_error("wl_display_read_events: connection lost (errno=%d); terminating", errno);
            _platform_linux_wayland_client_internal__g.should_close = TRUE;
            return FALSE;
        }
    }
    else
    {
        wl_display_cancel_read(dpy);
    }

    if (wl_display_dispatch_pending(dpy) < 0)
    {
        log_error("wl_display_dispatch_pending: connection lost (errno=%d); terminating",
                  errno);
        _platform_linux_wayland_client_internal__g.should_close = TRUE;
        return FALSE;
    }
    if (_platform_linux_wayland_client_internal__g.should_close) { return FALSE; }

    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64 now_ms = (int64)ts.tv_sec * 1000 + (int64)(ts.tv_nsec / 1000000);
        scene__begin_frame_time(now_ms);
    }

    //
    // Re-evaluate activity AFTER dispatch. The top-of-tick check
    // ran before poll(), so it could not see force_render / animator
    // signals raised by handlers that fired during
    // wl_display_dispatch_pending() above (input events,
    // configure, IPC wake-ups, etc.). If we don't pick those up
    // here, last_activity_ms remains anchored at the moment of the
    // last "naturally observed" signal, since_activity stays >=
    // ACTIVITY_TIMEOUT_MS forever, and every subsequent tick is
    // stuck at the 250 ms idle cadence even though the user is
    // actively interacting -- exactly the "only fast for first 10
    // seconds, then slow forever" symptom.
    //
    // After this bump, the gate sees a fresh signal from THIS
    // tick's dispatch and the activity window is restored.
    //
    {
        struct timespec ts2;
        clock_gettime(CLOCK_MONOTONIC, &ts2);
        int64 now_ms2 = (int64)ts2.tv_sec * 1000 + (int64)(ts2.tv_nsec / 1000000);
        if (_platform_linux_wayland_client_internal__g.force_render
            || animator__has_active())
        {
            _platform_linux_wayland_client_internal__g.last_activity_ms = now_ms2;
            since_activity = 0;
        }
        else
        {
            since_activity =
                now_ms2 - _platform_linux_wayland_client_internal__g.last_activity_ms;
        }
    }

    //
    // Render gate (Phase 2 v3). The user's spec: "if pixels changed
    // on screen within the last 10 seconds, no timeout, full speed.
    // Otherwise, sleep." -- which means we must NOT skip the render
    // block during the active window. animator__has_active is
    // incomplete (it only tracks appear/disappear; transitions,
    // widget auto-repeat timers, and other animators aren't caught),
    // so during the active window we always render and let
    // eglSwapBuffers' triple-buffer back-pressure pace us.
    //
    // Skip the render block ONLY when the system has been visually
    // quiet for ACTIVITY_TIMEOUT_MS AND nothing's forcing a render.
    // That's the true-idle case where blocking in poll above
    // already happened; if we got woken and still have nothing to
    // do (no force, no animator), don't bother rendering.
    //
    int can_skip_render =
        _platform_linux_wayland_client_internal__g.gating_disabled == 0 &&
        _platform_linux_wayland_client_internal__g.force_render    == 0 &&
        animator__has_active() == 0 &&
        since_activity >= _PLATFORM_LINUX_WAYLAND_CLIENT_INTERNAL__ACTIVITY_TIMEOUT_MS;

    if (can_skip_render)
    {
        //
        // Idle path. Still flush queued requests so the compositor
        // sees anything we sent during dispatch (e.g. xdg_surface.
        // ack_configure) without having to wait for the next render.
        //
        wl_display_flush(_platform_linux_wayland_client_internal__g.display);
        return TRUE;
    }

    //
    // Render path. Clear the force flag so a single forced render
    // doesn't keep us awake past its useful frame.
    //
    // last_activity_ms is NOT bumped here. It's bumped only by real
    // activity signals (force_render set, animator active) at the
    // top of the tick. Bumping on every render would make the
    // window self-perpetuating: each render resets the clock, so
    // we'd never reach the idle threshold even when nothing is
    // happening except our own redraws of the same scene.
    //
    _platform_linux_wayland_client_internal__g.force_render = 0;

    int64 vw = (int64)_platform_linux_wayland_client_internal__g.viewport_w;
    int64 vh = (int64)_platform_linux_wayland_client_internal__g.viewport_h;

    scene__resolve_styles();
    animator__tick();
    scene__layout(vw, vh);

    renderer__begin_frame(vw, vh, _platform_linux_wayland_client_internal__g.clear_color);
    scene__emit_draws();
    renderer__end_frame();

    //
    // eglSwapBuffers on a wl_egl_window surface internally does
    // wl_surface.attach(new_buffer) + damage + commit. We never
    // touch wl_surface directly for buffer handoff -- that would
    // double-commit.
    //
    eglSwapBuffers(_platform_linux_wayland_client_internal__g.egl_display,
                   _platform_linux_wayland_client_internal__g.egl_surface);

    //
    // Periodic glFinish (every N ticks) forces the GL driver to
    // flush + wait on its command queue. On Mesa's Zink backend
    // (GLES-over-Vulkan, which is what Adreno uses here), per-frame
    // eglSwapBuffers accumulates Vulkan semaphores / swapchain
    // state that isn't reclaimed until a synchronous wait point.
    // Without this, long-running clients hit
    // `CreateSwapchainKHR: VK_ERROR_OUT_OF_HOST_MEMORY` around the
    // 80--120 s mark. glFinish stalls the pipeline briefly every
    // ~1 s at 60 Hz, which is invisible to the user but keeps the
    // driver's memory pressure bounded.
    //
    static int _swap_counter = 0;
    if ((++_swap_counter % 60) == 0) { glFinish(); }

    //
    // Flush queued client->server traffic so the compositor sees
    // this frame's commit + any queued requests before our next
    // dispatch_pending.
    //
    wl_display_flush(_platform_linux_wayland_client_internal__g.display);
    return TRUE;
}

void platform__shutdown(void)
{
    widget_image__cache_shutdown();
    font__shutdown();
    renderer__shutdown();
    _platform_linux_wayland_client_internal__term_egl();
    _platform_linux_wayland_client_internal__close_display();
    memory_manager__shutdown();
}

void platform__set_topmost(void)
{
    //
    // Wayland clients don't control their own z-order -- that's
    // the compositor's job. No-op.
    //
}

boole platform__capture_bmp(const char* path)
{
    (void)path;
    //
    // Wayland has no XGetImage equivalent for clients to grab
    // their own window. screencopy protocols exist but require
    // separate binding; not needed for visual tests since the
    // renderer's output can be read back with glReadPixels if
    // A3+ wires that up. For now, return FALSE.
    //
    return FALSE;
}

//============================================================================
// Wayland-specific extension hook (platform_linux_wayland_client.h)
//============================================================================

struct wl_display* platform_wayland__get_display(void)
{
    return _platform_linux_wayland_client_internal__g.display;
}

//
// EGL handles exposed as void* so consumers can import dmabufs
// forwarded by the compositor (meek_shell_v1.toplevel_buffer) into
// textures this context can sample. See header for the cast pattern.
// Before platform__init both are EGL_NO_DISPLAY / EGL_NO_CONTEXT,
// which EGL spec guarantees fit into a void* on every platform we
// target -- no NULL-specific check needed.
//
void* platform_wayland__get_egl_display(void)
{
    return (void*)_platform_linux_wayland_client_internal__g.egl_display;
}

void* platform_wayland__get_egl_context(void)
{
    return (void*)_platform_linux_wayland_client_internal__g.egl_context;
}

//============================================================================
// wl_seat / wl_pointer / wl_touch
//============================================================================
//
// Input pipeline on the client side:
//
//   compositor (libinput) -> wl_seat protocol -> our pointer/touch
//   listeners -> translate to meek-ui coords -> scene__on_mouse_*
//   calls -> meek-ui's hit-tester fires registered handlers from
//   the .ui file (on_click, etc.)
//
// We currently unify pointer + touch into the same mouse event
// surface (scene__on_mouse_button(0, down, x, y)) because meek-ui's
// scene doesn't yet distinguish multi-touch; touch `id` is
// essentially ignored beyond "is there any finger down right now".
// Multi-touch (pinch, two-finger scroll) is future work.
//
// Coordinates arrive as wl_fixed_t (24.8 fixed point, in surface-
// local pixels). wl_fixed_to_int truncates the fractional part --
// fine for a hit-test at button resolution; we'd need
// wl_fixed_to_double for drag inertia / gestures later.
//

static void _platform_linux_wayland_client_internal__on_seat_capabilities(void* data, struct wl_seat* s, uint32_t caps)
{
    (void)data;

    //
    // Pointer: create if present, destroy if removed. Capability
    // can flap at runtime (e.g. USB mouse plugged / unplugged) --
    // compositor re-sends the capabilities event each time.
    //
    boole has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_ptr && _platform_linux_wayland_client_internal__g.pointer == NULL)
    {
        _platform_linux_wayland_client_internal__g.pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(_platform_linux_wayland_client_internal__g.pointer,
                                &_platform_linux_wayland_client_internal__pointer_listener,
                                NULL);
        log_info("platform_linux_wayland_client: wl_pointer acquired");
    }
    else if (!has_ptr && _platform_linux_wayland_client_internal__g.pointer != NULL)
    {
        wl_pointer_release(_platform_linux_wayland_client_internal__g.pointer);
        _platform_linux_wayland_client_internal__g.pointer = NULL;
    }

    //
    // Touch: same dance. On a phone this is usually the ONLY
    // capability advertised (no physical pointer, no keyboard).
    //
    boole has_touch = (caps & WL_SEAT_CAPABILITY_TOUCH) != 0;
    if (has_touch && _platform_linux_wayland_client_internal__g.touch == NULL)
    {
        _platform_linux_wayland_client_internal__g.touch = wl_seat_get_touch(s);
        wl_touch_add_listener(_platform_linux_wayland_client_internal__g.touch,
                              &_platform_linux_wayland_client_internal__touch_listener,
                              NULL);
        log_info("platform_linux_wayland_client: wl_touch acquired");
    }
    else if (!has_touch && _platform_linux_wayland_client_internal__g.touch != NULL)
    {
        wl_touch_release(_platform_linux_wayland_client_internal__g.touch);
        _platform_linux_wayland_client_internal__g.touch = NULL;
    }

    //
    // Keyboard: grab if present. The compositor will send a
    // wl_keyboard.keymap event first, which we mmap + feed to
    // xkbcommon so subsequent key/modifiers events translate
    // correctly. Needed for demo-settings' text field, shell
    // shortcuts, and any foreign key events the compositor
    // forwards via meek_shell_v1.route_keyboard_key.
    //
    boole has_kb = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (has_kb && _platform_linux_wayland_client_internal__g.keyboard == NULL)
    {
        _platform_linux_wayland_client_internal__g.keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(_platform_linux_wayland_client_internal__g.keyboard,
                                 &_platform_linux_wayland_client_internal__keyboard_listener,
                                 NULL);
        log_info("platform_linux_wayland_client: wl_keyboard acquired");
    }
    else if (!has_kb && _platform_linux_wayland_client_internal__g.keyboard != NULL)
    {
        wl_keyboard_release(_platform_linux_wayland_client_internal__g.keyboard);
        _platform_linux_wayland_client_internal__g.keyboard = NULL;
    }
}

static void _platform_linux_wayland_client_internal__on_seat_name(void* data, struct wl_seat* s, const char* name)
{
    (void)data; (void)s;
    log_info("platform_linux_wayland_client: seat name='%s'", name);
}

//-- wl_pointer -------------------------------------------------------------

static void _platform_linux_wayland_client_internal__on_pointer_enter(void* data, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)serial; (void)s;
    _platform_linux_wayland_client_internal__g.last_x = wl_fixed_to_int(x);
    _platform_linux_wayland_client_internal__g.last_y = wl_fixed_to_int(y);
    scene__on_mouse_move(_platform_linux_wayland_client_internal__g.last_x,
                         _platform_linux_wayland_client_internal__g.last_y);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_pointer_leave(void* data, struct wl_pointer* p, uint32_t serial, struct wl_surface* s)
{
    (void)data; (void)p; (void)serial; (void)s;
    //
    // No dedicated "mouse-left" in meek-ui's scene API. Park the
    // cursor off-screen so any hover-state styling fades out on
    // the next tick.
    //
    scene__on_mouse_move(-1, -1);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_pointer_motion(void* data, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)time;
    _platform_linux_wayland_client_internal__g.last_x = wl_fixed_to_int(x);
    _platform_linux_wayland_client_internal__g.last_y = wl_fixed_to_int(y);
    scene__on_mouse_move(_platform_linux_wayland_client_internal__g.last_x,
                         _platform_linux_wayland_client_internal__g.last_y);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_pointer_button(void* data, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)data; (void)p; (void)serial; (void)time;
    //
    // button codes are Linux evdev codes: BTN_LEFT=0x110,
    // BTN_RIGHT=0x111, BTN_MIDDLE=0x112. Map them to meek-ui's
    // 0/1/2 indices.
    //
    int b;
    switch (button)
    {
        case 0x110: b = 0; break; // BTN_LEFT
        case 0x111: b = 1; break; // BTN_RIGHT
        case 0x112: b = 2; break; // BTN_MIDDLE
        default:    return;       // side buttons; ignore
    }
    boole down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    scene__on_mouse_button(b, down,
                           _platform_linux_wayland_client_internal__g.last_x,
                           _platform_linux_wayland_client_internal__g.last_y);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_pointer_axis(void* data, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)p; (void)time;
    //
    // axis=VERTICAL_SCROLL is what scroll wheels report. value is
    // in units of "scroll distance" (usually ~10 per notch). meek-ui
    // expects +1/-1 per notch; scale down and clamp.
    //
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
    {
        return;
    }
    float v = (float)wl_fixed_to_double(value) / 10.0f;
    if (v > 0) v =  1.0f; //normalise direction; actual magnitude doesn't matter for scene
    if (v < 0) v = -1.0f;
    scene__on_mouse_wheel(_platform_linux_wayland_client_internal__g.last_x,
                          _platform_linux_wayland_client_internal__g.last_y,
                          v);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_pointer_frame(void* data, struct wl_pointer* p)
{
    (void)data; (void)p;
    // Frame boundary -- compositor groups related events (motion+button)
    // into a frame. We dispatch per-event already so nothing to do.
}

static void _platform_linux_wayland_client_internal__on_pointer_axis_source(void* data, struct wl_pointer* p, uint32_t source)
{ (void)data; (void)p; (void)source; }

static void _platform_linux_wayland_client_internal__on_pointer_axis_stop(void* data, struct wl_pointer* p, uint32_t time, uint32_t axis)
{ (void)data; (void)p; (void)time; (void)axis; }

static void _platform_linux_wayland_client_internal__on_pointer_axis_discrete(void* data, struct wl_pointer* p, uint32_t axis, int32_t discrete)
{ (void)data; (void)p; (void)axis; (void)discrete; }

//-- wl_output --------------------------------------------------------------
//
// We care about exactly one piece of wl_output state: the current
// mode's pixel dimensions. These feed gesture-classification code in
// downstream consumers (meek-shell's edge-swipe recognizer needs
// panel-native px to compute zone thirds; passing the shell's logical
// surface size produces wrong zones on phones where logical != native).
// geometry / done / scale are observed but not consumed.
//

static void _platform_linux_wayland_client_internal__on_output_geometry(
    void* data, struct wl_output* o, int32_t x, int32_t y,
    int32_t physical_width, int32_t physical_height, int32_t subpixel,
    const char* make, const char* model, int32_t transform)
{
    (void)data; (void)o; (void)x; (void)y;
    (void)physical_width; (void)physical_height; (void)subpixel;
    (void)make; (void)model; (void)transform;
}

static void _platform_linux_wayland_client_internal__on_output_mode(
    void* data, struct wl_output* o, uint32_t flags,
    int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)o; (void)refresh;
    //
    // wl_output advertises every supported mode at registry time,
    // distinguishing them via flags. We only latch the one with
    // WL_OUTPUT_MODE_CURRENT set -- the mode the panel is actually
    // configured to right now.
    //
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) { return; }
    _platform_linux_wayland_client_internal__g.output_pixel_w = (int)width;
    _platform_linux_wayland_client_internal__g.output_pixel_h = (int)height;
    log_info("wl_output.mode current %dx%d @ %d.%03d Hz",
             (int)width, (int)height, refresh / 1000, refresh % 1000);
}

static void _platform_linux_wayland_client_internal__on_output_done(
    void* data, struct wl_output* o)
{ (void)data; (void)o; }

static void _platform_linux_wayland_client_internal__on_output_scale(
    void* data, struct wl_output* o, int32_t factor)
{ (void)data; (void)o; (void)factor; }

//-- wl_touch ---------------------------------------------------------------

static void _platform_linux_wayland_client_internal__on_touch_down(void* data, struct wl_touch* t, uint32_t serial, uint32_t time, struct wl_surface* s, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)t; (void)serial; (void)time; (void)s;
    log_info("wl_touch.down id=%d x=%d y=%d", id, wl_fixed_to_int(x), wl_fixed_to_int(y));
    //
    // First-finger-down. We only track one finger at a time for
    // now; additional fingers on an already-active gesture are
    // ignored. Multi-touch support would track per-id state here.
    //
    if (_platform_linux_wayland_client_internal__g.touch_active_id != -1)
    {
        return; //already tracking another finger
    }
    _platform_linux_wayland_client_internal__g.touch_active_id = id;
    _platform_linux_wayland_client_internal__g.touch_x = wl_fixed_to_int(x);
    _platform_linux_wayland_client_internal__g.touch_y = wl_fixed_to_int(y);
    _platform_linux_wayland_client_internal__g.last_x  = _platform_linux_wayland_client_internal__g.touch_x;
    _platform_linux_wayland_client_internal__g.last_y  = _platform_linux_wayland_client_internal__g.touch_y;

    //
    // Fire as "button 0 pressed". meek-ui's hit-tester + on_click
    // dispatch works on mouse semantics, which matches single-
    // finger touch cleanly (down + up at same position = click).
    //
    scene__on_mouse_button(0, TRUE,
                           _platform_linux_wayland_client_internal__g.touch_x,
                           _platform_linux_wayland_client_internal__g.touch_y);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_touch_up(void* data, struct wl_touch* t, uint32_t serial, uint32_t time, int32_t id)
{
    (void)data; (void)t; (void)serial; (void)time;
    //
    // wl_touch.up carries no coords (the finger is gone). Use the
    // last-known position from down / motion. Only react if the
    // released finger is the one we were tracking.
    //
    if (id != _platform_linux_wayland_client_internal__g.touch_active_id)
    {
        return;
    }
    scene__on_mouse_button(0, FALSE,
                           _platform_linux_wayland_client_internal__g.touch_x,
                           _platform_linux_wayland_client_internal__g.touch_y);
    _platform_linux_wayland_client_internal__g.touch_active_id = -1;
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_touch_motion(void* data, struct wl_touch* t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)t; (void)time;
    if (id != _platform_linux_wayland_client_internal__g.touch_active_id)
    {
        return;
    }
    _platform_linux_wayland_client_internal__g.touch_x = wl_fixed_to_int(x);
    _platform_linux_wayland_client_internal__g.touch_y = wl_fixed_to_int(y);
    _platform_linux_wayland_client_internal__g.last_x  = _platform_linux_wayland_client_internal__g.touch_x;
    _platform_linux_wayland_client_internal__g.last_y  = _platform_linux_wayland_client_internal__g.touch_y;
    scene__on_mouse_move(_platform_linux_wayland_client_internal__g.touch_x,
                         _platform_linux_wayland_client_internal__g.touch_y);
    _platform_linux_wayland_client_internal__force_render_now();
}

static void _platform_linux_wayland_client_internal__on_touch_frame(void* data, struct wl_touch* t)
{ (void)data; (void)t; /*see pointer_frame*/ }

static void _platform_linux_wayland_client_internal__on_touch_cancel(void* data, struct wl_touch* t)
{
    (void)data; (void)t;
    //
    // Compositor cancels touch when grab is broken (e.g. a gesture
    // was recognized and taken over by the compositor). Release
    // any held "button" so widgets don't stay stuck in :active.
    //
    if (_platform_linux_wayland_client_internal__g.touch_active_id != -1)
    {
        scene__on_mouse_button(0, FALSE,
                               _platform_linux_wayland_client_internal__g.touch_x,
                               _platform_linux_wayland_client_internal__g.touch_y);
        _platform_linux_wayland_client_internal__g.touch_active_id = -1;
        _platform_linux_wayland_client_internal__force_render_now();
    }
}

static void _platform_linux_wayland_client_internal__on_touch_shape(void* data, struct wl_touch* t, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{ (void)data; (void)t; (void)id; (void)major; (void)minor; }

static void _platform_linux_wayland_client_internal__on_touch_orientation(void* data, struct wl_touch* t, int32_t id, wl_fixed_t orientation)
{ (void)data; (void)t; (void)id; (void)orientation; }

//-- wl_keyboard -----------------------------------------------------------
//
// Protocol event order:
//   keymap       once, on get_keyboard, with the xkb keymap as a
//                mmap-able fd.
//   repeat_info  once if version >= 4, says rate/delay for auto-
//                repeat. We don't implement repeat yet; log only.
//   enter        when a surface of ours gains keyboard focus.
//                carries an array of already-pressed keys (empty
//                for our use case).
//   modifiers    any time shift/ctrl/alt/super state changes.
//                must be applied to xkb_state BEFORE the next key
//                event so translation uses the right state.
//   key          press / release of a physical key. event's
//                `key` field is a Linux evdev keycode minus 8
//                (xkb convention: xkb_keycode = evdev_keycode + 8).
//   leave        focus moves off our surface.
//
// Translation. Each `key` event carries an evdev keycode. We:
//   1. Add 8 to get the xkb keycode (evdev->xkb offset).
//   2. xkb_state_key_get_one_sym gives the xkb_keysym_t with
//      current modifier state applied. Keysym is what the user
//      "typed" (e.g. XKB_KEY_a vs XKB_KEY_A depending on shift).
//   3. xkb_keysym_to_utf32 converts keysym -> unicode codepoint
//      for printable characters. Non-printable (arrows, F-keys,
//      enter, backspace) return 0; we handle those via
//      scene__on_key with a VK_ code.
//

static void _platform_linux_wayland_client_internal__on_keyboard_keymap(void* data, struct wl_keyboard* k, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)k;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
    {
        log_warn("wl_keyboard.keymap: unexpected format=%u; ignoring", format);
        close(fd);
        return;
    }

    //
    // Map the compositor's keymap file into our address space read-
    // only. MAP_PRIVATE is required per spec -- the compositor may
    // still have the fd open and we mustn't modify shared pages.
    //
    void* map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED)
    {
        log_error("wl_keyboard.keymap: mmap(%u bytes) failed: %s", size, strerror(errno));
        close(fd);
        return;
    }

    if (_platform_linux_wayland_client_internal__g.xkb_ctx == NULL)
    {
        _platform_linux_wayland_client_internal__g.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (_platform_linux_wayland_client_internal__g.xkb_ctx == NULL)
        {
            log_error("wl_keyboard.keymap: xkb_context_new failed");
            munmap(map, size);
            close(fd);
            return;
        }
    }

    //
    // Replace keymap + state atomically. xkb_keymap_new_from_string
    // copies the text, so we can unmap + close immediately after.
    // (String is NUL-terminated per protocol: the compositor writes
    // `strlen(keymap) + 1` bytes.)
    //
    struct xkb_keymap* km = xkb_keymap_new_from_string(
        _platform_linux_wayland_client_internal__g.xkb_ctx,
        (const char*)map,
        XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);

    if (km == NULL)
    {
        log_error("wl_keyboard.keymap: xkb_keymap_new_from_string failed");
        return;
    }

    struct xkb_state* st = xkb_state_new(km);
    if (st == NULL)
    {
        log_error("wl_keyboard.keymap: xkb_state_new failed");
        xkb_keymap_unref(km);
        return;
    }

    if (_platform_linux_wayland_client_internal__g.xkb_state != NULL)
    {
        xkb_state_unref(_platform_linux_wayland_client_internal__g.xkb_state);
    }
    if (_platform_linux_wayland_client_internal__g.xkb_keymap != NULL)
    {
        xkb_keymap_unref(_platform_linux_wayland_client_internal__g.xkb_keymap);
    }
    _platform_linux_wayland_client_internal__g.xkb_keymap = km;
    _platform_linux_wayland_client_internal__g.xkb_state  = st;

    log_info("platform_linux_wayland_client: xkb keymap installed (%u bytes)", size);
}

static void _platform_linux_wayland_client_internal__on_keyboard_enter(void* data, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys)
{
    (void)data; (void)k; (void)serial; (void)s; (void)keys;
    //
    // We gained keyboard focus. No per-enter state to set up --
    // xkb_state persists across enter/leave. Clear any sticky
    // "this character is already held" accumulation (we don't
    // track it, but a future enter-carries-pressed-keys path
    // would iterate `keys` here).
    //
    log_trace("wl_keyboard.enter");
}

static void _platform_linux_wayland_client_internal__on_keyboard_leave(void* data, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s)
{
    (void)data; (void)k; (void)serial; (void)s;
    log_trace("wl_keyboard.leave");
}

static void _platform_linux_wayland_client_internal__on_keyboard_key(void* data, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)data; (void)k; (void)serial; (void)time;
    if (_platform_linux_wayland_client_internal__g.xkb_state == NULL)
    {
        //
        // keymap event hasn't arrived yet (unusual; compositor is
        // supposed to send it BEFORE the first key). Drop.
        //
        log_warn("wl_keyboard.key before keymap; dropping (key=%u state=%u)", key, state);
        return;
    }

    boole pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);

    //
    // xkb keycode = evdev keycode + 8. (Historical xkb convention
    // carried over from X11. Every Wayland client does this.)
    //
    xkb_keycode_t xkb_kc = (xkb_keycode_t)(key + 8);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(
        _platform_linux_wayland_client_internal__g.xkb_state, xkb_kc);

    //
    // Translate to a unicode codepoint for printable keys, and a
    // VK_ code for non-printable ones. We deliver both when both
    // are meaningful (e.g. return = codepoint 0x0D AND VK_RETURN),
    // so widgets that listen on either path work.
    //

    //
    // VK code: Windows virtual-key convention, which is what
    // widget_input / widget_textarea listen for. Mirror of the
    // subset widget_keyboard uses on the dispatch side.
    //
    int64 vk = 0;
    switch (sym)
    {
        case XKB_KEY_BackSpace: vk = 0x08; break;
        case XKB_KEY_Tab:       vk = 0x09; break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:  vk = 0x0D; break;
        case XKB_KEY_Escape:    vk = 0x1B; break;
        case XKB_KEY_Left:      vk = 0x25; break;
        case XKB_KEY_Up:        vk = 0x26; break;
        case XKB_KEY_Right:     vk = 0x27; break;
        case XKB_KEY_Down:      vk = 0x28; break;
        case XKB_KEY_Delete:    vk = 0x2E; break;
        case XKB_KEY_Home:      vk = 0x24; break;
        case XKB_KEY_End:       vk = 0x23; break;
        default: break;
    }

    if (vk != 0)
    {
        //
        // Non-printable key: deliver via scene__on_key only. Don't
        // also emit scene__on_char; widget_input/textarea already
        // listen for VK_BACK / VK_RETURN in on_key.
        //
        scene__on_key(vk, pressed);
        _platform_linux_wayland_client_internal__force_render_now();
        log_trace("wl_keyboard.key evdev=%u sym=%#x -> VK 0x%x %s",
                  key, sym, (unsigned)vk, pressed ? "DOWN" : "UP");
        return;
    }

    //
    // Printable key. Only deliver char on press (matching how
    // widget_input.input_on_char expects single codepoints per
    // keystroke, not auto-repeat duplicates).
    //
    if (pressed)
    {
        uint32_t cp = xkb_keysym_to_utf32(sym);
        if (cp != 0)
        {
            scene__on_char((uint)cp);
            _platform_linux_wayland_client_internal__force_render_now();
            log_trace("wl_keyboard.key evdev=%u sym=%#x -> codepoint U+%04X",
                      key, sym, cp);
        }
        else
        {
            log_trace("wl_keyboard.key evdev=%u sym=%#x: no codepoint, no VK (dropped)",
                      key, sym);
        }
    }
}

static void _platform_linux_wayland_client_internal__on_keyboard_modifiers(void* data, struct wl_keyboard* k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data; (void)k; (void)serial;
    if (_platform_linux_wayland_client_internal__g.xkb_state == NULL) { return; }
    //
    // Push modifier state into xkb_state so the next
    // xkb_state_key_get_one_sym call returns a shifted / ctrl'd
    // keysym as appropriate.
    //
    xkb_state_update_mask(_platform_linux_wayland_client_internal__g.xkb_state,
                          depressed, latched, locked,
                          /*depressed_layout*/ 0,
                          /*latched_layout*/   0,
                          group);
    log_trace("wl_keyboard.modifiers d=%u l=%u lk=%u g=%u",
              depressed, latched, locked, group);
}

static void _platform_linux_wayland_client_internal__on_keyboard_repeat_info(void* data, struct wl_keyboard* k, int32_t rate, int32_t delay)
{
    (void)data; (void)k;
    //
    // Auto-repeat not implemented yet. Log for diagnostics.
    //
    log_trace("wl_keyboard.repeat_info rate=%d delay=%d (not implemented)", rate, delay);
}
