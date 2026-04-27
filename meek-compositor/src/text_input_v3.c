//
// text_input_v3.c -- stub implementation of zwp_text_input_manager_v3.
//
// GOAL (this pass): advertise the global so clients that check for
// it (foot, gtk, qt, etc.) stop emitting "text input interface not
// implemented by compositor; IME will be disabled" warnings at
// startup. Every request is a no-op; we never emit enter / leave /
// preedit / commit_string. So IMEs don't actually work yet --
// clients that bind still get their preferred text-entry widget
// path, just without server-side IME assistance.
//
// FUTURE (layer 2): wire commit / enable / disable events through
// meek_shell_v1 to meek-shell so widget_keyboard can act as the
// input-method. See crucial_fixes.md entry #N (TBD) once wired.
//
// Design mirrors xdg_shell.c and meek_shell_v1.c: one bind function
// on the global, per-resource vtables of request handlers, minimal
// per-text_input state struct tracked via wl_list.
//

#include <stdlib.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "text-input-unstable-v3-protocol.h"

#include "types.h"
#include "third_party/log.h"
#include "meek_shell_v1.h"
#include "text_input_v3.h"

//
// Most-recently-enabled text_input, for forwarding IME events from
// the shell back to the app. Cleared when the resource is destroyed
// or the underlying client disables / goes away. Single-slot is
// correct for the phone use case (one focused app at a time).
//
static struct wl_resource* _text_input_v3_internal__active = NULL;
static uint32_t            _text_input_v3_internal__serial = 0;

//---------------------------------------------------------------------
// zwp_text_input_v3 resource -- per-client-per-seat text-input object.
//---------------------------------------------------------------------

static void _text_input_v3_internal__ti_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _text_input_v3_internal__ti_enable(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    //
    // Remember this text_input as the active one. Next shell-side
    // ime_commit_string will route here. Also tell the shell to
    // show its on-screen keyboard via meek_shell_v1.ime_request_on.
    //
    _text_input_v3_internal__active = r;
    //
    // We currently have no cross-map from text_input's wl_client to
    // a meek_shell_v1 toplevel handle, so pass app_handle=0 for now.
    // meek-shell treats 0 as "whatever app is focused". Future work:
    // track wl_surface focus in text_input.enter/leave and plumb
    // the matching xdg_shell handle through here.
    //
    meek_shell_v1__fire_ime_request_on(/*app_handle*/ 0);
    log_info("zwp_text_input_v3.enable -> ime_request_on fired");
}

static void _text_input_v3_internal__ti_disable(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    if (_text_input_v3_internal__active == r)
    {
        _text_input_v3_internal__active = NULL;
    }
    meek_shell_v1__fire_ime_request_off(0);
    log_info("zwp_text_input_v3.disable -> ime_request_off fired");
}

static void _text_input_v3_internal__ti_set_surrounding_text(
    struct wl_client* c, struct wl_resource* r,
    const char* text, int32_t cursor, int32_t anchor)
{
    (void)c; (void)r; (void)text; (void)cursor; (void)anchor;
}

static void _text_input_v3_internal__ti_set_text_change_cause(
    struct wl_client* c, struct wl_resource* r, uint32_t cause)
{
    (void)c; (void)r; (void)cause;
}

static void _text_input_v3_internal__ti_set_content_type(
    struct wl_client* c, struct wl_resource* r, uint32_t hint, uint32_t purpose)
{
    (void)c; (void)r; (void)hint; (void)purpose;
}

static void _text_input_v3_internal__ti_set_cursor_rectangle(
    struct wl_client* c, struct wl_resource* r,
    int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static void _text_input_v3_internal__ti_commit(struct wl_client* c, struct wl_resource* r)
{
    (void)c; (void)r;
    //
    // Layer 2 will emit a signal to meek-shell here so its
    // widget_keyboard can open. For now we log + no-op so foot
    // doesn't stall waiting for a response on commit.
    //
    log_trace("zwp_text_input_v3.commit (stub no-op; layer 2 will wire to meek-shell)");
}

static const struct zwp_text_input_v3_interface _text_input_v3_internal__ti_impl = {
    .destroy                = _text_input_v3_internal__ti_destroy,
    .enable                 = _text_input_v3_internal__ti_enable,
    .disable                = _text_input_v3_internal__ti_disable,
    .set_surrounding_text   = _text_input_v3_internal__ti_set_surrounding_text,
    .set_text_change_cause  = _text_input_v3_internal__ti_set_text_change_cause,
    .set_content_type       = _text_input_v3_internal__ti_set_content_type,
    .set_cursor_rectangle   = _text_input_v3_internal__ti_set_cursor_rectangle,
    .commit                 = _text_input_v3_internal__ti_commit,
};

//---------------------------------------------------------------------
// zwp_text_input_manager_v3 -- one per client; factory for text_input.
//---------------------------------------------------------------------

static void _text_input_v3_internal__mgr_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _text_input_v3_internal__mgr_get_text_input(
    struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* seat)
{
    (void)r; (void)seat;
    //
    // Version of the manager determines version of the child
    // text_input (spec section "object versioning"). v1 of the
    // protocol is all we currently advertise.
    //
    uint32_t version = wl_resource_get_version(r);
    struct wl_resource* ti =
        wl_resource_create(c, &zwp_text_input_v3_interface, version, id);
    if (ti == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(ti, &_text_input_v3_internal__ti_impl, NULL, NULL);
    log_trace("zwp_text_input_manager_v3.get_text_input id=%u (stub text_input created)", id);
}

static const struct zwp_text_input_manager_v3_interface _text_input_v3_internal__mgr_impl = {
    .destroy        = _text_input_v3_internal__mgr_destroy,
    .get_text_input = _text_input_v3_internal__mgr_get_text_input,
};

static void _text_input_v3_internal__mgr_bind(
    struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* r = wl_resource_create(
        client, &zwp_text_input_manager_v3_interface, version, id);
    if (r == NULL)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &_text_input_v3_internal__mgr_impl, NULL, NULL);
    log_trace("zwp_text_input_manager_v3 bind id=%u v=%u (stub)", id, version);
}

//---------------------------------------------------------------------
// public
//---------------------------------------------------------------------

void text_input_v3__forward_commit_string(const char* text)
{
    if (_text_input_v3_internal__active == NULL)
    {
        log_trace("text_input_v3: commit_string dropped (no active text_input)");
        return;
    }
    if (text == NULL) { text = ""; }
    zwp_text_input_v3_send_commit_string(_text_input_v3_internal__active, text);
    //
    // commit_string alone isn't enough -- spec requires a matching
    // `done(serial)` to flush the pending edit batch. Serial is
    // per-text_input; we keep a module-wide counter which is fine
    // for single-active-client. Increments on every done().
    //
    _text_input_v3_internal__serial++;
    zwp_text_input_v3_send_done(_text_input_v3_internal__active,
                                _text_input_v3_internal__serial);
    log_info("text_input_v3: forwarded commit_string='%s' serial=%u",
             text, _text_input_v3_internal__serial);
}

void text_input_v3__register(struct wl_display* display)
{
    struct wl_global* g = wl_global_create(
        display,
        &zwp_text_input_manager_v3_interface,
        /*version*/ 1,
        /*data*/    NULL,
        _text_input_v3_internal__mgr_bind);
    if (g == NULL)
    {
        log_fatal("wl_global_create(zwp_text_input_manager_v3) failed");
        return;
    }
    log_info("text_input_v3 registered (stub; IME request paths accepted but no-ops)");
}
