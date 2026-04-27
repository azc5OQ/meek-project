//
//xdg_shell.c - minimum-viable xdg-shell server implementation.
//
//Covers xdg_wm_base v5, xdg_surface, xdg_toplevel, xdg_popup, and
//xdg_positioner. Most handlers are stubs at this pass (A2.2) --
//enough to let simple shm / terminal / gtk4-demo style clients make
//it past the xdg-shell init dance and reach the "attach buffer +
//commit" phase where we start logging their commits.
//
//Configure dance (A2.2 simplification):
//
//  1. Client calls xdg_wm_base.get_xdg_surface(wl_surface)
//  2. Client calls xdg_surface.get_toplevel
//     --> server immediately sends xdg_toplevel.configure(0,0,{})
//         and xdg_surface.configure(serial++). Real implementations
//         wait for the client's initial empty commit; sending
//         configure eagerly is slightly non-standard but works for
//         every client we've seen in testing. If this bites us,
//         track a "configure_needed" flag on the surface and fire
//         on first commit.
//  3. Client acks configure, attaches buffer, commits.
//  4. Server just logs the commit for now.
//
//Buffer import, damage tracking, frame pacing, popup positioning --
//all future passes. This file is a protocol compliance layer only.
//
//Depends on:
//  * wayland-server-core.h  (wl_resource, wl_global, etc.)
//  * xdg-shell-protocol.h   (wayland-scanner server-header output)
//

#include <stdlib.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "xdg-shell-protocol.h"

#include "types.h"
#include "third_party/log.h"
#include "clib/memory_manager.h"

#include <string.h>

#include "globals.h"           //for globals__wl_surface_set_role_hook().
#include "output_drm.h"        //for panel native size in deferred configure.
#include "meek_shell_v1.h"     //for shell-client detection + toplevel event fires.
#include "fractional_scale.h"  //for Level-2 configure scale-down (client-capability gate).
#include "xdg_shell.h"

//
//monotonically-increasing configure serial. Shared across all
//toplevels/popups in the compositor. 32-bit is fine -- wrap-around
//at ~4 billion configures is not a real concern.
//
static uint32_t _xdg_shell_internal__next_serial = 1;

//
//Monotonic counter for the meek_shell_v1 toplevel handle space.
//Starts at 1 because 0 is reserved as "this toplevel has no handle"
//(used for the shell's own toplevels, which we don't forward).
//32-bit wrap is not realistic -- a phone session churning through
//2^32 windows is beyond any realistic workload.
//
static uint32_t _xdg_shell_internal__next_handle = 1;

//
//Head of the doubly-linked list of all live xdg_surface structs.
//Every xdg_surface inserts itself on `get_xdg_surface` and removes
//in its destroy callback. Exists so C6 can iterate all client
//windows to forward their buffers via meek_shell_v1 without
//building a second index.
//
//Using libwayland's wl_list (intrusive doubly-linked) instead of
//rolling our own; wl_list_for_each is the idiomatic iteration
//pattern across libwayland-based code.
//
static struct wl_list _xdg_shell_internal__xdg_surfaces; //initialized in xdg_shell__register.

//
//forward decls for file-local statics
//

//-- xdg_positioner (stub) --
struct _xdg_shell_internal__positioner;
static void _xdg_shell_internal__positioner__destroy_resource(struct wl_resource* r);
static void _xdg_shell_internal__positioner__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _xdg_shell_internal__positioner__on_set_size(struct wl_client* c, struct wl_resource* r, int32_t w, int32_t h);
static void _xdg_shell_internal__positioner__on_set_anchor_rect(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h);
static void _xdg_shell_internal__positioner__on_set_anchor(struct wl_client* c, struct wl_resource* r, uint32_t anchor);
static void _xdg_shell_internal__positioner__on_set_gravity(struct wl_client* c, struct wl_resource* r, uint32_t gravity);
static void _xdg_shell_internal__positioner__on_set_constraint_adjustment(struct wl_client* c, struct wl_resource* r, uint32_t constraint_adjustment);
static void _xdg_shell_internal__positioner__on_set_offset(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y);
static void _xdg_shell_internal__positioner__on_set_reactive(struct wl_client* c, struct wl_resource* r);
static void _xdg_shell_internal__positioner__on_set_parent_size(struct wl_client* c, struct wl_resource* r, int32_t parent_w, int32_t parent_h);
static void _xdg_shell_internal__positioner__on_set_parent_configure(struct wl_client* c, struct wl_resource* r, uint32_t serial);

//-- xdg_popup (destroy-only stub) --
struct _xdg_shell_internal__popup;
static void _xdg_shell_internal__popup__destroy_resource(struct wl_resource* r);
static void _xdg_shell_internal__popup__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _xdg_shell_internal__popup__on_grab(struct wl_client* c, struct wl_resource* r, struct wl_resource* seat, uint32_t serial);
static void _xdg_shell_internal__popup__on_reposition(struct wl_client* c, struct wl_resource* r, struct wl_resource* positioner, uint32_t token);

//-- xdg_toplevel --
struct _xdg_shell_internal__toplevel;
static void _xdg_shell_internal__toplevel__destroy_resource(struct wl_resource* r);
static void _xdg_shell_internal__toplevel__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _xdg_shell_internal__toplevel__on_set_parent(struct wl_client* c, struct wl_resource* r, struct wl_resource* parent);
static void _xdg_shell_internal__toplevel__on_set_title(struct wl_client* c, struct wl_resource* r, const char* title);
static void _xdg_shell_internal__toplevel__on_set_app_id(struct wl_client* c, struct wl_resource* r, const char* app_id);
static void _xdg_shell_internal__toplevel__on_show_window_menu(struct wl_client* c, struct wl_resource* r, struct wl_resource* seat, uint32_t serial, int32_t x, int32_t y);
static void _xdg_shell_internal__toplevel__on_move(struct wl_client* c, struct wl_resource* r, struct wl_resource* seat, uint32_t serial);
static void _xdg_shell_internal__toplevel__on_resize(struct wl_client* c, struct wl_resource* r, struct wl_resource* seat, uint32_t serial, uint32_t edges);
static void _xdg_shell_internal__toplevel__on_set_max_size(struct wl_client* c, struct wl_resource* r, int32_t w, int32_t h);
static void _xdg_shell_internal__toplevel__on_set_min_size(struct wl_client* c, struct wl_resource* r, int32_t w, int32_t h);
static void _xdg_shell_internal__toplevel__on_set_maximized(struct wl_client* c, struct wl_resource* r);
static void _xdg_shell_internal__toplevel__on_unset_maximized(struct wl_client* c, struct wl_resource* r);
static void _xdg_shell_internal__toplevel__on_set_fullscreen(struct wl_client* c, struct wl_resource* r, struct wl_resource* output);
static void _xdg_shell_internal__toplevel__on_unset_fullscreen(struct wl_client* c, struct wl_resource* r);
static void _xdg_shell_internal__toplevel__on_set_minimized(struct wl_client* c, struct wl_resource* r);

