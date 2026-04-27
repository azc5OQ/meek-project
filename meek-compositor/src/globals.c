//
//globals.c -- remaining compositor-wide Wayland globals after the
//surface.c / seat.c split.
//
//Owns:
//  * wl_compositor (delegates create_surface/create_region to surface.c)
//  * wl_output     (advertises panel geometry)
//  * wl_subcompositor + wl_subsurface (stubs so toolkit clients
//    don't bail at init)
//  * wl_data_device_manager + wl_data_source + wl_data_device
//    (stubs for clipboard/DnD capability-check)
//  * wl_shm        (init via libwayland's canned helper)
//  * globals__register_all: creates all globals + kicks off the
//    subsystems owned by surface.c / seat.c / xdg_shell.c /
//    linux_dmabuf.c / meek_shell_v1.c
//
//Surface/region and seat code live in their own translation units
//now; we keep their public function names stable via globals.h so
//existing callers (input.c, meek_shell_v1.c, xdg_shell.c,
//output_drm.c) don't need to adjust includes.
//

#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>

#include "types.h"
#include "third_party/log.h"
#include "clib/memory_manager.h"

#include "globals.h"
#include "surface.h"
#include "seat.h"
#include "xdg_shell.h"
#include "linux_dmabuf.h"
#include "meek_shell_v1.h"
#include "text_input_v3.h"
#include "viewporter.h"
#include "fractional_scale.h"
#include "output_drm.h"

//-- forward decls --
static void _globals_internal__compositor__on_create_surface(struct wl_client* c, struct wl_resource* r, uint32_t id);
static void _globals_internal__compositor__on_create_region(struct wl_client* c, struct wl_resource* r, uint32_t id);
static void _globals_internal__compositor__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id);

static void _globals_internal__output__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id);

static void _globals_internal__subcompositor__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _globals_internal__subcompositor__on_get_subsurface(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* surface, struct wl_resource* parent);
static void _globals_internal__subcompositor__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id);
static void _globals_internal__subsurface__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _globals_internal__subsurface__on_set_position(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y);
static void _globals_internal__subsurface__on_place_above(struct wl_client* c, struct wl_resource* r, struct wl_resource* sibling);
static void _globals_internal__subsurface__on_place_below(struct wl_client* c, struct wl_resource* r, struct wl_resource* sibling);
static void _globals_internal__subsurface__on_set_sync(struct wl_client* c, struct wl_resource* r);
static void _globals_internal__subsurface__on_set_desync(struct wl_client* c, struct wl_resource* r);

static void _globals_internal__ddm__on_create_data_source(struct wl_client* c, struct wl_resource* r, uint32_t id);
static void _globals_internal__ddm__on_get_data_device(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* seat);
static void _globals_internal__ddm__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id);
static void _globals_internal__data_source__on_offer(struct wl_client* c, struct wl_resource* r, const char* mime_type);
static void _globals_internal__data_source__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _globals_internal__data_source__on_set_actions(struct wl_client* c, struct wl_resource* r, uint32_t dnd_actions);
static void _globals_internal__data_device__on_start_drag(struct wl_client* c, struct wl_resource* r, struct wl_resource* source, struct wl_resource* origin, struct wl_resource* icon, uint32_t serial);
static void _globals_internal__data_device__on_set_selection(struct wl_client* c, struct wl_resource* r, struct wl_resource* source, uint32_t serial);
static void _globals_internal__data_device__on_release(struct wl_client* c, struct wl_resource* r);

// ============================================================
// wl_compositor
// ============================================================

static const struct wl_compositor_interface _globals_internal__compositor_impl = {
    .create_surface = _globals_internal__compositor__on_create_surface,
    .create_region  = _globals_internal__compositor__on_create_region,
};

static void _globals_internal__compositor__on_create_surface(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    uint32_t version = wl_resource_get_version(r);
    surface__create(c, version, id);
}

static void _globals_internal__compositor__on_create_region(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    uint32_t version = wl_resource_get_version(r);
    surface__create_region(c, version, id);
}

static void _globals_internal__compositor__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* r = wl_resource_create(client, &wl_compositor_interface, version, id);
    if (r == NULL)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &_globals_internal__compositor_impl, NULL, NULL);
    log_trace("wl_compositor bind id=%u v=%u", id, version);
}

