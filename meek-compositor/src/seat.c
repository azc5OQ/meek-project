//
//seat.c -- wl_seat + wl_touch + wl_pointer + wl_keyboard subsystem.
//
//Moved out of globals.c. Owns the touches list, the keyboards list,
//broadcast helpers (for raw panel touches), and directed route
//helpers (for Phase 6 tap delivery via meek_shell_v1.route_touch_*
//and for Latin-input delivery via meek_shell_v1.route_keyboard_*).
//
//The wl_seat advertises TOUCH + KEYBOARD capabilities. Pointer
//resources are created as no-op stubs for clients that ignore the
//announced caps and call get_pointer anyway.
//
//KEYBOARD PATH (why this module owns the keymap, not the shell):
//--------------------------------------------------------------
//Standard Wayland hands every client that binds wl_keyboard a
//keymap the compositor built via libxkbcommon
//(xkb_keymap_new_from_names). Clients mmap the file descriptor
//the compositor sends and use their OWN xkbcommon instance to
//translate incoming wl_keyboard.key evdev keycodes into unicode
//codepoints / keysyms per their configured layout.
//
//In the three-project split, meek-shell decides WHICH app receives
//a given keystroke (policy) and meek-compositor performs the
//wl_keyboard.enter/key/leave dispatch (mechanism). The shell sends
//keycode + state via meek_shell_v1.route_keyboard_key; this module
//resolves the target client, synthesises enter/leave on focus
//change, and emits wl_keyboard.key.
//
//The keymap itself is built once at seat__init and served to every
//wl_keyboard binder via a freshly-allocated memfd. xkbcommon
//computes a full text keymap (~50 KB for us QWERTY); we mmap it
//into the memfd once and re-send the same bytes to every binder.
//
//This design matches the standard Wayland compositor pattern: the
//compositor is the source of truth for the keymap apps receive.
//What differs in meek is that the shell, not the compositor,
//decides whose keyboard focus is active; see DESIGN.md § "Input
//routing".
//

//
//memfd_create + MFD_CLOEXEC are Linux extensions. On glibc they
//require _GNU_SOURCE; on musl they live in <sys/mman.h> under the
//same gate. Must be defined BEFORE any libc include.
//
#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <xkbcommon/xkbcommon.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>

#include "types.h"
#include "third_party/log.h"
#include "clib/memory_manager.h"

#include "globals.h"
#include "seat.h"
#include "surface.h"
#include "meek_shell_v1.h"

//
//Module-private state. display pointer is held so touch/keyboard
//dispatch can mint serials via wl_display_next_serial; the touches
//+ keyboards lists track each live wl_touch / wl_keyboard resource
//wrapped for list membership. xkb keymap is built once at seat__init
//and shared with every wl_keyboard binder.
//
static struct wl_display*   _seat_internal__display        = NULL;
static struct wl_list       _seat_internal__touches;
static struct wl_list       _seat_internal__keyboards;

static struct xkb_context*  _seat_internal__xkb_ctx        = NULL;
static struct xkb_keymap*   _seat_internal__xkb_keymap     = NULL;
static char*                _seat_internal__keymap_string  = NULL;
static size_t               _seat_internal__keymap_size    = 0;

//
//Wrapper for each wl_touch resource (wl_resource has no public link).
//
struct _seat_internal__touch
{
    struct wl_resource* resource;
    struct wl_listener  destroy_listener;
    struct wl_list      link;
};

//
//Wrapper for each wl_keyboard resource. focused_surface tracks
//which wl_surface this keyboard resource is currently delivering
//events to. NULL until the first route_keyboard_key_to_client call
//for this client, at which point we synthesize wl_keyboard.enter
//for the target surface. Changing focus synthesizes leave(old) +
//enter(new). See globals__seat__route_keyboard_key_to_client.
//
struct _seat_internal__keyboard
{
    struct wl_resource* resource;
    struct wl_listener  destroy_listener;
    struct wl_list      link;
    struct wl_resource* focused_surface;
};