//-- xdg_surface --
struct _xdg_shell_internal__xdg_surface;
static void _xdg_shell_internal__xdg_surface__destroy_resource(struct wl_resource* r);
static void _xdg_shell_internal__xdg_surface__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _xdg_shell_internal__xdg_surface__on_get_toplevel(struct wl_client* c, struct wl_resource* r, uint32_t id);
static void _xdg_shell_internal__xdg_surface__on_get_popup(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* parent, struct wl_resource* positioner);
static void _xdg_shell_internal__xdg_surface__on_set_window_geometry(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h);
static void _xdg_shell_internal__xdg_surface__on_ack_configure(struct wl_client* c, struct wl_resource* r, uint32_t serial);

//-- deferred configure hook (called from globals.c on wl_surface.commit) --
static void _xdg_shell_internal__on_wl_surface_commit(void* role_data);

//-- destroy-listener callback when the target wl_surface dies --
static void _xdg_shell_internal__on_wl_surface_destroyed(struct wl_listener* listener, void* data);

//-- xdg_wm_base --
static void _xdg_shell_internal__wm_base__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _xdg_shell_internal__wm_base__on_create_positioner(struct wl_client* c, struct wl_resource* r, uint32_t id);
static void _xdg_shell_internal__wm_base__on_get_xdg_surface(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* surface);
static void _xdg_shell_internal__wm_base__on_pong(struct wl_client* c, struct wl_resource* r, uint32_t serial);
static void _xdg_shell_internal__wm_base__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id);

//
//per-client-per-xdg_surface state. The xdg_surface is the "role"
//layer on top of wl_surface -- it knows whether the surface is a
//toplevel or popup, and mediates configure/ack.
//
//ROLE invariant (xdg_wm_base spec): an xdg_surface can be assigned
//at most ONE role via get_toplevel OR get_popup. Once assigned,
//re-assigning is a protocol error (XDG_WM_BASE_ERROR_ROLE). We
//track this on the `role` field and enforce at both role-assignment
//paths.
//
enum _xdg_shell_internal__xdg_surface_role
{
    _XDG_SURFACE_ROLE_NONE = 0,
    _XDG_SURFACE_ROLE_TOPLEVEL,
    _XDG_SURFACE_ROLE_POPUP,
};

struct _xdg_shell_internal__xdg_surface
{
    //
    //Link into the module-level _xdg_shell_internal__xdg_surfaces
    //list. wl_list_insert on creation, wl_list_remove on destroy.
    //Standard libwayland intrusive-list pattern.
    //
    struct wl_list link;

    struct wl_resource* resource;
    struct wl_resource* wl_surface; // raw wl_surface resource; not owned. NULL after wl_surface destroyed.

    //
    //Destroy listener on wl_surface so we learn when it goes
    //away. Spec says clients MUST destroy xdg_surface before
    //wl_surface, but misbehaving / crashing clients may not. If
    //wl_surface dies first and we later try to clear the role
    //hook on it in our own destroy path, we'd UAF. With this
    //listener we null `wl_surface` the moment it's gone and the
    //subsequent destroy path skips the hook-clear.
    //
    struct wl_listener wl_surface_destroy_listener;

    enum _xdg_shell_internal__xdg_surface_role role;

    //
    //Back-pointers to the role object (at most one is non-NULL;
    //which one depends on `role`). Set when get_toplevel /
    //get_popup runs. Needed so the deferred-configure hook can
    //fire the role-specific configure event (xdg_toplevel.configure
    //vs xdg_popup.configure) BEFORE the xdg_surface.configure.
    //
    struct _xdg_shell_internal__toplevel* toplevel;
    struct _xdg_shell_internal__popup*    popup;

    //
    //Deferred-configure state. Set to 1 on role assignment; the
    //wl_surface commit hook fires configure pair + clears. After
    //that, subsequent commits go through the hook but it's a no-op.
    //Restoring to 1 could be done for resize-triggered configures
    //but A2.2+ doesn't do resize yet.
    //
    int pending_configure;
};

struct _xdg_shell_internal__toplevel
{
    struct wl_resource* resource;
    struct _xdg_shell_internal__xdg_surface* xdg_surface;

    //
    //meek_shell_v1 handle: compositor-assigned u32 that uniquely
    //identifies this toplevel across the meek_shell_v1 protocol.
    //Non-zero for toplevels belonging to OTHER clients (regular
    //apps); zero for the shell's own toplevels (we never forward
    //those to itself).
    //
    uint32_t handle;

    //
    //Title + app_id strings (strdup'd from client-provided
    //values). NULL until the client sets them. Kept so we can
    //replay toplevel_added when the shell connects late, and to
    //track the current title for toplevel_title_changed.
    //
    char* app_id;
    char* title;

    //
    //Last xdg_toplevel.configure(w, h) we sent. Tracked so
    //fractional-scale reconfigure (and eventually input routing)
    //can look up the client's current logical size. 0 means "not
    //yet configured".
    //
    int configured_w;
    int configured_h;

    //
    //Last buffer dimensions committed by the client on this
    //toplevel's surface. Updated from the surface.c commit path
    //via xdg_shell__record_buffer_size(). The shell routes tap
    //coords in buffer-sample space; we translate to surface-local
    //by the ratio logical / buffer before dispatching wl_touch.
    //0 means "no buffer committed yet".
    //
    int last_buffer_w;
    int last_buffer_h;
};

struct _xdg_shell_internal__popup
{
    struct wl_resource* resource;
    struct _xdg_shell_internal__xdg_surface* xdg_surface;
};

struct _xdg_shell_internal__positioner
{
    struct wl_resource* resource;
};

// ============================================================
// xdg_positioner (stubs)
// ============================================================

