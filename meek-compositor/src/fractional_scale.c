//
// fractional_scale.c -- wp_fractional_scale_manager_v1 implementation.
//
// Each wp_fractional_scale_v1 is created with preferred_scale fired
// immediately. No further scale-change events in this pass (single
// fixed output, no hot-plug, no runtime DPI change).
//
// Scale resolution at register time, in this priority order:
//   1. MEEK_PREFERRED_SCALE env var (integer wire value, range 60-960)
//   2. Auto-detect from panel DPI reported by globals__get_output_geometry
//   3. Hardcoded fallback (240 = 2.0x)
//
// Auto-detect math:
//   panel_dpi = panel_w_px * 25.4 / panel_w_mm
//   scale     = panel_dpi / _TARGET_LOGICAL_DPI
//   preferred = (uint32)(scale * 120 + 0.5)  -- wire format = scale*120
//
// Target logical DPI is configurable at top of file. 160 DPI = Android
// mdpi baseline. A typical phone panel at 400 DPI gives scale ~2.5x
// (preferred_scale 300), which produces comfortable touch targets
// without making assets look oversized. Desktop panels at 96 DPI give
// scale 0.6x, which we clamp up to 1.0x. 4K laptop panels at 200 DPI
// give scale 1.25x (preferred_scale 150), a light HiDPI bump.
//
// Override at runtime via `MEEK_PREFERRED_SCALE=<integer>`:
//   120  -> 1.0x (panel-native, for pixel-perfect measurement)
//   180  -> 1.5x (HiDPI laptop)
//   240  -> 2.0x (classic Retina)
//   300  -> 2.5x (phone-comfortable on ~400 DPI panels)
//   360  -> 3.0x (accessibility / very large touch targets)
//
// See fractional_scale.h for the design notes + protocol shape.
//

#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "fractional-scale-v1-protocol.h"

#include "types.h"
#include "third_party/log.h"
#include "clib/memory_manager.h"
#include "globals.h"
#include "xdg_shell.h"
#include "fractional_scale.h"

//
// Per-client tracking of manager binds. We track PIDs (rather than
// wl_client pointers) so a client that binds, unbinds, re-binds
// still registers as "has bound". Keeping this small -- a linear
// list is fine for a mobile use case with a handful of clients.
//
struct _fractional_scale_internal__bound_client
{
    struct wl_client*    client;
    struct wl_listener   destroy_listener;
    struct wl_list       link;
};

static struct wl_list _fractional_scale_internal__bound_clients = {
    .prev = &_fractional_scale_internal__bound_clients,
    .next = &_fractional_scale_internal__bound_clients,
};

//
// Logical DPI our auto-detection targets. 160 matches Android's
// "mdpi" baseline; apps authored against that assumption feel
// natural on the panel after the compositor's scale is applied.
// Higher values produce smaller UI (more density); lower values
// produce bigger UI (bigger widgets, better for touch).
//
#define _FRACTIONAL_SCALE_INTERNAL__TARGET_LOGICAL_DPI 160.0

//
// Clamp bounds for both auto-detected and env-var-supplied values.
// 120 = 1.0x is the floor (never shrink below panel-native). 480 =
// 4.0x is the ceiling (beyond that most clients stop coping well
// and text becomes absurdly large anyway).
//
#define _FRACTIONAL_SCALE_INTERNAL__MIN_PREFERRED_SCALE 120u
#define _FRACTIONAL_SCALE_INTERNAL__MAX_PREFERRED_SCALE 480u

//
// Hardcoded fallback -- used only if both env var is unset AND the
// auto-detection can't read panel geometry (panel_w_mm == 0 etc.).
//
#define _FRACTIONAL_SCALE_INTERNAL__FALLBACK_PREFERRED_SCALE 240u

//
// Module-level state -- the chosen scale value, lazily resolved on
// the first get_fractional_scale call. Lazy because
// fractional_scale__register runs BEFORE output_drm__init, so panel
// geometry isn't available at register time. By the time a client
// binds the manager + calls get_fractional_scale, the DRM backend
// has initialized and globals__get_output_geometry returns the real
// panel mode.
//
// 0 = not yet resolved. Non-zero = cached value in preferred_scale
// wire format (scale * 120).
//
static uint32_t _fractional_scale_internal__resolved_scale = 0;

//-- forward decls --
static void _fractional_scale_internal__scale__on_destroy(struct wl_client* c, struct wl_resource* r);

static void _fractional_scale_internal__mgr__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _fractional_scale_internal__mgr__on_get_fractional_scale(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* surface);
static void _fractional_scale_internal__mgr__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id);

static uint32_t _fractional_scale_internal__auto_from_panel_dpi(void);
static uint32_t _fractional_scale_internal__get_or_resolve_scale(void);

static void _fractional_scale_internal__remember_bound_client(struct wl_client* c);
static void _fractional_scale_internal__on_bound_client_destroy(struct wl_listener* l, void* data);