//-- forward decls --
static void _seat_internal__seat__on_get_pointer(struct wl_client* c, struct wl_resource* r, uint32_t id);
static void _seat_internal__seat__on_get_keyboard(struct wl_client* c, struct wl_resource* r, uint32_t id);
static void _seat_internal__seat__on_get_touch(struct wl_client* c, struct wl_resource* r, uint32_t id);
static void _seat_internal__seat__on_release(struct wl_client* c, struct wl_resource* r);
static void _seat_internal__seat__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id);
static void _seat_internal__on_touch_resource_destroy(struct wl_listener* l, void* data);
static void _seat_internal__on_keyboard_resource_destroy(struct wl_listener* l, void* data);
static int  _seat_internal__build_keymap(void);
static int  _seat_internal__send_keymap_to(struct wl_resource* k);

// ============================================================
// wl_pointer / wl_keyboard / wl_touch stubs
// ============================================================

static void _seat_internal__pointer__on_set_cursor(struct wl_client* c, struct wl_resource* r, uint32_t serial, struct wl_resource* surface, int32_t hotspot_x, int32_t hotspot_y)
{
    (void)c; (void)r; (void)serial; (void)surface; (void)hotspot_x; (void)hotspot_y;
}
static void _seat_internal__pointer__on_release(struct wl_client* c, struct wl_resource* r)
{ (void)c; wl_resource_destroy(r); }
static const struct wl_pointer_interface _seat_internal__pointer_impl = {
    .set_cursor = _seat_internal__pointer__on_set_cursor,
    .release    = _seat_internal__pointer__on_release,
};

static void _seat_internal__keyboard__on_release(struct wl_client* c, struct wl_resource* r)
{ (void)c; wl_resource_destroy(r); }
static const struct wl_keyboard_interface _seat_internal__keyboard_impl = {
    .release = _seat_internal__keyboard__on_release,
};

static void _seat_internal__touch__on_release(struct wl_client* c, struct wl_resource* r)
{ (void)c; wl_resource_destroy(r); }
static const struct wl_touch_interface _seat_internal__touch_impl = {
    .release = _seat_internal__touch__on_release,
};

// ============================================================
// wl_seat impl
// ============================================================

static const struct wl_seat_interface _seat_internal__seat_impl = {
    .get_pointer  = _seat_internal__seat__on_get_pointer,
    .get_keyboard = _seat_internal__seat__on_get_keyboard,
    .get_touch    = _seat_internal__seat__on_get_touch,
    .release      = _seat_internal__seat__on_release,
};

