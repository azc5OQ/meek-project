//
// viewporter.c -- wp_viewporter global + per-wl_surface wp_viewport.
//
// See viewporter.h for scope. Level 1 is "accept + store"; no
// transforms applied yet.
//
// The protocol shape, for reference:
//
//   interface wp_viewporter {
//       request destroy;
//       request get_viewport(new_id wp_viewport, object wl_surface);
//   }
//
//   interface wp_viewport {
//       request destroy;
//       request set_source(fixed x, fixed y, fixed w, fixed h);
//           // x=y=w=h = -1 means "unset"
//       request set_destination(int w, int h);
//           // w=h = -1 means "unset"
//   }
//
// One wp_viewport per wl_surface. Creating a second one for the
// same surface is a protocol error. Destroying the wl_surface
// before the wp_viewport is also a protocol error (we don't check
// right now -- the destroy listener keeps us alive long enough).
//

#include <stdlib.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "viewporter-protocol.h"

#include "types.h"
#include "third_party/log.h"
#include "clib/memory_manager.h"
#include "viewporter.h"

//
// Per-viewport state. Stored as user_data on the wp_viewport
// resource. Source uses fixed-point (wl_fixed_t, 24.8); destination
// is plain int. -1 in any field means "unset" per the protocol.
//
struct _viewporter_internal__viewport_state
{
    wl_fixed_t src_x;
    wl_fixed_t src_y;
    wl_fixed_t src_w;
    wl_fixed_t src_h;
    int32_t    dst_w;
    int32_t    dst_h;
};

//-- forward decls --
static void _viewporter_internal__viewport__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _viewporter_internal__viewport__on_set_source(struct wl_client* c, struct wl_resource* r, wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h);
static void _viewporter_internal__viewport__on_set_destination(struct wl_client* c, struct wl_resource* r, int32_t w, int32_t h);
static void _viewporter_internal__viewport__resource_destroy(struct wl_resource* r);

static void _viewporter_internal__mgr__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _viewporter_internal__mgr__on_get_viewport(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* surface);
static void _viewporter_internal__mgr__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id);

// ============================================================
// wp_viewport impl
// ============================================================

static const struct wp_viewport_interface _viewporter_internal__viewport_impl = {
    .destroy         = _viewporter_internal__viewport__on_destroy,
    .set_source      = _viewporter_internal__viewport__on_set_source,
    .set_destination = _viewporter_internal__viewport__on_set_destination,
};

static void _viewporter_internal__viewport__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _viewporter_internal__viewport__on_set_source(struct wl_client* c, struct wl_resource* r, wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h)
{
    (void)c;
    struct _viewporter_internal__viewport_state* s = wl_resource_get_user_data(r);
    if (s == NULL) { return; }
    s->src_x = x;
    s->src_y = y;
    s->src_w = w;
    s->src_h = h;
    log_trace("wp_viewport.set_source %.2f,%.2f %.2fx%.2f",
              wl_fixed_to_double(x), wl_fixed_to_double(y),
              wl_fixed_to_double(w), wl_fixed_to_double(h));
}

static void _viewporter_internal__viewport__on_set_destination(struct wl_client* c, struct wl_resource* r, int32_t w, int32_t h)
{
    (void)c;
    struct _viewporter_internal__viewport_state* s = wl_resource_get_user_data(r);
    if (s == NULL) { return; }
    s->dst_w = w;
    s->dst_h = h;
    log_trace("wp_viewport.set_destination %dx%d", w, h);
}

static void _viewporter_internal__viewport__resource_destroy(struct wl_resource* r)
{
    struct _viewporter_internal__viewport_state* s = wl_resource_get_user_data(r);
    if (s != NULL) { GUI_FREE(s); }
}

// ============================================================
// wp_viewporter impl
// ============================================================

static const struct wp_viewporter_interface _viewporter_internal__mgr_impl = {
    .destroy      = _viewporter_internal__mgr__on_destroy,
    .get_viewport = _viewporter_internal__mgr__on_get_viewport,
};

static void _viewporter_internal__mgr__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _viewporter_internal__mgr__on_get_viewport(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* surface)
{
    (void)r;
    (void)surface;

    uint32_t version = wl_resource_get_version(r);
    struct wl_resource* vp = wl_resource_create(c, &wp_viewport_interface, version, id);
    if (vp == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }

    //
    // Per-viewport state. Start with "unset" sentinels per the
    // protocol (-1 means the client hasn't called set_source /
    // set_destination yet). Fixed-point -1 in 24.8 is 0xFFFFFF00,
    // but the protocol specifies the literal int -1 for src as
    // "unset"; using wl_fixed_from_int(-1) matches that.
    //
    struct _viewporter_internal__viewport_state* s = GUI_CALLOC_T(1, sizeof(*s), MM_TYPE_NODE);
    if (s == NULL)
    {
        wl_client_post_no_memory(c);
        wl_resource_destroy(vp);
        return;
    }
    s->src_x = wl_fixed_from_int(-1);
    s->src_y = wl_fixed_from_int(-1);
    s->src_w = wl_fixed_from_int(-1);
    s->src_h = wl_fixed_from_int(-1);
    s->dst_w = -1;
    s->dst_h = -1;

    wl_resource_set_implementation(vp, &_viewporter_internal__viewport_impl, s, _viewporter_internal__viewport__resource_destroy);
    log_trace("wp_viewporter.get_viewport id=%u surface=%p (level-1: accept+store, no transform applied)", id, (void*)surface);
}

static void _viewporter_internal__mgr__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* r = wl_resource_create(client, &wp_viewporter_interface, version, id);
    if (r == NULL)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &_viewporter_internal__mgr_impl, NULL, NULL);
    log_trace("wp_viewporter bind id=%u v=%u", id, version);
}

// ============================================================
// public
// ============================================================

void viewporter__register(struct wl_display* display)
{
    static int registered = 0;
    if (registered)
    {
        log_warn("viewporter__register called twice; ignoring");
        return;
    }
    registered = 1;

    struct wl_global* g = wl_global_create(
        display,
        &wp_viewporter_interface,
        /*version*/ 1,
        /*data*/    NULL,
        _viewporter_internal__mgr__bind);
    if (g == NULL)
    {
        log_fatal("wl_global_create(wp_viewporter) failed");
        return;
    }
    log_info("wp_viewporter registered (v1, level-1 stub: accepts set_source/set_destination, no transform applied)");
}