static const struct xdg_positioner_interface _xdg_shell_internal__positioner_impl = {
    .destroy                 = _xdg_shell_internal__positioner__on_destroy,
    .set_size                = _xdg_shell_internal__positioner__on_set_size,
    .set_anchor_rect         = _xdg_shell_internal__positioner__on_set_anchor_rect,
    .set_anchor              = _xdg_shell_internal__positioner__on_set_anchor,
    .set_gravity             = _xdg_shell_internal__positioner__on_set_gravity,
    .set_constraint_adjustment = _xdg_shell_internal__positioner__on_set_constraint_adjustment,
    .set_offset              = _xdg_shell_internal__positioner__on_set_offset,
    .set_reactive            = _xdg_shell_internal__positioner__on_set_reactive,
    .set_parent_size         = _xdg_shell_internal__positioner__on_set_parent_size,
    .set_parent_configure    = _xdg_shell_internal__positioner__on_set_parent_configure,
};

static void _xdg_shell_internal__positioner__destroy_resource(struct wl_resource* r)
{
    struct _xdg_shell_internal__positioner* p = wl_resource_get_user_data(r);
    if (p != NULL)
    {
        GUI_FREE(p);
    }
}

static void _xdg_shell_internal__positioner__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void _xdg_shell_internal__positioner__on_set_size(struct wl_client* c, struct wl_resource* r, int32_t w, int32_t h)
{ (void)c; (void)r; (void)w; (void)h; }
static void _xdg_shell_internal__positioner__on_set_anchor_rect(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static void _xdg_shell_internal__positioner__on_set_anchor(struct wl_client* c, struct wl_resource* r, uint32_t anchor)
{ (void)c; (void)r; (void)anchor; }
static void _xdg_shell_internal__positioner__on_set_gravity(struct wl_client* c, struct wl_resource* r, uint32_t gravity)
{ (void)c; (void)r; (void)gravity; }
static void _xdg_shell_internal__positioner__on_set_constraint_adjustment(struct wl_client* c, struct wl_resource* r, uint32_t constraint_adjustment)
{ (void)c; (void)r; (void)constraint_adjustment; }
static void _xdg_shell_internal__positioner__on_set_offset(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y)
{ (void)c; (void)r; (void)x; (void)y; }
static void _xdg_shell_internal__positioner__on_set_reactive(struct wl_client* c, struct wl_resource* r)
{ (void)c; (void)r; }
static void _xdg_shell_internal__positioner__on_set_parent_size(struct wl_client* c, struct wl_resource* r, int32_t parent_w, int32_t parent_h)
{ (void)c; (void)r; (void)parent_w; (void)parent_h; }
static void _xdg_shell_internal__positioner__on_set_parent_configure(struct wl_client* c, struct wl_resource* r, uint32_t serial)
{ (void)c; (void)r; (void)serial; }

// ============================================================
// xdg_popup (destroy-only stub)
// ============================================================

static const struct xdg_popup_interface _xdg_shell_internal__popup_impl = {
    .destroy    = _xdg_shell_internal__popup__on_destroy,
    .grab       = _xdg_shell_internal__popup__on_grab,
    .reposition = _xdg_shell_internal__popup__on_reposition,
};

static void _xdg_shell_internal__popup__destroy_resource(struct wl_resource* r)
{
    struct _xdg_shell_internal__popup* p = wl_resource_get_user_data(r);
    if (p != NULL)
    {
        GUI_FREE(p);
    }
}

static void _xdg_shell_internal__popup__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _xdg_shell_internal__popup__on_grab(struct wl_client* c, struct wl_resource* r, struct wl_resource* seat, uint32_t serial)
{
    (void)c; (void)r; (void)seat; (void)serial;
    log_trace("xdg_popup.grab (stub)");
}

static void _xdg_shell_internal__popup__on_reposition(struct wl_client* c, struct wl_resource* r, struct wl_resource* positioner, uint32_t token)
{
    (void)c; (void)r; (void)positioner; (void)token;
    log_trace("xdg_popup.reposition (stub)");
}

// ============================================================
// xdg_toplevel
// ============================================================

static const struct xdg_toplevel_interface _xdg_shell_internal__toplevel_impl = {
    .destroy           = _xdg_shell_internal__toplevel__on_destroy,
    .set_parent        = _xdg_shell_internal__toplevel__on_set_parent,
    .set_title         = _xdg_shell_internal__toplevel__on_set_title,
    .set_app_id        = _xdg_shell_internal__toplevel__on_set_app_id,
    .show_window_menu  = _xdg_shell_internal__toplevel__on_show_window_menu,
    .move              = _xdg_shell_internal__toplevel__on_move,
    .resize            = _xdg_shell_internal__toplevel__on_resize,
    .set_max_size      = _xdg_shell_internal__toplevel__on_set_max_size,
    .set_min_size      = _xdg_shell_internal__toplevel__on_set_min_size,
    .set_maximized     = _xdg_shell_internal__toplevel__on_set_maximized,
    .unset_maximized   = _xdg_shell_internal__toplevel__on_unset_maximized,
    .set_fullscreen    = _xdg_shell_internal__toplevel__on_set_fullscreen,
    .unset_fullscreen  = _xdg_shell_internal__toplevel__on_unset_fullscreen,
    .set_minimized     = _xdg_shell_internal__toplevel__on_set_minimized,
};

static void _xdg_shell_internal__toplevel__destroy_resource(struct wl_resource* r)
{
    struct _xdg_shell_internal__toplevel* t = wl_resource_get_user_data(r);
    if (t != NULL)
    {
        //
        //Fire toplevel_removed (only for non-shell toplevels, i.e.
        //handle != 0). No-op if shell isn't bound / ready.
        //
        if (t->handle != 0)
        {
            meek_shell_v1__fire_toplevel_removed(t->handle);
        }
        if (t->app_id != NULL) free(t->app_id);
        if (t->title  != NULL) free(t->title);
        log_trace("xdg_toplevel %p destroyed (handle=%u)", (void*)t, t->handle);
        GUI_FREE(t);
    }
}

static void _xdg_shell_internal__toplevel__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _xdg_shell_internal__toplevel__on_set_parent(struct wl_client* c, struct wl_resource* r, struct wl_resource* parent)
{
    (void)c; (void)r; (void)parent;
}

static void _xdg_shell_internal__toplevel__on_set_title(struct wl_client* c, struct wl_resource* r, const char* title)
{
    (void)c;
    struct _xdg_shell_internal__toplevel* t = wl_resource_get_user_data(r);
    if (t == NULL) return;

    //
    //Replace stored title. Free previous copy first. If strdup
    //fails (OOM) we just leave the title NULL -- not worth killing
    //the client over a window title.
    //
    if (t->title != NULL) { free(t->title); t->title = NULL; }
    if (title != NULL) t->title = strdup(title);

    //
    //Notify the shell only if this is a shell-observable toplevel.
    //Fire is a no-op pre-announce_ready (shell replays at that time).
    //
    if (t->handle != 0)
    {
        meek_shell_v1__fire_toplevel_title_changed(t->handle, t->title);
    }
    log_trace("xdg_toplevel.set_title %p title=\"%s\" handle=%u",
              (void*)t, title != NULL ? title : "(null)", t->handle);
}

static void _xdg_shell_internal__toplevel__on_set_app_id(struct wl_client* c, struct wl_resource* r, const char* app_id)
{
    (void)c;
    struct _xdg_shell_internal__toplevel* t = wl_resource_get_user_data(r);
    if (t == NULL) return;

    if (t->app_id != NULL) { free(t->app_id); t->app_id = NULL; }
    if (app_id != NULL) t->app_id = strdup(app_id);

    //
    //No dedicated app_id_changed event in v1 of meek_shell_v1.
    //The app_id is exposed only via toplevel_added / the replay
    //path. Clients that change app_id post-creation (rare) will
    //need a v2 event or toplevel_added replay.
    //
    log_trace("xdg_toplevel.set_app_id %p app_id=\"%s\" handle=%u",
              (void*)t, app_id != NULL ? app_id : "(null)", t->handle);
}

static void _xdg_shell_internal__toplevel__on_show_window_menu(struct wl_client* c, struct wl_resource* r, struct wl_resource* seat, uint32_t serial, int32_t x, int32_t y)
{ (void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y; }
static void _xdg_shell_internal__toplevel__on_move(struct wl_client* c, struct wl_resource* r, struct wl_resource* seat, uint32_t serial)
{ (void)c; (void)r; (void)seat; (void)serial; }
static void _xdg_shell_internal__toplevel__on_resize(struct wl_client* c, struct wl_resource* r, struct wl_resource* seat, uint32_t serial, uint32_t edges)
{ (void)c; (void)r; (void)seat; (void)serial; (void)edges; }
static void _xdg_shell_internal__toplevel__on_set_max_size(struct wl_client* c, struct wl_resource* r, int32_t w, int32_t h)
{ (void)c; (void)r; (void)w; (void)h; }
static void _xdg_shell_internal__toplevel__on_set_min_size(struct wl_client* c, struct wl_resource* r, int32_t w, int32_t h)
{ (void)c; (void)r; (void)w; (void)h; }
static void _xdg_shell_internal__toplevel__on_set_maximized(struct wl_client* c, struct wl_resource* r)
{ (void)c; (void)r; }
static void _xdg_shell_internal__toplevel__on_unset_maximized(struct wl_client* c, struct wl_resource* r)
{ (void)c; (void)r; }
static void _xdg_shell_internal__toplevel__on_set_fullscreen(struct wl_client* c, struct wl_resource* r, struct wl_resource* output)
{ (void)c; (void)r; (void)output; log_trace("dbg: on_set_fullscreen entry r=%p output=%p", (void*)r, (void*)output); }
static void _xdg_shell_internal__toplevel__on_unset_fullscreen(struct wl_client* c, struct wl_resource* r)
{ (void)c; (void)r; }
static void _xdg_shell_internal__toplevel__on_set_minimized(struct wl_client* c, struct wl_resource* r)
{ (void)c; (void)r; }

// ============================================================
// xdg_surface
// ============================================================

static const struct xdg_surface_interface _xdg_shell_internal__xdg_surface_impl = {
    .destroy             = _xdg_shell_internal__xdg_surface__on_destroy,
    .get_toplevel        = _xdg_shell_internal__xdg_surface__on_get_toplevel,
    .get_popup           = _xdg_shell_internal__xdg_surface__on_get_popup,
    .set_window_geometry = _xdg_shell_internal__xdg_surface__on_set_window_geometry,
    .ack_configure       = _xdg_shell_internal__xdg_surface__on_ack_configure,
};

static void _xdg_shell_internal__xdg_surface__destroy_resource(struct wl_resource* r)
{
    struct _xdg_shell_internal__xdg_surface* x = wl_resource_get_user_data(r);
    if (x != NULL)
    {
        //
        //Unlink from the tracking list. wl_list_remove is
        //safe to call on a live link (the macro does both
        //prev and next pointer fixups).
        //
        wl_list_remove(&x->link);

        //
        //If wl_surface is still alive (spec-correct destroy
        //order: xdg_surface first, wl_surface later), remove our
        //destroy listener from it + clear the role hook. If
        //wl_surface died first (set by our destroy-listener
        //callback), x->wl_surface is NULL and both ops are
        //skipped.
        //
        if (x->wl_surface != NULL)
        {
            wl_list_remove(&x->wl_surface_destroy_listener.link);
            globals__wl_surface_set_role_hook(x->wl_surface, NULL, NULL);
        }
        log_trace("xdg_surface %p destroyed", (void*)x);
        GUI_FREE(x);
    }
}

//
//Fires when the client destroys the target wl_surface while
//our xdg_surface is still alive. The wl_surface is destroyed
//AFTER this callback returns, so x->wl_surface is still a valid
//read while we're here, but will be dangling after. Null it so
//the xdg_surface destroy path knows not to touch the role hook.
//We also pull ourselves out of the destroy-listener list via
//wl_list_remove so we don't fire a second time if the listener
//struct is reused.
//
static void _xdg_shell_internal__on_wl_surface_destroyed(struct wl_listener* listener, void* data)
{
    (void)data;
    //
    //Recover the enclosing xdg_surface via the standard
    //wl_container_of pattern on the listener's offset in the
    //struct. This is libwayland's conventional way to attach
    //arbitrary context to a listener.
    //
    struct _xdg_shell_internal__xdg_surface* xs =
        wl_container_of(listener, xs, wl_surface_destroy_listener);

    log_trace("xdg_surface %p: wl_surface died under us; nulling back-pointer",
              (void*)xs);
    wl_list_remove(&listener->link);
    xs->wl_surface = NULL;
}

//
//The deferred-configure hook. Called from globals.c's wl_surface
//commit handler after the pending->current buffer swap. Fires the
//role-specific configure event (xdg_toplevel or xdg_popup) plus
//xdg_surface.configure on the first empty commit; no-op thereafter.
//
static void _xdg_shell_internal__on_wl_surface_commit(void* role_data)
{
    log_trace("dbg: on_wl_surface_commit entry role_data=%p", role_data);
    struct _xdg_shell_internal__xdg_surface* xs = role_data;
    if (xs == NULL || !xs->pending_configure)
    {
        log_trace("dbg: on_wl_surface_commit early-out xs=%p pending_configure=%d",
                  (void*)xs, xs ? xs->pending_configure : -1);
        return;
    }
    log_trace("dbg: on_wl_surface_commit past early-out; role=%d toplevel=%p",
              xs->role, (void*)xs->toplevel);

    if (xs->role == _XDG_SURFACE_ROLE_TOPLEVEL && xs->toplevel != NULL)
    {
        struct wl_resource* t_res    = xs->toplevel->resource;
        uint32_t            t_version = wl_resource_get_version(t_res);

        //
        //v5+ sends the wm_capabilities event right before
        //configure. Carries a list of window-menu capabilities
        //we support: none yet (maximize/minimize/fullscreen/
        //window_menu all stubbed), so we send an empty array.
        //
        if (t_version >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
        {
            struct wl_array caps;
            wl_array_init(&caps);
            xdg_toplevel_send_wm_capabilities(t_res, &caps);
            wl_array_release(&caps);
        }

        //
        //Configure size — client-gated shrink with min clamp.
        //
        //  * Fractional-aware client (bound wp_fractional_scale_manager_v1):
        //    logical = panel / preferred_scale, clamped to MIN 720 wide
        //    (aspect preserved). Client renders at full preferred_scale,
        //    producing a buffer approximately panel-sized; shell paints
        //    1:1 with visibly bigger UI than panel-native would give.
        //  * Non-aware client (shell, meek-ui): panel-native (no shrink).
        //
        //720 floor avoids GTK4 / libadwaita widgets hitting their
        //internal minimum-size assertions -- below ~600 wide their
        //layout math produces negative-size contents and they abort
        //on first paint / tap.
        //
        int cfg_w = 0, cfg_h = 0;
        (void)output_drm__get_native_size(&cfg_w, &cfg_h);

        #define _XDG_SHELL_LEVEL2_MIN_LOGICAL_W 720

        int logical_w = cfg_w;
        int logical_h = cfg_h;
        struct wl_client* cli = wl_resource_get_client(t_res);
        int used_scaled = 0;
        if (cli != NULL && cfg_w > 0 && cfg_h > 0 &&
            fractional_scale__client_has_bound_manager(cli))
        {
            uint32_t wire_scale = fractional_scale__get_preferred_scale();
            if (wire_scale > 120)
            {
                int shrunk_w = (int)((long)cfg_w * 120 / (long)wire_scale);
                int shrunk_h = (int)((long)cfg_h * 120 / (long)wire_scale);
                if (shrunk_w < _XDG_SHELL_LEVEL2_MIN_LOGICAL_W)
                {
                    shrunk_w = _XDG_SHELL_LEVEL2_MIN_LOGICAL_W;
                    shrunk_h = (int)((long)cfg_h * (long)shrunk_w / (long)cfg_w);
                }
                logical_w = shrunk_w;
                logical_h = shrunk_h;
                used_scaled = 1;
            }
        }

        //
        // States array: ACTIVATED + FULLSCREEN. GTK4 / GDK gate
        // input event dispatch on ACTIVATED being set -- without it
        // GtkGestureClick silently drops wl_touch.down even though
        // the bytes reach the client socket (verified via strace).
        // Standard practice is to always include ACTIVATED when the
        // surface has focus; we're always-focused from the single-
        // app-at-a-time shell perspective. FULLSCREEN tells the
        // client to drop its own chrome since we paint fullscreen
        // anyway.
        //
        struct wl_array states;
        wl_array_init(&states);
        uint32_t* sp;
        sp = wl_array_add(&states, sizeof(uint32_t));
        if (sp != NULL) { *sp = XDG_TOPLEVEL_STATE_ACTIVATED; }
        sp = wl_array_add(&states, sizeof(uint32_t));
        if (sp != NULL) { *sp = XDG_TOPLEVEL_STATE_FULLSCREEN; }
        xdg_toplevel_send_configure(t_res, logical_w, logical_h, &states);
        wl_array_release(&states);
        xs->toplevel->configured_w = logical_w;
        xs->toplevel->configured_h = logical_h;

        log_trace("xdg_toplevel.configure sent %dx%d (panel=%dx%d, %s, states=activated|fullscreen)",
                  logical_w, logical_h, cfg_w, cfg_h,
                  used_scaled ? "scale-adjusted min-clamped" : "panel-native");
    }
    //
    //popup configure requires xdg_popup.configure(x, y, w, h) with
    //values from the positioner. Popups are stubs in A2.2 -- skip
    //here; real impl lands when we wire positioner arithmetic.
    //

    xdg_surface_send_configure(xs->resource, _xdg_shell_internal__next_serial++);
    xs->pending_configure = 0;

    log_trace("xdg_surface deferred configure fired (%s serial prev=%u)",
              xs->role == _XDG_SURFACE_ROLE_TOPLEVEL ? "toplevel" : "popup/other",
              _xdg_shell_internal__next_serial - 1);
}

static void _xdg_shell_internal__xdg_surface__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _xdg_shell_internal__xdg_surface__on_get_toplevel(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    struct _xdg_shell_internal__xdg_surface* xs = wl_resource_get_user_data(r);

    //
    //Role enforcement: per xdg_wm_base spec, an xdg_surface may
    //have at most one role. Re-assignment is a protocol error.
    //
    if (xs->role != _XDG_SURFACE_ROLE_NONE)
    {
        wl_resource_post_error(xs->resource,
                               XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "xdg_surface already has a role assigned (get_toplevel called on %s-roled surface)",
                               xs->role == _XDG_SURFACE_ROLE_TOPLEVEL ? "toplevel" : "popup");
        return;
    }

    struct _xdg_shell_internal__toplevel* t = GUI_MALLOC_T(sizeof(*t), MM_TYPE_NODE);
    if (t == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }

    uint32_t version = wl_resource_get_version(r);
    t->resource = wl_resource_create(c, &xdg_toplevel_interface, version, id);
    if (t->resource == NULL)
    {
        GUI_FREE(t);
        wl_client_post_no_memory(c);
        return;
    }
    t->xdg_surface = xs;
    t->handle      = 0;     //assigned below if this isn't the shell.
    t->app_id      = NULL;
    t->title       = NULL;
    xs->role              = _XDG_SURFACE_ROLE_TOPLEVEL;
    xs->toplevel          = t;
    xs->pending_configure = 1;

    wl_resource_set_implementation(t->resource,
                                   &_xdg_shell_internal__toplevel_impl,
                                   t,
                                   _xdg_shell_internal__toplevel__destroy_resource);

    //
    //Assign a meek_shell_v1 handle to every toplevel that's NOT
    //the shell itself. The shell is identified by wl_client
    //equality against meek_shell_v1's currently-bound client.
    //Shell's own windows aren't forwarded (doesn't observe itself).
    //
    //Fire toplevel_added immediately; the shell may or may not be
    //bound + ready. If it's not, the event fire no-ops silently;
    //on eventual announce_ready, xdg_shell__foreach_toplevel_for_
    //replay walks the list and replays.
    //
    if (c != meek_shell_v1__get_shell_client())
    {
        t->handle = _xdg_shell_internal__next_handle++;
        meek_shell_v1__fire_toplevel_added(t->handle, /*app_id*/NULL, /*title*/NULL);
        log_trace("xdg_surface.get_toplevel -> %p handle=%u (id=%u v=%u; non-shell)",
                  (void*)t, t->handle, id, version);
    }
    else
    {
        log_trace("xdg_surface.get_toplevel -> %p (id=%u v=%u; shell's own, no handle)",
                  (void*)t, id, version);
    }
}

static void _xdg_shell_internal__xdg_surface__on_get_popup(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* parent, struct wl_resource* positioner)
{
    struct _xdg_shell_internal__xdg_surface* xs = wl_resource_get_user_data(r);
    (void)parent;
    (void)positioner;

    //
    //Role enforcement -- same as get_toplevel.
    //
    if (xs->role != _XDG_SURFACE_ROLE_NONE)
    {
        wl_resource_post_error(xs->resource,
                               XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "xdg_surface already has a role assigned (get_popup called on %s-roled surface)",
                               xs->role == _XDG_SURFACE_ROLE_TOPLEVEL ? "toplevel" : "popup");
        return;
    }

    struct _xdg_shell_internal__popup* p = GUI_MALLOC_T(sizeof(*p), MM_TYPE_NODE);
    if (p == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }

    uint32_t version = wl_resource_get_version(r);
    p->resource = wl_resource_create(c, &xdg_popup_interface, version, id);
    if (p->resource == NULL)
    {
        GUI_FREE(p);
        wl_client_post_no_memory(c);
        return;
    }
    p->xdg_surface = xs;

    wl_resource_set_implementation(p->resource,
                                   &_xdg_shell_internal__popup_impl,
                                   p,
                                   _xdg_shell_internal__popup__destroy_resource);
    xs->role              = _XDG_SURFACE_ROLE_POPUP;
    xs->popup             = p;
    xs->pending_configure = 1;
    log_trace("xdg_surface.get_popup -> %p (id=%u v=%u; positioner ignored in A2.2; configure deferred)",
              (void*)p, id, version);
}

static void _xdg_shell_internal__xdg_surface__on_set_window_geometry(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r;
    log_trace("xdg_surface.set_window_geometry %d,%d %dx%d", x, y, w, h);
}

static void _xdg_shell_internal__xdg_surface__on_ack_configure(struct wl_client* c, struct wl_resource* r, uint32_t serial)
{
    (void)c; (void)r;
    log_trace("xdg_surface.ack_configure serial=%u", serial);
}

// ============================================================
// xdg_wm_base
// ============================================================

static const struct xdg_wm_base_interface _xdg_shell_internal__wm_base_impl = {
    .destroy           = _xdg_shell_internal__wm_base__on_destroy,
    .create_positioner = _xdg_shell_internal__wm_base__on_create_positioner,
    .get_xdg_surface   = _xdg_shell_internal__wm_base__on_get_xdg_surface,
    .pong              = _xdg_shell_internal__wm_base__on_pong,
};

static void _xdg_shell_internal__wm_base__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    //
    //Spec: destroying xdg_wm_base while xdg_surface/xdg_toplevel
    //children are alive is a protocol error. We don't track the
    //child list yet, so just accept the destroy.
    //
    wl_resource_destroy(r);
}

static void _xdg_shell_internal__wm_base__on_create_positioner(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    (void)r;
    struct _xdg_shell_internal__positioner* p = GUI_MALLOC_T(sizeof(*p), MM_TYPE_NODE);
    if (p == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }

    uint32_t version = wl_resource_get_version(r);
    p->resource = wl_resource_create(c, &xdg_positioner_interface, version, id);
    if (p->resource == NULL)
    {
        GUI_FREE(p);
        wl_client_post_no_memory(c);
        return;
    }

    wl_resource_set_implementation(p->resource,
                                   &_xdg_shell_internal__positioner_impl,
                                   p,
                                   _xdg_shell_internal__positioner__destroy_resource);
    log_trace("xdg_wm_base.create_positioner -> %p (id=%u v=%u)",
              (void*)p, id, version);
}

static void _xdg_shell_internal__wm_base__on_get_xdg_surface(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* surface)
{
    (void)r;
    struct _xdg_shell_internal__xdg_surface* x = GUI_MALLOC_T(sizeof(*x), MM_TYPE_NODE);
    if (x == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }

    uint32_t version = wl_resource_get_version(r);
    x->resource          = wl_resource_create(c, &xdg_surface_interface, version, id);
    x->wl_surface        = surface;
    x->role              = _XDG_SURFACE_ROLE_NONE;
    x->toplevel          = NULL;
    x->popup             = NULL;
    x->pending_configure = 0;
    x->wl_surface_destroy_listener.notify = _xdg_shell_internal__on_wl_surface_destroyed;
    if (x->resource == NULL)
    {
        GUI_FREE(x);
        wl_client_post_no_memory(c);
        return;
    }

    wl_resource_set_implementation(x->resource,
                                   &_xdg_shell_internal__xdg_surface_impl,
                                   x,
                                   _xdg_shell_internal__xdg_surface__destroy_resource);

    //
    //Add to the global tracking list. Enables iteration from
    //C6 (buffer forwarding) and C7 (input hit-test result
    //validation).
    //
    wl_list_insert(&_xdg_shell_internal__xdg_surfaces, &x->link);

    //
    //Install the wl_surface commit hook so we can fire deferred
    //configure on first empty commit. The hook stays installed
    //for the lifetime of the xdg_surface; on xdg_surface destroy
    //we clear it to avoid dangling callback.
    //
    globals__wl_surface_set_role_hook(
        surface,
        _xdg_shell_internal__on_wl_surface_commit,
        x);

    //
    //Register a destroy listener on the wl_surface so that if it
    //dies first (spec-violating but defensive), we null our
    //back-pointer and skip the role-hook-clear in our own
    //destroy path. Without this, the destroy path is a UAF.
    //
    wl_resource_add_destroy_listener(surface, &x->wl_surface_destroy_listener);

    log_trace("xdg_wm_base.get_xdg_surface wl_surface=%p -> %p (id=%u v=%u)",
              (void*)surface, (void*)x, id, version);
}

static void _xdg_shell_internal__wm_base__on_pong(struct wl_client* c, struct wl_resource* r, uint32_t serial)
{
    (void)c; (void)r;
    log_trace("xdg_wm_base.pong serial=%u", serial);
    //
    //We don't fire pings yet, so this shouldn't arrive. If it does
    //(spurious pong), just log.
    //
}

static void _xdg_shell_internal__wm_base__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* r = wl_resource_create(client, &xdg_wm_base_interface, version, id);
    if (r == NULL)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &_xdg_shell_internal__wm_base_impl, NULL, NULL);
    log_trace("xdg_wm_base bind id=%u v=%u", id, version);
}