// ============================================================
// wp_fractional_scale_v1 impl
// ============================================================

static const struct wp_fractional_scale_v1_interface _fractional_scale_internal__scale_impl = {
    .destroy = _fractional_scale_internal__scale__on_destroy,
};

static void _fractional_scale_internal__scale__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

// ============================================================
// wp_fractional_scale_manager_v1 impl
// ============================================================

static const struct wp_fractional_scale_manager_v1_interface _fractional_scale_internal__mgr_impl = {
    .destroy              = _fractional_scale_internal__mgr__on_destroy,
    .get_fractional_scale = _fractional_scale_internal__mgr__on_get_fractional_scale,
};

static void _fractional_scale_internal__mgr__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _fractional_scale_internal__mgr__on_get_fractional_scale(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* surface)
{
    (void)r;
    (void)surface;

    uint32_t version = wl_resource_get_version(r);
    struct wl_resource* fs = wl_resource_create(c, &wp_fractional_scale_v1_interface, version, id);
    if (fs == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(fs, &_fractional_scale_internal__scale_impl, NULL, NULL);

    //
    // Fire preferred_scale exactly once per creation. The spec
    // permits firing again if the scale changes (output move,
    // output scale change, etc.). We don't today -- single fixed
    // output, single fixed scale.
    //
    uint32_t scale = _fractional_scale_internal__get_or_resolve_scale();
    wp_fractional_scale_v1_send_preferred_scale(fs, scale);
    log_trace("wp_fractional_scale_manager_v1.get_fractional_scale id=%u surface=%p -> preferred_scale(%u = %.2fx)",
              id, (void*)surface, scale, scale / 120.0);

    //
    // Level-2 "bigger UI" trigger: now that we've confirmed this
    // client understands fractional scaling (it bound the manager
    // AND called get_fractional_scale on a specific surface), ask
    // xdg_shell to re-send the xdg_toplevel.configure for that
    // surface with a scale-adjusted logical size. GTK4 / libadwaita
    // then lay out at the smaller logical coords and render a
    // buffer that's roughly panel-native-sized -- which the shell
    // paints 1:1 with no downsample, producing visibly bigger UI
    // elements on the panel.
    //
    // This is deliberately deferred from first-commit configure:
    // at first commit we don't yet know if the client supports
    // fractional scaling. Sending the shrunken logical then would
    // break clients that don't know to scale their buffer up.
    //
    // No-op if the surface has no xdg_toplevel role yet (viewport
    // was acquired before xdg_surface.get_toplevel). GTK4's
    // ordering is "toplevel first, then fractional_scale", so the
    // toplevel exists by the time we hit this.
    //
    // See session/design_level2_fractional_scaling.md for the
    // full rationale.
    //
    xdg_shell__reconfigure_with_fractional_scale(surface);
}

static void _fractional_scale_internal__mgr__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* r = wl_resource_create(client, &wp_fractional_scale_manager_v1_interface, version, id);
    if (r == NULL)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &_fractional_scale_internal__mgr_impl, NULL, NULL);
    _fractional_scale_internal__remember_bound_client(client);
    log_trace("wp_fractional_scale_manager_v1 bind id=%u v=%u client=%p (registered as fractional-scale-aware)",
              id, version, (void*)client);
}

//
// Track clients that have bound the manager. xdg_shell.c queries
// this before applying the Level-2 configure scale-down -- clients
// that didn't opt in to fractional_scale will misrender a scaled
// configure as a tiny 1:1 window.
//
static void _fractional_scale_internal__remember_bound_client(struct wl_client* c)
{
    //
    // Dedupe: same client may bind multiple times (rare but legal
    // per spec, particularly for toolkits that re-initialise).
    // Walking the list is O(n) but n is small (handful of clients
    // on a phone stack).
    //
    struct _fractional_scale_internal__bound_client* entry;
    wl_list_for_each(entry, &_fractional_scale_internal__bound_clients, link)
    {
        if (entry->client == c) { return; }
    }

    entry = GUI_CALLOC_T(1, sizeof(*entry), MM_TYPE_NODE);
    if (entry == NULL) { return; }
    entry->client = c;
    entry->destroy_listener.notify = _fractional_scale_internal__on_bound_client_destroy;
    wl_client_add_destroy_listener(c, &entry->destroy_listener);
    wl_list_insert(&_fractional_scale_internal__bound_clients, &entry->link);
}

static void _fractional_scale_internal__on_bound_client_destroy(struct wl_listener* l, void* data)
{
    (void)data;
    struct _fractional_scale_internal__bound_client* entry =
        wl_container_of(l, entry, destroy_listener);
    wl_list_remove(&entry->link);
    wl_list_remove(&entry->destroy_listener.link);
    GUI_FREE(entry);
}

// ============================================================
// public
// ============================================================