// ============================================================
// wl_output
// ============================================================

//
// Per-client wl_output resource tracking. We need this for two
// reasons:
//   1. wl_surface.enter(output) must be emitted when a surface
//      becomes visible on our output. GTK4 / libadwaita /
//      fractional_scale-aware clients use wl_surface.enter to pair
//      their surface with an output and resolve scale from there.
//      Without it, GTK4 silently crashes after receiving
//      wp_fractional_scale_v1.preferred_scale.
//   2. Future multi-surface / multi-output code needs to walk
//      outputs per client to (re)send enter / leave.
//
// Stored as an intrusive wl_list of wrappers, each with a destroy
// listener so a disconnected client cleans up automatically.
//
struct _globals_internal__output_wrapper
{
    struct wl_resource* resource;
    struct wl_listener  destroy_listener;
    struct wl_list      link;
};
static struct wl_list _globals_internal__outputs;
static int            _globals_internal__outputs_initialized = 0;

static void _globals_internal__outputs_ensure_init(void)
{
    if (!_globals_internal__outputs_initialized)
    {
        wl_list_init(&_globals_internal__outputs);
        _globals_internal__outputs_initialized = 1;
    }
}

static void _globals_internal__on_output_resource_destroy(struct wl_listener* l, void* data)
{
    (void)data;
    struct _globals_internal__output_wrapper* w = wl_container_of(l, w, destroy_listener);
    wl_list_remove(&w->link);
    GUI_FREE(w);
}

//
// Send wl_surface.enter(output) for every live wl_output resource
// owned by this client. Idempotent for same-client / same-surface
// calls: the surface's internal "entered outputs" list is
// maintained by libwayland-server, duplicates are harmless (client
// would just receive the enter event again).
//
// Called from surface.c on the first commit with a buffer attached,
// so clients see enter right before they'd first render.
//
void globals__send_output_enter_for_surface(struct wl_resource* surface)
{
    if (surface == NULL) { return; }
    _globals_internal__outputs_ensure_init();
    struct wl_client* c = wl_resource_get_client(surface);
    if (c == NULL) { return; }

    struct _globals_internal__output_wrapper* w;
    wl_list_for_each(w, &_globals_internal__outputs, link)
    {
        if (wl_resource_get_client(w->resource) == c)
        {
            wl_surface_send_enter(surface, w->resource);
        }
    }
}

static void _globals_internal__output__on_release(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static const struct wl_output_interface _globals_internal__output_impl = {
    .release = _globals_internal__output__on_release,
};

//
// Physical panel dimensions as advertised via wl_output.geometry.
// These are the display AREA (active viewable LCD pixels), NOT the
// device chassis. Clients use this to compute DPI for automatic
// scale selection; wrong values make them pick a wrong scale.
//
// Defaults below are the Poco F1 (beryllium) display:
//   1080 x 2246 px on a 6.18" diagonal at 2.08:1 aspect.
//   Diagonal = 157 mm, so W = 157/sqrt(1+(2246/1080)^2) ≈ 67.8 mm,
//   H = W * 2246/1080 ≈ 141 mm.
//   -> ~403 DPI physical.
//
// The old values (155 x 87) were the device-chassis outer dims,
// which falsely made GTK4 compute ~177 DPI and pick an integer
// scale of 1 (unreadable on a real 400-DPI phone).
//
#define _GLOBALS_OUTPUT_W_MM        68
#define _GLOBALS_OUTPUT_H_MM        141
#define _GLOBALS_OUTPUT_W_PX        1920
#define _GLOBALS_OUTPUT_H_PX        1080
#define _GLOBALS_OUTPUT_REFRESH_MHZ 60000

static void _globals_internal__output__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* r = wl_resource_create(client, &wl_output_interface, version, id);
    if (r == NULL)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &_globals_internal__output_impl, NULL, NULL);

    //
    // Track the output resource so surface.c::on_commit can find
    // per-client outputs to fire wl_surface.enter on.
    //
    _globals_internal__outputs_ensure_init();
    struct _globals_internal__output_wrapper* w = GUI_CALLOC_T(1, sizeof(*w), MM_TYPE_NODE);
    if (w != NULL)
    {
        w->resource = r;
        w->destroy_listener.notify = _globals_internal__on_output_resource_destroy;
        wl_resource_add_destroy_listener(r, &w->destroy_listener);
        wl_list_insert(&_globals_internal__outputs, &w->link);
    }

    int panel_w = _GLOBALS_OUTPUT_W_PX;
    int panel_h = _GLOBALS_OUTPUT_H_PX;
    (void)output_drm__get_native_size(&panel_w, &panel_h);

    wl_output_send_geometry(r,
                            /*x*/ 0, /*y*/ 0,
                            _GLOBALS_OUTPUT_W_MM,
                            _GLOBALS_OUTPUT_H_MM,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            /*make*/  "meek",
                            /*model*/ "virtual",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r,
                        WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        panel_w,
                        panel_h,
                        _GLOBALS_OUTPUT_REFRESH_MHZ);
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)    { wl_output_send_scale(r, 1); }
    if (version >= WL_OUTPUT_NAME_SINCE_VERSION)     { wl_output_send_name(r, "MEEK-0"); }
    if (version >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION) { wl_output_send_description(r, "meek-compositor virtual output"); }
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION)     { wl_output_send_done(r); }
    log_trace("wl_output bind id=%u v=%u", id, version);
}