// ============================================================
// Public entry
// ============================================================

void xdg_shell__register(struct wl_display* display)
{
    //
    //Initialize the tracking list. Must run BEFORE any xdg_surface
    //could be created (no race in practice because register runs
    //from globals__register_all which is called at startup, before
    //any client connects).
    //
    wl_list_init(&_xdg_shell_internal__xdg_surfaces);

    //
    //xdg_wm_base v5 is current stable. v6 exists (adds
    //suspended_state on xdg_toplevel) but clients that care about
    //it handle v5 gracefully. Advertise v5 for broadest
    //compatibility; bump when we implement the newer events.
    //
    struct wl_global* g = wl_global_create(display,
                                           &xdg_wm_base_interface,
                                           /*version*/ 5,
                                           /*data*/    NULL,
                                           _xdg_shell_internal__wm_base__bind);
    if (g == NULL)
    {
        log_fatal("wl_global_create(xdg_wm_base) failed");
        return;
    }
    log_info("xdg-shell registered: xdg_wm_base v5");
}

//
// Public helpers for meek_shell_v1 buffer forwarding + replay.
// See xdg_shell.h for contracts.
//

struct wl_resource* xdg_shell__find_wl_surface_by_handle(uint32_t handle)
{
    if (handle == 0) { return NULL; }
    struct _xdg_shell_internal__xdg_surface* x;
    wl_list_for_each(x, &_xdg_shell_internal__xdg_surfaces, link)
    {
        if (x->role == _XDG_SURFACE_ROLE_TOPLEVEL &&
            x->toplevel != NULL &&
            x->toplevel->handle == handle)
        {
            return x->wl_surface;
        }
    }
    return NULL;
}