//
// wl_pointer factory. Stub resource only: we advertise the POINTER
// seat capability so GTK4 / Qt initialise their click recognisers
// against touch input (they gate on POINTER cap even when they never
// call get_pointer). We never actually emit wl_pointer.* events --
// the standard pattern is to dispatch only wl_touch for touch input
// and let the toolkit's gesture recognisers convert touch-down/up to
// click internally. Synthesising wl_pointer from touch in parallel
// with wl_touch confuses GTK4's GtkGestureClick (two input devices
// appear to fire simultaneously), so we don't either.
//
static void _seat_internal__seat__on_get_pointer(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    uint32_t version = wl_resource_get_version(r);
    struct wl_resource* p = wl_resource_create(c, &wl_pointer_interface, version, id);
    if (p == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(p, &_seat_internal__pointer_impl, NULL, NULL);
    log_trace("wl_seat.get_pointer id=%u (stub, no events dispatched)", id);
}

//
//wl_keyboard factory. Unlike the pointer stub, this one actually
//tracks the resource + sends the keymap. The per-client wl_keyboard
//is what globals__seat__route_keyboard_* dispatches on when the
//shell forwards a keystroke via meek_shell_v1.route_keyboard_key.
//
//Keymap delivery is one-shot: we build the xkb keymap at
//seat__init and, on every get_keyboard, write it to a fresh memfd
//and send via wl_keyboard.keymap(XKB_V1, fd, size). libwayland
//dup()s the fd internally so we close our copy immediately after
//(same fd-close rule as crucial_fixes #7). Client mmaps the fd and
//parses with its own libxkbcommon.
//
static void _seat_internal__seat__on_get_keyboard(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    uint32_t version = wl_resource_get_version(r);
    struct wl_resource* k = wl_resource_create(c, &wl_keyboard_interface, version, id);
    if (k == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(k, &_seat_internal__keyboard_impl, NULL, NULL);

    //
    //Track the resource so route_keyboard_* can iterate it later.
    //destroy_listener removes the wrapper from the list when the
    //client releases / disconnects.
    //
    struct _seat_internal__keyboard* kw = GUI_CALLOC_T(1, sizeof(*kw), MM_TYPE_NODE);
    if (kw == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    kw->resource = k;
    kw->focused_surface = NULL;
    kw->destroy_listener.notify = _seat_internal__on_keyboard_resource_destroy;
    wl_resource_add_destroy_listener(k, &kw->destroy_listener);
    wl_list_insert(&_seat_internal__keyboards, &kw->link);

    //
    //Send the keymap. Failure here is logged but non-fatal -- the
    //client still gets a wl_keyboard resource; it just won't know
    //how to decode keycodes. Most real clients will bail or run
    //with a default keymap in that case.
    //
    if (_seat_internal__send_keymap_to(k) != 0)
    {
        log_warn("wl_seat.get_keyboard id=%u: keymap send failed; client will receive raw keycodes only", id);
    }

    //
    //repeat_info (introduced in wl_keyboard v4). Not all clients
    //honor auto-repeat; those that do will repeat held keys at the
    //declared rate. 25 keys/sec after 600ms hold matches the standard
    //compositor default.
    //
    if (version >= 4)
    {
        wl_keyboard_send_repeat_info(k, 25, 600);
    }

    log_trace("wl_seat.get_keyboard id=%u v=%u (tracked, keymap sent)", id, version);
}

static void _seat_internal__on_touch_resource_destroy(struct wl_listener* l, void* data)
{
    (void)data;
    struct _seat_internal__touch* tw =
        wl_container_of(l, tw, destroy_listener);
    wl_list_remove(&tw->link);
    log_trace("wl_touch wrapper %p removed from list", (void*)tw);
    GUI_FREE(tw);
}

static void _seat_internal__on_keyboard_resource_destroy(struct wl_listener* l, void* data)
{
    (void)data;
    struct _seat_internal__keyboard* kw =
        wl_container_of(l, kw, destroy_listener);
    wl_list_remove(&kw->link);
    log_trace("wl_keyboard wrapper %p removed from list", (void*)kw);
    GUI_FREE(kw);
}

static void _seat_internal__seat__on_get_touch(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    uint32_t version = wl_resource_get_version(r);
    struct wl_resource* t = wl_resource_create(c, &wl_touch_interface, version, id);
    if (t == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(t, &_seat_internal__touch_impl, NULL, NULL);

    struct _seat_internal__touch* tw = GUI_CALLOC_T(1, sizeof(*tw), MM_TYPE_NODE);
    if (tw == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    tw->resource = t;
    tw->destroy_listener.notify = _seat_internal__on_touch_resource_destroy;
    wl_resource_add_destroy_listener(t, &tw->destroy_listener);
    wl_list_insert(&_seat_internal__touches, &tw->link);

    log_trace("wl_seat.get_touch id=%u -> tracked wrapper %p", id, (void*)tw);
}

static void _seat_internal__seat__on_release(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _seat_internal__seat__bind(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* r = wl_resource_create(client, &wl_seat_interface, version, id);
    if (r == NULL)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &_seat_internal__seat_impl, NULL, NULL);

    //
    //Advertise POINTER | TOUCH | KEYBOARD. Pointer advertising is
    //needed despite the phone having no physical pointer: GTK4 /
    //libadwaita gate their touch-gesture-recogniser init on
    //wl_seat's POINTER capability being present. Without POINTER
    //announced, gnome-calculator et al bind wl_touch but never
    //route incoming wl_touch.down through their click-gesture
    //controller, so button taps don't fire on_click.
    //
    //The wl_pointer we create via get_pointer is a stub: we never
    //send pointer.motion or pointer.button events (no hardware). But
    //GTK's GestureClick uses wl_touch events internally once the
    //seat's pointer slot is populated, which is what we need.
    //
    //See session/bugs_to_investigate.md entry #2 for the diagnosis
    //flow that landed this.
    //
    wl_seat_send_capabilities(r,
        WL_SEAT_CAPABILITY_POINTER |
        WL_SEAT_CAPABILITY_TOUCH   |
        WL_SEAT_CAPABILITY_KEYBOARD);
    if (version >= WL_SEAT_NAME_SINCE_VERSION)
    {
        wl_seat_send_name(r, "seat0");
    }
    log_trace("wl_seat bind id=%u v=%u (caps: pointer|touch|keyboard)", id, version);
}

// ============================================================
// xkb keymap init + per-resource serve
// ============================================================

//
//Build the US QWERTY keymap once at seat__init. xkbcommon compiles
//the keymap from RMLVO (rules/model/layout/variant/options) into a
//text form that clients can re-parse on their side.
//
//"evdev" rules + "pc105" model + "us" layout is the canonical
//"default keyboard" choice, matches the standard compositor
//defaults, and works with foot / gtk / qt out of the box. If we
//later want to honor $XKB_DEFAULT_LAYOUT or a shell-configured
//layout, swap names.layout here.
//
static int _seat_internal__build_keymap(void)
{
    _seat_internal__xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (_seat_internal__xkb_ctx == NULL)
    {
        log_error("xkb_context_new failed");
        return -1;
    }

    struct xkb_rule_names names;
    names.rules   = "evdev";
    names.model   = "pc105";
    names.layout  = "us";
    names.variant = NULL;
    names.options = NULL;

    _seat_internal__xkb_keymap = xkb_keymap_new_from_names(
        _seat_internal__xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (_seat_internal__xkb_keymap == NULL)
    {
        log_error("xkb_keymap_new_from_names failed (rules=evdev model=pc105 layout=us)");
        xkb_context_unref(_seat_internal__xkb_ctx);
        _seat_internal__xkb_ctx = NULL;
        return -1;
    }

    _seat_internal__keymap_string =
        xkb_keymap_get_as_string(_seat_internal__xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (_seat_internal__keymap_string == NULL)
    {
        log_error("xkb_keymap_get_as_string failed");
        xkb_keymap_unref(_seat_internal__xkb_keymap);
        xkb_context_unref(_seat_internal__xkb_ctx);
        _seat_internal__xkb_keymap = NULL;
        _seat_internal__xkb_ctx    = NULL;
        return -1;
    }
    _seat_internal__keymap_size = strlen(_seat_internal__keymap_string) + 1;

    log_info("seat: xkb keymap built (us qwerty, %zu bytes)",
             _seat_internal__keymap_size);
    return 0;
}

//
//Send the cached keymap to one wl_keyboard resource. memfd_create
//gives us an anonymous file living entirely in RAM; we size it,
//mmap it write-enabled, memcpy the keymap bytes in, munmap, then
//send. libwayland dup()s the fd into its SCM_RIGHTS batch; we close
//our copy immediately after (same rule as crucial_fixes #7 for
//toplevel_buffer).
//
//Using a fresh memfd per client (instead of one shared memfd held
//open forever) is the standard pattern -- keeps the keymap bytes in
//RAM for exactly as long as the client needs to mmap + parse them,
//then both sides can drop the fd.
//
static int _seat_internal__send_keymap_to(struct wl_resource* k)
{
    if (_seat_internal__keymap_string == NULL ||
        _seat_internal__keymap_size   == 0)
    {
        log_warn("seat: keymap not built yet; skipping send");
        return -1;
    }

    //
    //MFD_CLOEXEC: the fd won't leak across an exec; not strictly
    //needed here (we close it immediately) but defensive. No
    //MFD_ALLOW_SEALING -- clients don't need to seal our keymap.
    //
    int fd = memfd_create("meek-xkb-keymap", MFD_CLOEXEC);
    if (fd < 0)
    {
        log_error("memfd_create(meek-xkb-keymap) failed: %s", strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)_seat_internal__keymap_size) < 0)
    {
        log_error("ftruncate(%zu) failed: %s",
                  _seat_internal__keymap_size, strerror(errno));
        close(fd);
        return -1;
    }
    void* mem = mmap(NULL, _seat_internal__keymap_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        log_error("mmap keymap failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    memcpy(mem, _seat_internal__keymap_string, _seat_internal__keymap_size);
    munmap(mem, _seat_internal__keymap_size);

    wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                            fd, (uint32_t)_seat_internal__keymap_size);

    //
    //Flush the owning client so libwayland sends the keymap event
    //(with its internal dup of fd) BEFORE we close below. Without
    //flush, libwayland may still be holding the event in its outbound
    //queue when close() runs, which would SCM_RIGHTS a dead fd.
    //
    struct wl_client* c = wl_resource_get_client(k);
    if (c != NULL) { wl_client_flush(c); }
    close(fd);
    return 0;
}

// ============================================================
// Public entry points (seat.h)
// ============================================================

void seat__init(void)
{
    wl_list_init(&_seat_internal__touches);
    wl_list_init(&_seat_internal__keyboards);

    //
    //Build the xkb keymap once here so every subsequent get_keyboard
    //binder can serve the same bytes via memfd. Non-fatal on
    //failure -- seat still serves touch; clients that try
    //get_keyboard will receive a resource but no keymap, and most
    //will fall back to built-in defaults or log a warning.
    //
    if (_seat_internal__build_keymap() != 0)
    {
        log_warn("seat: running without a keymap; wl_keyboard clients will be starved");
    }
}

void seat__register(struct wl_display* display)
{
    _seat_internal__display = display;

    struct wl_global* g = wl_global_create(display,
                                           &wl_seat_interface,
                                           /*version*/ 7,
                                           /*data*/ NULL,
                                           _seat_internal__seat__bind);
    if (g == NULL)
    {
        log_fatal("wl_global_create(wl_seat) failed");
    }
}

// ============================================================
// Broadcast dispatch (globals.h API)
// ============================================================

//
//Shell-only filter. Real panel touches only reach the shell; other
//apps receive input exclusively via the directed route_touch_*
//variants below. See crucial_fixes.md entry #2.
//
static int _seat_internal__is_shell_touch(struct wl_resource* touch_resource)
{
    struct wl_client* owner = wl_resource_get_client(touch_resource);
    struct wl_client* shell = meek_shell_v1__get_shell_client();
    return (shell != NULL && owner == shell);
}

void globals__seat__send_touch_down(uint32_t time, int32_t id, int32_t x, int32_t y)
{
    if (_seat_internal__display == NULL) { return; }
    uint32_t serial = wl_display_next_serial(_seat_internal__display);

    struct _seat_internal__touch* tw;
    wl_list_for_each(tw, &_seat_internal__touches, link)
    {
        if (!_seat_internal__is_shell_touch(tw->resource)) { continue; }
        struct wl_client* c = wl_resource_get_client(tw->resource);
        struct wl_resource* surface = surface__first_resource_for_client(c);
        if (surface == NULL) { continue; }
        wl_touch_send_down(tw->resource, serial, time, surface, id,
                           wl_fixed_from_int(x), wl_fixed_from_int(y));
    }
}

void globals__seat__send_touch_up(uint32_t time, int32_t id)
{
    if (_seat_internal__display == NULL) { return; }
    uint32_t serial = wl_display_next_serial(_seat_internal__display);

    struct _seat_internal__touch* tw;
    wl_list_for_each(tw, &_seat_internal__touches, link)
    {
        if (!_seat_internal__is_shell_touch(tw->resource)) { continue; }
        wl_touch_send_up(tw->resource, serial, time, id);
    }
}

void globals__seat__send_touch_motion(uint32_t time, int32_t id, int32_t x, int32_t y)
{
    struct _seat_internal__touch* tw;
    wl_list_for_each(tw, &_seat_internal__touches, link)
    {
        if (!_seat_internal__is_shell_touch(tw->resource)) { continue; }
        wl_touch_send_motion(tw->resource, time, id,
                             wl_fixed_from_int(x), wl_fixed_from_int(y));
    }
}

void globals__seat__send_touch_frame(void)
{
    struct _seat_internal__touch* tw;
    wl_list_for_each(tw, &_seat_internal__touches, link)
    {
        if (!_seat_internal__is_shell_touch(tw->resource)) { continue; }
        wl_touch_send_frame(tw->resource);
    }
}

void globals__seat__send_touch_cancel(void)
{
    struct _seat_internal__touch* tw;
    wl_list_for_each(tw, &_seat_internal__touches, link)
    {
        if (!_seat_internal__is_shell_touch(tw->resource)) { continue; }
        wl_touch_send_cancel(tw->resource);
    }
}

// ============================================================
// Directed routing (Phase 6)
// ============================================================

//
// Dispatch wl_touch events ONLY. The standard pattern is: touch
// input is touch input; the client's toolkit (GTK4's
// GtkGestureClick, Qt's QEventPoint path, libadwaita's gesture
// controllers) recognises wl_touch.down+up as a click natively.
// Synthesising parallel wl_pointer events from touches -- which we
// tried earlier and abandoned -- made GTK4 see two simultaneous
// input devices and broke click recognition for gnome-calculator.
//
// The wl_seat advertises POINTER capability so toolkits initialise
// their pointer-gated gesture recognisers (GTK4 gates this on
// POINTER being announced, even for touch-only hardware); the
// get_pointer stub creates a resource but we never emit events.
//
// Frame ordering: per-event wl_touch.frame. The shell forwards one
// route_touch_* call per user action (down OR up, never batched),
// so each call's frame IS the end-of-batch from the client's
// perspective.
//
void globals__seat__route_touch_down_to_client(
    struct wl_client* client, struct wl_resource* surface,
    uint32_t time, int32_t id, int32_t sx, int32_t sy)
{
    if (_seat_internal__display == NULL) { return; }
    if (client == NULL || surface == NULL) { return; }

    uint32_t serial = wl_display_next_serial(_seat_internal__display);
    int sent = 0;
    struct _seat_internal__touch* tw;
    wl_list_for_each(tw, &_seat_internal__touches, link)
    {
        if (wl_resource_get_client(tw->resource) != client) { continue; }
        wl_touch_send_down(tw->resource, serial, time, surface, id,
                           wl_fixed_from_int(sx), wl_fixed_from_int(sy));
        wl_touch_send_frame(tw->resource);
        sent++;
    }
    wl_client_flush(client);
    log_trace("globals: route_touch_down client=%p id=%d (%d,%d) -> %d wl_touch(es)",
              (void*)client, id, sx, sy, sent);
}

void globals__seat__route_touch_motion_to_client(
    struct wl_client* client, uint32_t time, int32_t id, int32_t sx, int32_t sy)
{
    if (client == NULL) { return; }

    int sent = 0;
    struct _seat_internal__touch* tw;
    wl_list_for_each(tw, &_seat_internal__touches, link)
    {
        if (wl_resource_get_client(tw->resource) != client) { continue; }
        wl_touch_send_motion(tw->resource, time, id,
                             wl_fixed_from_int(sx), wl_fixed_from_int(sy));
        wl_touch_send_frame(tw->resource);
        sent++;
    }
    wl_client_flush(client);
    log_trace("globals: route_touch_motion client=%p id=%d (%d,%d) -> %d wl_touch(es)",
              (void*)client, id, sx, sy, sent);
}

void globals__seat__route_touch_up_to_client(
    struct wl_client* client, uint32_t time, int32_t id)
{
    if (_seat_internal__display == NULL) { return; }
    if (client == NULL) { return; }

    uint32_t serial = wl_display_next_serial(_seat_internal__display);
    int sent = 0;
    struct _seat_internal__touch* tw;
    wl_list_for_each(tw, &_seat_internal__touches, link)
    {
        if (wl_resource_get_client(tw->resource) != client) { continue; }
        wl_touch_send_up(tw->resource, serial, time, id);
        wl_touch_send_frame(tw->resource);
        sent++;
    }
    wl_client_flush(client);
    log_trace("globals: route_touch_up client=%p id=%d -> %d wl_touch(es)",
              (void*)client, id, sent);
}

// ============================================================
// Directed keyboard routing (Latin input via widget_keyboard)
// ============================================================

//
//Send a wl_keyboard.key event to every wl_keyboard resource owned
//by `client`. Synthesizes wl_keyboard.enter for `surface` on first
//call (or on focus change). The enter carries an empty `keys` array
//because we don't simulate "held-at-enter" keys -- every keystroke
//the shell forwards has a fresh press/release pair.
//
//`keycode` is a Linux evdev keycode (e.g. KEY_A == 30) as defined
//in <linux/input-event-codes.h>. This is NOT the xkbcommon keysym
//-- the client does that translation via its own libxkbcommon
//instance using the keymap we sent at get_keyboard. The protocol
//carries evdev codes directly, matching how libinput delivers them
//on the server side of a real keyboard.
//
//`state` is 0 (released) or 1 (pressed), per wl_keyboard.key_state.
//
//ON FOCUS CHANGE we emit: leave(old_surface) → enter(new_surface,
//empty_keys) → modifiers(0,0,0,0). The explicit baseline modifiers
//event is important even when the state is all-zero: clients like
//foot run xkb_state_update_mask on every modifiers event to reset
//their xkb state to a known baseline. Without it, foot's first
//key after an enter translates with uninitialised modifier state
//and the character can land as a no-op (no echo, no error) --
//documented symptom in "keys deliver per compositor log, nothing
//shows in terminal."
//
//NOTE on wl_keyboard.frame: there is no such event. frame exists
//on wl_pointer (v5+) and wl_touch (v1+), NOT wl_keyboard. Each
//wl_keyboard event is processed atomically by the client on its
//own. Do not add wl_keyboard_send_frame calls -- the symbol
//doesn't exist in the scanner output.
//
void globals__seat__route_keyboard_key_to_client(
    struct wl_client* client, struct wl_resource* surface,
    uint32_t time_ms, uint32_t keycode, uint32_t state)
{
    if (_seat_internal__display == NULL) { return; }
    if (client == NULL || surface == NULL) { return; }

    int sent = 0;
    struct _seat_internal__keyboard* kw;
    wl_list_for_each(kw, &_seat_internal__keyboards, link)
    {
        if (wl_resource_get_client(kw->resource) != client) { continue; }

        //
        //Focus synth: if this keyboard isn't already pointed at
        //`surface`, send leave(old) + enter(new) + modifiers(0,...).
        //empty key array on enter means "no keys currently held",
        //which is the standard convention for virtual-keyboard-
        //driven enters. The baseline modifiers event is required
        //for foot (see block comment above).
        //
        if (kw->focused_surface != surface)
        {
            if (kw->focused_surface != NULL)
            {
                uint32_t s_leave = wl_display_next_serial(_seat_internal__display);
                wl_keyboard_send_leave(kw->resource, s_leave, kw->focused_surface);
            }
            struct wl_array keys;
            wl_array_init(&keys);
            uint32_t s_enter = wl_display_next_serial(_seat_internal__display);
            wl_keyboard_send_enter(kw->resource, s_enter, surface, &keys);
            wl_array_release(&keys);

            uint32_t s_mods = wl_display_next_serial(_seat_internal__display);
            wl_keyboard_send_modifiers(kw->resource, s_mods, 0, 0, 0, 0);

            kw->focused_surface = surface;
        }

        uint32_t s_key = wl_display_next_serial(_seat_internal__display);
        wl_keyboard_send_key(kw->resource, s_key, time_ms, keycode, state);
        sent++;
    }
    log_trace("globals: route_keyboard_key client=%p keycode=%u state=%u -> %d wl_keyboard(s)",
              (void*)client, keycode, state, sent);
}

//
//Send wl_keyboard.modifiers. Called before/after a shifted key so
//the client's xkbcommon state sees MOD_SHIFT depressed during the
//keypress translation.
//
//Bit layout matches the compositor's xkb state -- standard is:
//  shift   = 1 << 0
//  caps    = 1 << 1
//  ctrl    = 1 << 2
//  alt     = 1 << 3
//
//Clients translate these via xkb_state_update_mask on their side.
//
void globals__seat__route_keyboard_modifiers_to_client(
    struct wl_client* client,
    uint32_t mods_depressed, uint32_t mods_latched,
    uint32_t mods_locked,    uint32_t group)
{
    if (_seat_internal__display == NULL) { return; }
    if (client == NULL) { return; }

    int sent = 0;
    struct _seat_internal__keyboard* kw;
    wl_list_for_each(kw, &_seat_internal__keyboards, link)
    {
        if (wl_resource_get_client(kw->resource) != client) { continue; }
        uint32_t serial = wl_display_next_serial(_seat_internal__display);
        wl_keyboard_send_modifiers(kw->resource, serial,
                                   mods_depressed, mods_latched,
                                   mods_locked, group);
        sent++;
    }
    log_trace("globals: route_keyboard_modifiers client=%p d=%u l=%u lk=%u g=%u -> %d wl_keyboard(s)",
              (void*)client, mods_depressed, mods_latched, mods_locked, group, sent);
}