void globals__get_output_geometry(int* w_px, int* h_px, int* w_mm, int* h_mm)
{
    if (w_px == NULL || h_px == NULL || w_mm == NULL || h_mm == NULL) { return; }

    *w_px = _GLOBALS_OUTPUT_W_PX;
    *h_px = _GLOBALS_OUTPUT_H_PX;
    (void)output_drm__get_native_size(w_px, h_px);

    *w_mm = _GLOBALS_OUTPUT_W_MM;
    *h_mm = _GLOBALS_OUTPUT_H_MM;
}

// ============================================================
// wl_subcompositor + wl_subsurface (stubs)
// ============================================================

static const struct wl_subsurface_interface _globals_internal__subsurface_impl = {
    .destroy      = _globals_internal__subsurface__on_destroy,
    .set_position = _globals_internal__subsurface__on_set_position,
    .place_above  = _globals_internal__subsurface__on_place_above,
    .place_below  = _globals_internal__subsurface__on_place_below,
    .set_sync     = _globals_internal__subsurface__on_set_sync,
    .set_desync   = _globals_internal__subsurface__on_set_desync,
};

static const struct wl_subcompositor_interface _globals_internal__subcompositor_impl = {
    .destroy        = _globals_internal__subcompositor__on_destroy,
    .get_subsurface = _globals_internal__subcompositor__on_get_subsurface,
};

static void _globals_internal__subsurface__on_destroy(struct wl_client* c, struct wl_resource* r)
{ (void)c; wl_resource_destroy(r); }
static void _globals_internal__subsurface__on_set_position(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y)
{ (void)c; (void)r; (void)x; (void)y; }
static void _globals_internal__subsurface__on_place_above(struct wl_client* c, struct wl_resource* r, struct wl_resource* sibling)
{ (void)c; (void)r; (void)sibling; }
static void _globals_internal__subsurface__on_place_below(struct wl_client* c, struct wl_resource* r, struct wl_resource* sibling)
{ (void)c; (void)r; (void)sibling; }
static void _globals_internal__subsurface__on_set_sync(struct wl_client* c, struct wl_resource* r)
{ (void)c; (void)r; }
static void _globals_internal__subsurface__on_set_desync(struct wl_client* c, struct wl_resource* r)
{ (void)c; (void)r; }

static void _globals_internal__subcompositor__on_destroy(struct wl_client* c, struct wl_resource* r)
{ (void)c; wl_resource_destroy(r); }