//
// Returns the xdg_toplevel wl_resource for `handle`, or NULL if no
// matching toplevel is alive. Used by meek_shell_v1's
// close_toplevel handler so it can call xdg_toplevel_send_close
// on the right resource.
//
struct wl_resource* xdg_shell__find_xdg_toplevel_by_handle(uint32_t handle)
{
    if (handle == 0) { return NULL; }
    struct _xdg_shell_internal__xdg_surface* x;
    wl_list_for_each(x, &_xdg_shell_internal__xdg_surfaces, link)
    {
        if (x->role == _XDG_SURFACE_ROLE_TOPLEVEL &&
            x->toplevel != NULL &&
            x->toplevel->handle == handle)
        {
            return x->toplevel->resource;
        }
    }
    return NULL;
}

uint32_t xdg_shell__get_toplevel_handle_for_surface(struct wl_resource* wl_surface_resource)
{
    if (wl_surface_resource == NULL) return 0;

    //
    //We don't maintain a wl_surface -> xdg_toplevel index (small
    //surface count + per-frame lookup = linear search is fine).
    //Walk the xdg_surface list, match by wl_surface pointer, and
    //if found + has a toplevel role, return the toplevel's handle.
    //
    struct _xdg_shell_internal__xdg_surface* x;
    wl_list_for_each(x, &_xdg_shell_internal__xdg_surfaces, link)
    {
        if (x->wl_surface == wl_surface_resource &&
            x->role == _XDG_SURFACE_ROLE_TOPLEVEL &&
            x->toplevel != NULL)
        {
            return x->toplevel->handle;
        }
    }
    return 0;
}