//
// Compute preferred_scale from the panel's actual DPI. Returns a
// value in the valid range (min..max) clamped. If panel geometry
// can't be read (w_mm == 0), returns 0 so the caller falls back.
//
static uint32_t _fractional_scale_internal__auto_from_panel_dpi(void)
{
    int w_px = 0, h_px = 0, w_mm = 0, h_mm = 0;
    globals__get_output_geometry(&w_px, &h_px, &w_mm, &h_mm);
    if (w_px <= 0 || w_mm <= 0)
    {
        return 0;
    }

    double dpi_w   = (double)w_px * 25.4 / (double)w_mm;
    double dpi_h   = (h_mm > 0 && h_px > 0)
                     ? ((double)h_px * 25.4 / (double)h_mm)
                     : dpi_w;
    double dpi_avg = (dpi_w + dpi_h) * 0.5;
    double scale   = dpi_avg / _FRACTIONAL_SCALE_INTERNAL__TARGET_LOGICAL_DPI;
    long   wire    = (long)(scale * 120.0 + 0.5);

    if (wire < (long)_FRACTIONAL_SCALE_INTERNAL__MIN_PREFERRED_SCALE)
    {
        wire = _FRACTIONAL_SCALE_INTERNAL__MIN_PREFERRED_SCALE;
    }
    if (wire > (long)_FRACTIONAL_SCALE_INTERNAL__MAX_PREFERRED_SCALE)
    {
        wire = _FRACTIONAL_SCALE_INTERNAL__MAX_PREFERRED_SCALE;
    }

    log_info("fractional_scale: auto-detect from panel %dx%dpx / %dx%dmm -> %.1f DPI -> %.2fx -> preferred_scale=%ld (target %.0f logical DPI)",
             w_px, h_px, w_mm, h_mm, dpi_avg, scale, wire,
             _FRACTIONAL_SCALE_INTERNAL__TARGET_LOGICAL_DPI);
    return (uint32_t)wire;
}

//
// Lazy resolver: run once on demand. Priority order:
//   1. MEEK_PREFERRED_SCALE env var (if in valid range)
//   2. Auto-detect from panel DPI
//   3. Hardcoded fallback
//
// Called from get_fractional_scale on first client use. AFTER
// output_drm__init, so panel geometry is real, not the pre-init
// fallback.
//
static uint32_t _fractional_scale_internal__get_or_resolve_scale(void)
{
    if (_fractional_scale_internal__resolved_scale != 0)
    {
        return _fractional_scale_internal__resolved_scale;
    }

    const char* env = getenv("MEEK_PREFERRED_SCALE");
    if (env != NULL && env[0] != '\0')
    {
        char* end = NULL;
        long v = strtol(env, &end, 10);
        if (end != env && *end == '\0' && v >= 60 && v <= 960)
        {
            _fractional_scale_internal__resolved_scale = (uint32_t)v;
            log_info("fractional_scale: MEEK_PREFERRED_SCALE=%ld accepted (%.2fx)", v, v / 120.0);
            return _fractional_scale_internal__resolved_scale;
        }
        log_warn("fractional_scale: MEEK_PREFERRED_SCALE='%s' ignored (expect integer in [60,960]); falling back to auto-detect",
                 env);
    }

    uint32_t auto_val = _fractional_scale_internal__auto_from_panel_dpi();
    if (auto_val != 0)
    {
        _fractional_scale_internal__resolved_scale = auto_val;
        return _fractional_scale_internal__resolved_scale;
    }

    _fractional_scale_internal__resolved_scale =
        _FRACTIONAL_SCALE_INTERNAL__FALLBACK_PREFERRED_SCALE;
    log_warn("fractional_scale: auto-detect unavailable (panel geometry missing); using fallback %u (%.2fx)",
             _fractional_scale_internal__resolved_scale,
             _fractional_scale_internal__resolved_scale / 120.0);
    return _fractional_scale_internal__resolved_scale;
}

void fractional_scale__register(struct wl_display* display)
{
    static int registered = 0;
    if (registered)
    {
        log_warn("fractional_scale__register called twice; ignoring");
        return;
    }
    registered = 1;

    struct wl_global* g = wl_global_create(
        display,
        &wp_fractional_scale_manager_v1_interface,
        /*version*/ 1,
        /*data*/    NULL,
        _fractional_scale_internal__mgr__bind);
    if (g == NULL)
    {
        log_fatal("wl_global_create(wp_fractional_scale_manager_v1) failed");
        return;
    }
    log_info("wp_fractional_scale_manager_v1 registered (v1, preferred_scale resolved lazily on first get_fractional_scale)");
}

uint32_t fractional_scale__get_preferred_scale(void)
{
    return _fractional_scale_internal__get_or_resolve_scale();
}

int fractional_scale__client_has_bound_manager(struct wl_client* client)
{
    if (client == NULL) { return 0; }
    struct _fractional_scale_internal__bound_client* entry;
    wl_list_for_each(entry, &_fractional_scale_internal__bound_clients, link)
    {
        if (entry->client == client) { return 1; }
    }
    return 0;
}