static void _globals_internal__subcompositor__on_get_subsurface(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* surface, struct wl_resource* parent)
{
    (void)r; (void)surface; (void)parent;
    struct wl_resource* ss = wl_resource_create(c, &wl_subsurface_interface, 1, id);
    if (ss == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(ss, &_globals_internal__subsurface_impl, NULL, NULL);
    log_trace("wl_subcompositor.get_subsurface -> stub %p (id=%u)", (void*)ss, id);
}

static void _globals_internal__subcompositor__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* r = wl_resource_create(client, &wl_subcompositor_interface, version, id);
    if (r == NULL)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &_globals_internal__subcompositor_impl, NULL, NULL);
    log_trace("wl_subcompositor bind id=%u v=%u", id, version);
}

// ============================================================
// wl_data_device_manager stubs
// ============================================================

static const struct wl_data_source_interface _globals_internal__data_source_impl = {
    .offer       = _globals_internal__data_source__on_offer,
    .destroy     = _globals_internal__data_source__on_destroy,
    .set_actions = _globals_internal__data_source__on_set_actions,
};

static const struct wl_data_device_interface _globals_internal__data_device_impl = {
    .start_drag    = _globals_internal__data_device__on_start_drag,
    .set_selection = _globals_internal__data_device__on_set_selection,
    .release       = _globals_internal__data_device__on_release,
};

static const struct wl_data_device_manager_interface _globals_internal__ddm_impl = {
    .create_data_source = _globals_internal__ddm__on_create_data_source,
    .get_data_device    = _globals_internal__ddm__on_get_data_device,
};

static void _globals_internal__data_source__on_offer(struct wl_client* c, struct wl_resource* r, const char* mime_type)
{ (void)c; (void)r; (void)mime_type; }
static void _globals_internal__data_source__on_destroy(struct wl_client* c, struct wl_resource* r)
{ (void)c; wl_resource_destroy(r); }
static void _globals_internal__data_source__on_set_actions(struct wl_client* c, struct wl_resource* r, uint32_t a)
{ (void)c; (void)r; (void)a; }

static void _globals_internal__data_device__on_start_drag(struct wl_client* c, struct wl_resource* r, struct wl_resource* source, struct wl_resource* origin, struct wl_resource* icon, uint32_t serial)
{ (void)c; (void)r; (void)source; (void)origin; (void)icon; (void)serial; }
static void _globals_internal__data_device__on_set_selection(struct wl_client* c, struct wl_resource* r, struct wl_resource* source, uint32_t serial)
{ (void)c; (void)r; (void)source; (void)serial; }
static void _globals_internal__data_device__on_release(struct wl_client* c, struct wl_resource* r)
{ (void)c; wl_resource_destroy(r); }

static void _globals_internal__ddm__on_create_data_source(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    (void)r;
    struct wl_resource* ds = wl_resource_create(c, &wl_data_source_interface, 3, id);
    if (ds == NULL) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(ds, &_globals_internal__data_source_impl, NULL, NULL);
}

static void _globals_internal__ddm__on_get_data_device(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* seat)
{
    (void)r; (void)seat;
    struct wl_resource* dd = wl_resource_create(c, &wl_data_device_interface, 3, id);
    if (dd == NULL) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(dd, &_globals_internal__data_device_impl, NULL, NULL);
}

static void _globals_internal__ddm__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* r = wl_resource_create(client, &wl_data_device_manager_interface, version, id);
    if (r == NULL) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &_globals_internal__ddm_impl, NULL, NULL);
    log_trace("wl_data_device_manager bind id=%u v=%u", id, version);
}

// ============================================================
// Composition entry point
// ============================================================