void xdg_shell__foreach_toplevel_for_replay(
    void (*cb)(uint32_t handle, const char* app_id, const char* title, void* userdata),
    void* userdata)
{
    if (cb == NULL) return;

    struct _xdg_shell_internal__xdg_surface* x;
    wl_list_for_each(x, &_xdg_shell_internal__xdg_surfaces, link)
    {
        if (x->role == _XDG_SURFACE_ROLE_TOPLEVEL &&
            x->toplevel != NULL &&
            x->toplevel->handle != 0)
        {
            cb(x->toplevel->handle,
               x->toplevel->app_id,
               x->toplevel->title,
               userdata);
        }
    }
}

void xdg_shell__demote_client_toplevels(struct wl_client* shell_client)
{
    if (shell_client == NULL) return;

    struct _xdg_shell_internal__xdg_surface* x;
    wl_list_for_each(x, &_xdg_shell_internal__xdg_surfaces, link)
    {
        if (x->role == _XDG_SURFACE_ROLE_TOPLEVEL &&
            x->toplevel != NULL &&
            x->toplevel->handle != 0 &&
            wl_resource_get_client(x->toplevel->resource) == shell_client)
        {
            //
            //Clear the handle. All forwarding paths check `handle != 0`
            //before firing, so this single zero reliably suppresses
            //future toplevel_added/title_changed/buffer/removed events
            //AND the replay pass for this toplevel. No other
            //cleanup needed.
            //
            log_trace("xdg_shell: demoting toplevel %p (handle %u -> 0, owner is shell)",
                      (void*)x->toplevel, x->toplevel->handle);
            x->toplevel->handle = 0;
        }
    }
}