void globals__register_all(struct wl_display* display)
{
    //
    //Initialize the two per-subsystem lists BEFORE any global is
    //registered -- a client could bind wl_compositor or wl_seat
    //inside the dispatch that runs right after we return.
    //
    surface__init();
    seat__init();

    struct wl_global* g_compositor = wl_global_create(display,
                                                      &wl_compositor_interface,
                                                      /*version*/ 6,
                                                      /*data*/    NULL,
                                                      _globals_internal__compositor__bind);
    if (g_compositor == NULL) { log_fatal("wl_global_create(wl_compositor) failed"); return; }

    if (wl_display_init_shm(display) != 0)
    {
        log_fatal("wl_display_init_shm failed");
        return;
    }

    struct wl_global* g_output = wl_global_create(display,
                                                  &wl_output_interface,
                                                  /*version*/ 4,
                                                  /*data*/    NULL,
                                                  _globals_internal__output__bind);
    if (g_output == NULL) { log_fatal("wl_global_create(wl_output) failed"); return; }

    seat__register(display);

    xdg_shell__register(display);
    linux_dmabuf__register(display);

    struct wl_global* g_subcompositor = wl_global_create(display,
                                                         &wl_subcompositor_interface,
                                                         /*version*/ 1,
                                                         /*data*/    NULL,
                                                         _globals_internal__subcompositor__bind);
    if (g_subcompositor == NULL) { log_fatal("wl_global_create(wl_subcompositor) failed"); return; }

    struct wl_global* g_ddm = wl_global_create(display,
                                               &wl_data_device_manager_interface,
                                               /*version*/ 3,
                                               /*data*/    NULL,
                                               _globals_internal__ddm__bind);
    if (g_ddm == NULL) { log_fatal("wl_global_create(wl_data_device_manager) failed"); return; }

    //
    // zwp_text_input_manager_v3 -- stub. foot / gtk / qt clients
    // bind this to request IME services. We advertise so clients
    // don't log "text input interface not implemented"; actual IME
    // flow is deferred to layer 2 (wire to meek-shell's
    // widget_keyboard).
    //
    text_input_v3__register(display);

    //
    // wp_viewporter + wp_fractional_scale_manager_v1 -- Level 1
    // (accept requests, send a fixed preferred_scale=120 = 1.0x).
    // libadwaita / GTK4 clients refuse to render on a high-DPI
    // wl_output if these aren't advertised; amberol was the
    // forcing function (silently exited after xdg_toplevel init).
    // Viewporter is registered FIRST because fractional_scale
    // makes no sense without it -- a client that sees fractional
    // but not viewporter can't actually apply the scale.
    //
    //
    // `MEEK_FRACTIONAL_SCALE` env-var gate on viewporter +
    // fractional_scale_manager advertisements. Useful for future
    // A/B bisection if a GTK4 / Qt client ever misbehaves and we
    // want to confirm the cause is (or isn't) these protocols.
    //
    //   unset / "both" / "1"   -> advertise BOTH (default, production)
    //   "fs"                   -> fractional_scale only
    //   "vp"                   -> viewporter only
    //   "0" / "none" / other   -> advertise NEITHER
    //
    // History (2026-04-23): added during amberol SIGSEGV triage.
    // Result: amberol crashes independently of these two globals
    // (same strlen(NULL) in GTK4 regardless of MEEK_FRACTIONAL_SCALE
    // value). Bug is inside amberol's Rust/GTK4/GStreamer init path,
    // not in meek-compositor. See session/crucial_fixes.md entry on
    // amberol for the gdb backtrace + details.
    //
    // Kept as a diagnostic lever; safe to remove once another client
    // surfaces a new bisection need. Default-on preserves Level-1
    // behaviour for all clients.
    //
    const char* meek_fs = getenv("MEEK_FRACTIONAL_SCALE");
    int want_vp = 0;
    int want_fs = 0;
    if (meek_fs == NULL || strcmp(meek_fs, "both") == 0 || strcmp(meek_fs, "1") == 0)
    {
        want_vp = 1;
        want_fs = 1;
    }
    else if (strcmp(meek_fs, "fs") == 0)
    {
        want_fs = 1;
    }
    else if (strcmp(meek_fs, "vp") == 0)
    {
        want_vp = 1;
    }
    // else: want_vp=0, want_fs=0 (neither advertised)

    if (want_vp) { viewporter__register(display); }
    if (want_fs) { fractional_scale__register(display); }
    log_info("Level-1 scale globals: viewporter=%d fractional_scale=%d (MEEK_FRACTIONAL_SCALE=%s)",
             want_vp, want_fs, meek_fs ? meek_fs : "(unset)");

    meek_shell_v1__register(display);

    log_info("core globals registered: wl_compositor v6, wl_shm, wl_output v4, wl_seat v7, xdg_wm_base v5, wp_viewporter v1, wp_fractional_scale_manager_v1 v1, meek_shell_v1 v1");
    log_info("(zwp_linux_dmabuf_v1 registration depends on EGL; see prior log lines)");
}