//
// Helper: find the _xdg_shell_internal__xdg_surface* for a given
// wl_surface resource. Linear walk of our list. Returns NULL if
// the wl_surface has no xdg role.
//
static struct _xdg_shell_internal__xdg_surface* _xdg_shell_internal__find_xs_for_wl_surface(struct wl_resource* wl_surface_resource)
{
    if (wl_surface_resource == NULL) { return NULL; }
    struct _xdg_shell_internal__xdg_surface* x;
    wl_list_for_each(x, &_xdg_shell_internal__xdg_surfaces, link)
    {
        if (x->wl_surface == wl_surface_resource) { return x; }
    }
    return NULL;
}

int xdg_shell__get_configure_size_for_surface(struct wl_resource* wl_surface_resource, int* w_out, int* h_out)
{
    if (w_out == NULL || h_out == NULL) { return 0; }
    struct _xdg_shell_internal__xdg_surface* xs = _xdg_shell_internal__find_xs_for_wl_surface(wl_surface_resource);
    if (xs == NULL || xs->role != _XDG_SURFACE_ROLE_TOPLEVEL || xs->toplevel == NULL)
    {
        return 0;
    }
    if (xs->toplevel->configured_w <= 0 || xs->toplevel->configured_h <= 0)
    {
        return 0;
    }
    *w_out = xs->toplevel->configured_w;
    *h_out = xs->toplevel->configured_h;
    return 1;
}

void xdg_shell__reconfigure_with_fractional_scale(struct wl_resource* wl_surface_resource)
{
    struct _xdg_shell_internal__xdg_surface* xs = _xdg_shell_internal__find_xs_for_wl_surface(wl_surface_resource);
    if (xs == NULL || xs->role != _XDG_SURFACE_ROLE_TOPLEVEL || xs->toplevel == NULL)
    {
        return;
    }
    struct wl_resource* t_res = xs->toplevel->resource;
    if (t_res == NULL) { return; }

    //
    // Shell client stays at panel-native (it renders chrome + app
    // tiles at panel resolution; scaling it would shrink everyone).
    //
    struct wl_client* cli = wl_resource_get_client(t_res);
    if (cli == NULL || cli == meek_shell_v1__get_shell_client())
    {
        return;
    }

    uint32_t wire_scale = fractional_scale__get_preferred_scale();
    if (wire_scale <= 120)   //1.0x -- no change from panel-native
    {
        return;
    }

    int panel_w = 0, panel_h = 0;
    (void)output_drm__get_native_size(&panel_w, &panel_h);
    if (panel_w <= 0 || panel_h <= 0) { return; }

    int logical_w = (int)((long)panel_w * 120 / (long)wire_scale);
    int logical_h = (int)((long)panel_h * 120 / (long)wire_scale);
    if (logical_w < 1) { logical_w = 1; }
    if (logical_h < 1) { logical_h = 1; }

    //
    // Only re-send if the logical size actually changed from the
    // last sent value. Avoids a re-configure storm when a client
    // creates multiple surfaces and each triggers get_fractional_scale.
    //
    if (xs->toplevel->configured_w == logical_w &&
        xs->toplevel->configured_h == logical_h)
    {
        return;
    }

    struct wl_array states;
    wl_array_init(&states);
    uint32_t* sp;
    sp = wl_array_add(&states, sizeof(uint32_t));
    if (sp != NULL) { *sp = XDG_TOPLEVEL_STATE_ACTIVATED; }
    sp = wl_array_add(&states, sizeof(uint32_t));
    if (sp != NULL) { *sp = XDG_TOPLEVEL_STATE_FULLSCREEN; }
    xdg_toplevel_send_configure(t_res, logical_w, logical_h, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(xs->resource, _xdg_shell_internal__next_serial++);
    xs->toplevel->configured_w = logical_w;
    xs->toplevel->configured_h = logical_h;

    log_info("xdg_shell: reconfigured toplevel handle=%u to %dx%d logical (panel=%dx%d, scale=%.2fx, serial=%u)",
             xs->toplevel->handle, logical_w, logical_h,
             panel_w, panel_h, wire_scale / 120.0,
             _xdg_shell_internal__next_serial - 1);
}

void xdg_shell__record_buffer_size(struct wl_resource* wl_surface_resource, int buffer_w, int buffer_h)
{
    if (buffer_w <= 0 || buffer_h <= 0) { return; }
    struct _xdg_shell_internal__xdg_surface* xs = _xdg_shell_internal__find_xs_for_wl_surface(wl_surface_resource);
    if (xs == NULL || xs->role != _XDG_SURFACE_ROLE_TOPLEVEL || xs->toplevel == NULL)
    {
        return;
    }
    xs->toplevel->last_buffer_w = buffer_w;
    xs->toplevel->last_buffer_h = buffer_h;
}

int xdg_shell__translate_tap_coords_for_surface(struct wl_resource* wl_surface_resource, int32_t sx_in, int32_t sy_in, int32_t* out_sx, int32_t* out_sy)
{
    if (out_sx == NULL || out_sy == NULL) { return 0; }
    *out_sx = sx_in;
    *out_sy = sy_in;

    struct _xdg_shell_internal__xdg_surface* xs = _xdg_shell_internal__find_xs_for_wl_surface(wl_surface_resource);
    if (xs == NULL || xs->role != _XDG_SURFACE_ROLE_TOPLEVEL || xs->toplevel == NULL)
    {
        return 0;
    }
    int lw = xs->toplevel->configured_w;
    int lh = xs->toplevel->configured_h;
    int bw = xs->toplevel->last_buffer_w;
    int bh = xs->toplevel->last_buffer_h;
    if (lw <= 0 || lh <= 0 || bw <= 0 || bh <= 0)
    {
        //
        // Missing either side of the ratio. Pass coords through;
        // better to route to a slightly-wrong pixel than to drop
        // the touch entirely.
        //
        return 0;
    }
    if (lw == bw && lh == bh)
    {
        //
        // No fractional scale in effect (logical == buffer). Shell's
        // coords are already surface-local.
        //
        return 1;
    }
    //
    // Shell sends sx_buffer = tap_widget * buffer / widget.
    // We want  sx_logical = tap_widget * logical / widget.
    // Ratio:    sx_logical = sx_buffer * logical / buffer.
    //
    *out_sx = (int32_t)((long)sx_in * (long)lw / (long)bw);
    *out_sy = (int32_t)((long)sy_in * (long)lh / (long)bh);
    return 1;
}
