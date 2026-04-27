//
// meek_shell_v1.c - privileged-shell extension implementation (C5 pass).
//
// WHAT THIS FILE DOES RIGHT NOW (C5):
//   - Creates the wl_global for meek_shell_v1 (one per display).
//   - Gates bind: any client attempting to bind must match a PID
//     allowlist (read + verified lazily via /proc/PID/exe). Non-
//     matching clients that somehow got the global interface
//     descriptor and tried to bind anyway get disconnected with
//     the `not_allowed` protocol error.
//   - Installs the full vtable as stubs: every request logs but
//     does nothing; there's no event dispatch back to shell yet.
//     That's intentional -- C6 (compositor-side forwarding of
//     other clients' buffers) and C7 (input routing) wire the
//     real event generators into this module.
//
// BIND GATE DETAIL:
//   wl_client_get_credentials(client, &pid, ...) gives us the
//   peer PID. Then readlink("/proc/<pid>/exe") resolves the path
//   of the binary that connected. We compare that against the
//   MEEK_SHELL_ALLOWED_EXE list.
//
//   If MEEK_SHELL_DEV is set in the environment at compositor
//   startup (exposed later; this file itself just reads the env
//   var every bind, cheap), the gate is relaxed to accept any
//   client. Intended for dev iteration where you're running
//   meek-shell from a checkout path outside the allowlist.
//

//
// memfd_create + MFD_CLOEXEC are Linux extensions. On glibc they
// live behind _GNU_SOURCE; on musl they live in <sys/mman.h> under
// the same gate. Must be defined BEFORE any libc include.
//
#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "xdg-shell-protocol.h"     //for xdg_toplevel_send_close (swipe-up-to-dismiss).
#include "meek-shell-v1-protocol.h"

#include "types.h"
#include "third_party/log.h"

#include "linux_dmabuf.h"  //for plane-info lookup when forwarding buffers.
#include "xdg_shell.h"     //for toplevel replay on announce_ready + find_wl_surface_by_handle.
#include "globals.h"       //for route_touch_*_to_client dispatch (Phase 6).
#include "text_input_v3.h" //for ime_commit_string forwarding (layer 2 IME).
#include "meek_shell_v1.h"

//
// Exe paths permitted to bind this extension. If the binary was
// installed the normal way it'll match "/usr/bin/meek_shell" or
// "/usr/libexec/meek-shell/meek_shell". Dev checkouts live
// wherever the user put them -- we cover a few common locations
// and fall back to the MEEK_SHELL_DEV=1 escape hatch.
//
static const char* const _meek_shell_v1_internal__allowed_exes[] = {
    "/usr/bin/meek_shell",
    "/usr/bin/meek-shell",
    "/usr/libexec/meek-shell/meek_shell",
    "/usr/local/bin/meek_shell",
    NULL,
};

//
// module-level state. One bound client at a time (second bind
// attempt while one is still live is rejected with already_bound
// error).
//
static struct wl_resource* _meek_shell_v1_internal__bound     = NULL;
static int                 _meek_shell_v1_internal__announced = 0;  //1 after announce_ready.

//
// Stashed at register-time. Needed by the kill_toplevel deferred-
// SIGKILL path which schedules a 2-second timer on this display's
// wl_event_loop.
//
static struct wl_display*  _meek_shell_v1_internal__display    = NULL;

//
// Per-pending-kill context. The kill_toplevel handler sends
// xdg_toplevel.close immediately, then schedules a 2s timer; if
// the client hasn't exited by then, SIGKILL fires. Heap-allocated
// because the timer callback runs on the wl_event_loop thread
// after the request handler has already returned.
//
typedef struct _meek_shell_v1_internal__kill_ctx
{
    pid_t                   pid;
    uint32_t                handle;
    struct wl_event_source* timer;
} _meek_shell_v1_internal__kill_ctx;

//
// Buffer-id cache. Every distinct wl_buffer we forward to the shell
// gets a stable, opaque uint32 buffer_id. The shell uses it as the
// key for its EGLImage + GL-texture cache: first time it sees a
// given id, import; subsequent times, reuse. This is the standard
// shared-import pattern across commits -- critical on drivers
// (Mesa/Zink on Adreno specifically) where per-commit eglCreateImage
// exhausts host VkImage memory within ~2 minutes at 60Hz.
//
// Implementation: intrusive wl_list keyed by wl_resource*. Each
// entry carries a wl_listener that fires when the wl_buffer
// resource is destroyed; the listener removes the entry from the
// list AND emits `buffer_forgotten(id)` to the shell so its cache
// entry is released symmetrically.
//
struct _meek_shell_v1_internal__buffer_cache_entry
{
    uint32_t            id;
    struct wl_resource* buffer;
    struct wl_listener  destroy_listener;
    struct wl_list      link;
};
static struct wl_list _meek_shell_v1_internal__buffer_cache;
static int            _meek_shell_v1_internal__buffer_cache_initialized = 0;
static uint32_t       _meek_shell_v1_internal__next_buffer_id = 1;

static void _meek_shell_v1_internal__buffer_cache_ensure_init(void)
{
    if (!_meek_shell_v1_internal__buffer_cache_initialized)
    {
        wl_list_init(&_meek_shell_v1_internal__buffer_cache);
        _meek_shell_v1_internal__buffer_cache_initialized = 1;
    }
}

static void _meek_shell_v1_internal__on_buffer_destroyed(struct wl_listener* listener, void* data)
{
    (void)data;
    struct _meek_shell_v1_internal__buffer_cache_entry* e =
        wl_container_of(listener, e, destroy_listener);
    //
    // Tell the shell to drop its cache entry. We check the bound
    // resource is still alive first -- the shell might have
    // disconnected already (destroy order isn't fixed).
    //
    if (_meek_shell_v1_internal__bound != NULL &&
        _meek_shell_v1_internal__announced)
    {
        meek_shell_v1_send_buffer_forgotten(
            _meek_shell_v1_internal__bound, e->id);
        struct wl_client* c = wl_resource_get_client(
            _meek_shell_v1_internal__bound);
        if (c != NULL) { wl_client_flush(c); }
    }
    wl_list_remove(&e->link);
    wl_list_remove(&e->destroy_listener.link);
    free(e);
}

static uint32_t _meek_shell_v1_internal__buffer_id_for(struct wl_resource* buffer)
{
    _meek_shell_v1_internal__buffer_cache_ensure_init();

    struct _meek_shell_v1_internal__buffer_cache_entry* e;
    wl_list_for_each(e, &_meek_shell_v1_internal__buffer_cache, link)
    {
        if (e->buffer == buffer) { return e->id; }
    }

    e = (struct _meek_shell_v1_internal__buffer_cache_entry*)
        calloc(1, sizeof(*e));
    if (e == NULL) { return 0; }

    e->id = _meek_shell_v1_internal__next_buffer_id++;
    e->buffer = buffer;
    e->destroy_listener.notify = _meek_shell_v1_internal__on_buffer_destroyed;
    wl_resource_add_destroy_listener(buffer, &e->destroy_listener);
    wl_list_insert(&_meek_shell_v1_internal__buffer_cache, &e->link);
    return e->id;
}

//
// forward decls for file-local statics.
//
static int  _meek_shell_v1_internal__exe_allowed(const char* exe);
static int  _meek_shell_v1_internal__read_exe(pid_t pid, char* buf, size_t buflen);
static void _meek_shell_v1_internal__replay_cb(uint32_t handle, const char* app_id, const char* title, void* userdata);

static void _meek_shell_v1_internal__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _meek_shell_v1_internal__on_announce_ready(struct wl_client* c, struct wl_resource* r);

static void _meek_shell_v1_internal__on_route_pointer_motion(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, int32_t surface_x, int32_t surface_y);
static void _meek_shell_v1_internal__on_route_pointer_button(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, uint32_t button, uint32_t state);
static void _meek_shell_v1_internal__on_route_key(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, uint32_t keycode, uint32_t state);
static void _meek_shell_v1_internal__on_route_touch_down(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, int32_t id, int32_t sx, int32_t sy);
static void _meek_shell_v1_internal__on_route_touch_motion(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, int32_t id, int32_t sx, int32_t sy);
static void _meek_shell_v1_internal__on_route_touch_up(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, int32_t id);

static void _meek_shell_v1_internal__on_route_keyboard_key(
    struct wl_client* c, struct wl_resource* r,
    uint32_t app_handle, uint32_t time_ms, uint32_t keycode, uint32_t state);
static void _meek_shell_v1_internal__on_route_keyboard_modifiers(
    struct wl_client* c, struct wl_resource* r,
    uint32_t app_handle, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);

static void _meek_shell_v1_internal__on_close_toplevel(
    struct wl_client* c, struct wl_resource* r, uint32_t handle);
static void _meek_shell_v1_internal__on_kill_toplevel(
    struct wl_client* c, struct wl_resource* r, uint32_t handle);
static int  _meek_shell_v1_internal__on_kill_timer(void* data);

static void _meek_shell_v1_internal__bind(
    struct wl_client* client, void* data, uint32_t version, uint32_t id);
static void _meek_shell_v1_internal__resource_destroy(struct wl_resource* r);

//============================================================================
// bind gate
//============================================================================

//
// Resolves /proc/<pid>/exe to a path. readlink() doesn't
// null-terminate; we handle that.
//
static int _meek_shell_v1_internal__read_exe(pid_t pid, char* buf, size_t buflen)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(path, buf, buflen - 1);
    if (n < 0)
    {
        log_error("meek_shell_v1: readlink(%s) failed: %s", path, strerror(errno));
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

static int _meek_shell_v1_internal__exe_allowed(const char* exe)
{
    //
    // Dev escape hatch. Checked every bind (cheap) so users can
    // toggle without compositor restart.
    //
    const char* dev = getenv("MEEK_SHELL_DEV");
    if (dev != NULL && dev[0] == '1')
    {
        log_warn("meek_shell_v1: MEEK_SHELL_DEV=1 -- bind gate bypassed for %s",
                 exe);
        return 1;
    }

    for (int i = 0; _meek_shell_v1_internal__allowed_exes[i] != NULL; ++i)
    {
        if (strcmp(exe, _meek_shell_v1_internal__allowed_exes[i]) == 0)
        {
            return 1;
        }
    }
    //
    // Also allow exes whose basename is meek_shell / meek-shell --
    // a little loose but covers the dev-checkout case without
    // needing MEEK_SHELL_DEV. Production tightening (signature
    // verify, systemd-cgroup check) deferred.
    //
    const char* slash = strrchr(exe, '/');
    const char* bn = slash ? slash + 1 : exe;
    if (strcmp(bn, "meek_shell") == 0 || strcmp(bn, "meek-shell") == 0)
    {
        log_warn("meek_shell_v1: allowing bind based on basename '%s' (path=%s)",
                 bn, exe);
        return 1;
    }
    return 0;
}

//============================================================================
// request handlers (stubs for C5)
//============================================================================

static void _meek_shell_v1_internal__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    log_info("meek_shell_v1.destroy called; shell is gone");
    wl_resource_destroy(r);
}

static void _meek_shell_v1_internal__on_announce_ready(struct wl_client* c, struct wl_resource* r)
{
    (void)c; (void)r;
    //
    // Flag the bound shell as ready to receive events. After this
    // point forwarding helpers (fire_toplevel_*, future raw-input
    // forwarders) stop no-op'ing and actually send events.
    //
    _meek_shell_v1_internal__announced = 1;
    log_info("meek_shell_v1.announce_ready -- event forwarding enabled");

    //
    // Replay pass: any toplevel that exists right now -- either
    // because apps connected before the shell bound, or because
    // this is a post-crash respawn -- must get its toplevel_added
    // replayed so the shell can rebuild its scene. Title/buffer
    // events DIDN'T fire while shell was pre-ready, so the most
    // recent (current) title goes out now with the replay and the
    // next commit by the client will fire toplevel_buffer naturally.
    //
    xdg_shell__foreach_toplevel_for_replay(_meek_shell_v1_internal__replay_cb, NULL);
}

//
// All six route_* handlers share the same "validate handle + log"
// shape today. C7 replaces the bodies with actual dispatch onto
// the target client's seat.
//

static void _meek_shell_v1_internal__on_route_pointer_motion(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, int32_t surface_x, int32_t surface_y)
{
    (void)c; (void)r;
    log_trace("meek_shell_v1.route_pointer_motion handle=%u t=%u (%d,%d)",
              handle, time_ms, surface_x, surface_y);
}

static void _meek_shell_v1_internal__on_route_pointer_button(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, uint32_t button, uint32_t state)
{
    (void)c; (void)r;
    log_trace("meek_shell_v1.route_pointer_button handle=%u t=%u button=%u state=%u",
              handle, time_ms, button, state);
}

static void _meek_shell_v1_internal__on_route_key(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, uint32_t keycode, uint32_t state)
{
    (void)c; (void)r;
    log_trace("meek_shell_v1.route_key handle=%u t=%u keycode=%u state=%u",
              handle, time_ms, keycode, state);
}

//
// Produce a monotonic-ms timestamp suitable for wl_touch / wl_pointer
// events. Wayland clients (GTK4 in particular) filter out input
// events whose `time` is 0 or goes backwards -- they treat those as
// invalid or out-of-order and discard them. The shell's
// meek_shell_v1.route_touch_* requests currently pass time_ms=0
// through from its gesture-recogniser stub; substituting a real
// CLOCK_MONOTONIC value here keeps the dispatch inside the
// compositor consistent with what libinput would produce for a
// physical touch. See session/bugs_to_investigate.md entry #2.
//
static uint32_t _meek_shell_v1_internal__monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
    return (uint32_t)((ts.tv_sec * 1000u) + (ts.tv_nsec / 1000000u));
}

//
// Track the last touch-down time per slot so the matching touch-up
// is guaranteed to arrive strictly later, with a minimum gap that
// matches typical physical tap duration. Shell's gesture recogniser
// emits route_touch_down and route_touch_up back-to-back on tap
// recognition, making both events carry effectively the same
// monotonic timestamp. GTK4's gesture-click recogniser discards
// zero-duration or near-zero-duration touch sequences as invalid;
// real taps are 50-300ms between press + release. 50ms is enough to
// pass GTK4's threshold without introducing noticeable latency.
//
#define _MEEK_SHELL_V1_INTERNAL__MIN_TAP_MS 50

static uint32_t _meek_shell_v1_internal__last_down_time[16];

static uint32_t _meek_shell_v1_internal__record_down_time(int32_t id, uint32_t time_ms)
{
    if (id < 0 || id >= (int32_t)(sizeof(_meek_shell_v1_internal__last_down_time)/sizeof(uint32_t)))
    {
        return time_ms;
    }
    _meek_shell_v1_internal__last_down_time[id] = time_ms;
    return time_ms;
}

static uint32_t _meek_shell_v1_internal__up_time_with_gap(int32_t id, uint32_t time_ms)
{
    if (id < 0 || id >= (int32_t)(sizeof(_meek_shell_v1_internal__last_down_time)/sizeof(uint32_t)))
    {
        return time_ms;
    }
    uint32_t last_down = _meek_shell_v1_internal__last_down_time[id];
    uint32_t min_up    = last_down + _MEEK_SHELL_V1_INTERNAL__MIN_TAP_MS;
    if (time_ms < min_up) { return min_up; }
    return time_ms;
}

static void _meek_shell_v1_internal__on_route_touch_down(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, int32_t id, int32_t sx, int32_t sy)
{
    (void)c; (void)r;
    //
    // Resolve handle -> target wl_surface -> target wl_client -> dispatch.
    // bad_handle and dead_handle are the same observable failure here
    // (surface lookup returns NULL); the XML calls these out as
    // protocol errors, but treating them as "silently drop" is safer
    // during shell development. We can promote to error-post when we
    // trust the shell's hit-testing.
    //
    struct wl_resource* surf = xdg_shell__find_wl_surface_by_handle(handle);
    if (surf == NULL)
    {
        log_warn("meek_shell_v1.route_touch_down: unknown handle=%u; dropped", handle);
        return;
    }
    //
    // Replace shell-supplied time_ms (often 0) with a monotonic
    // timestamp. See _monotonic_ms docblock above.
    //
    uint32_t dispatch_time = (time_ms != 0) ? time_ms : _meek_shell_v1_internal__monotonic_ms();
    //
    // Record this down-time so the matching up event (which shell
    // sends immediately after on a recognised tap) can be forced at
    // least MIN_TAP_MS later. See _up_time_with_gap docblock.
    //
    _meek_shell_v1_internal__record_down_time(id, dispatch_time);
    //
    // Shell sends tap coords scaled to buffer-sample space. If the
    // toplevel is under a fractional scale, surface-local logical
    // space is smaller than buffer space -- translate before
    // dispatching wl_touch.down. Helper returns 0 if data is
    // incomplete (no buffer/configure yet); we pass the original
    // coords through in that case (better wrong-pixel than dropped).
    //
    int32_t lsx = sx, lsy = sy;
    (void)xdg_shell__translate_tap_coords_for_surface(surf, sx, sy, &lsx, &lsy);
    log_trace("meek_shell_v1.route_touch_down handle=%u t=%u(->%u) id=%d (%d,%d) -> surface-local (%d,%d)",
              handle, time_ms, dispatch_time, id, sx, sy, lsx, lsy);
    struct wl_client* target = wl_resource_get_client(surf);
    globals__seat__route_touch_down_to_client(target, surf, dispatch_time, id, lsx, lsy);
}

static void _meek_shell_v1_internal__on_route_touch_motion(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, int32_t id, int32_t sx, int32_t sy)
{
    (void)c; (void)r;
    struct wl_resource* surf = xdg_shell__find_wl_surface_by_handle(handle);
    if (surf == NULL) { return; }
    uint32_t dispatch_time = (time_ms != 0) ? time_ms : _meek_shell_v1_internal__monotonic_ms();
    int32_t lsx = sx, lsy = sy;
    (void)xdg_shell__translate_tap_coords_for_surface(surf, sx, sy, &lsx, &lsy);
    log_trace("meek_shell_v1.route_touch_motion handle=%u t=%u(->%u) id=%d (%d,%d) -> surface-local (%d,%d)",
              handle, time_ms, dispatch_time, id, sx, sy, lsx, lsy);
    struct wl_client* target = wl_resource_get_client(surf);
    globals__seat__route_touch_motion_to_client(target, dispatch_time, id, lsx, lsy);
}

static void _meek_shell_v1_internal__on_route_touch_up(
    struct wl_client* c, struct wl_resource* r,
    uint32_t handle, uint32_t time_ms, int32_t id)
{
    (void)c; (void)r;
    uint32_t dispatch_time = (time_ms != 0) ? time_ms : _meek_shell_v1_internal__monotonic_ms();
    //
    // Enforce a minimum gap after the corresponding touch-down so
    // GTK4 doesn't discard the sequence as zero-duration.
    //
    dispatch_time = _meek_shell_v1_internal__up_time_with_gap(id, dispatch_time);
    log_trace("meek_shell_v1.route_touch_up handle=%u t=%u(->%u) id=%d",
              handle, time_ms, dispatch_time, id);
    struct wl_resource* surf = xdg_shell__find_wl_surface_by_handle(handle);
    if (surf == NULL) { return; }
    struct wl_client* target = wl_resource_get_client(surf);
    globals__seat__route_touch_up_to_client(target, dispatch_time, id);
}

//
// route_keyboard_key / route_keyboard_modifiers. The shell calls
// these when the on-screen widget_keyboard produces a keystroke
// that should land in the currently-focused foreign app. Mechanism
// matches route_touch_*:
//
//   1. Resolve app_handle -> wl_surface via xdg_shell lookup.
//   2. Get the owning wl_client of that surface.
//   3. Delegate to globals__seat__route_keyboard_*_to_client,
//      which iterates the per-client wl_keyboard resources and
//      emits wl_keyboard.enter (once) + wl_keyboard.key (or
//      wl_keyboard.modifiers).
//
// bad_handle is treated as "silently drop the event" for now. The
// XML declares it as a protocol error, but during shell dev a stale
// handle is more helpful as a log line than a disconnect.
//
static void _meek_shell_v1_internal__on_route_keyboard_key(
    struct wl_client* c, struct wl_resource* r,
    uint32_t app_handle, uint32_t time_ms, uint32_t keycode, uint32_t state)
{
    (void)c; (void)r;
    log_trace("meek_shell_v1.route_keyboard_key app_handle=%u t=%u keycode=%u state=%u",
              app_handle, time_ms, keycode, state);
    struct wl_resource* surf = xdg_shell__find_wl_surface_by_handle(app_handle);
    if (surf == NULL)
    {
        log_warn("meek_shell_v1.route_keyboard_key: unknown app_handle=%u; dropped", app_handle);
        return;
    }
    struct wl_client* target = wl_resource_get_client(surf);
    globals__seat__route_keyboard_key_to_client(target, surf, time_ms, keycode, state);
}

static void _meek_shell_v1_internal__on_route_keyboard_modifiers(
    struct wl_client* c, struct wl_resource* r,
    uint32_t app_handle,
    uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)c; (void)r;
    log_trace("meek_shell_v1.route_keyboard_modifiers app_handle=%u d=%u l=%u lk=%u g=%u",
              app_handle, depressed, latched, locked, group);
    struct wl_resource* surf = xdg_shell__find_wl_surface_by_handle(app_handle);
    if (surf == NULL)
    {
        log_warn("meek_shell_v1.route_keyboard_modifiers: unknown app_handle=%u; dropped", app_handle);
        return;
    }
    struct wl_client* target = wl_resource_get_client(surf);
    globals__seat__route_keyboard_modifiers_to_client(target, depressed, latched, locked, group);
}

static void _meek_shell_v1_internal__on_close_toplevel(
    struct wl_client* c, struct wl_resource* r, uint32_t handle)
{
    (void)c; (void)r;
    struct wl_resource* tl = xdg_shell__find_xdg_toplevel_by_handle(handle);
    if (tl == NULL)
    {
        log_warn("meek_shell_v1.close_toplevel handle=%u not in toplevel list", handle);
        return;
    }
    //
    // xdg_toplevel.close is a polite-close request: the spec calls
    // it "the equivalent to clicking the close button". The client
    // may show a save-prompt or just exit. Hard-kill is not exposed
    // through this protocol -- callers can SIGKILL the process if
    // they want a force-close.
    //
    xdg_toplevel_send_close(tl);
    log_info("meek_shell_v1.close_toplevel handle=%u -> xdg_toplevel.close sent", handle);
}

//
// Timer callback that fires SIGKILL on the deferred-kill context.
// Runs on the wl_event_loop thread 2 seconds after kill_toplevel was
// requested. Whether the app already exited cleanly in response to
// the polite xdg_toplevel.close (in which case kill returns ESRCH
// and we log "already gone") or it didn't (in which case SIGKILL
// terminates it), this is one-shot: timer source removed, ctx freed.
//
static int _meek_shell_v1_internal__on_kill_timer(void* data)
{
    _meek_shell_v1_internal__kill_ctx* ctx = (_meek_shell_v1_internal__kill_ctx*)data;
    if (ctx == NULL) { return 0; }
    int rc = kill(ctx->pid, SIGKILL);
    if (rc == 0)
    {
        log_info("meek_shell_v1.kill_toplevel handle=%u pid=%d -> SIGKILL fired (2s grace expired)", ctx->handle, (int)ctx->pid);
    }
    else if (errno == ESRCH)
    {
        log_info("meek_shell_v1.kill_toplevel handle=%u pid=%d: already exited within 2s grace (ESRCH)", ctx->handle, (int)ctx->pid);
    }
    else
    {
        log_warn("meek_shell_v1.kill_toplevel handle=%u pid=%d: kill failed errno=%d (%s)", ctx->handle, (int)ctx->pid, errno, strerror(errno));
    }
    if (ctx->timer != NULL) { wl_event_source_remove(ctx->timer); }
    free(ctx);
    return 0;
}

//
// Force-quit the target client. Used by the shell's swipe-up-to-
// dismiss gesture in the task switcher.
//
// Two-stage:
//   1. Send xdg_toplevel.close immediately. Apps with a clean-exit
//      handler (foot, gtk apps responding to delete-event, etc.)
//      get a chance to flush state / save / unmap cleanly within
//      the next 2 seconds.
//   2. Schedule a SIGKILL 2 seconds later via a wl_event_loop
//      timer. Fires unconditionally - apps that ignored close, or
//      hung in a save-prompt, or didn't have a handler at all
//      get terminated. If the app already exited cleanly in stage
//      1, the kill returns ESRCH and we just log "already gone".
//
// Either way, libwayland's EPIPE/destroy path fires toplevel_removed
// for the shell once the client process is gone, so callers don't
// need a separate cleanup call.
//
static void _meek_shell_v1_internal__on_kill_toplevel(
    struct wl_client* c, struct wl_resource* r, uint32_t handle)
{
    (void)c; (void)r;
    struct wl_resource* tl = xdg_shell__find_xdg_toplevel_by_handle(handle);
    if (tl == NULL)
    {
        log_warn("meek_shell_v1.kill_toplevel handle=%u not in toplevel list", handle);
        return;
    }
    struct wl_client* target = wl_resource_get_client(tl);
    if (target == NULL)
    {
        log_warn("meek_shell_v1.kill_toplevel handle=%u: wl_resource_get_client returned NULL", handle);
        return;
    }
    pid_t pid = 0;
    wl_client_get_credentials(target, &pid, NULL, NULL);
    if (pid <= 0)
    {
        log_warn("meek_shell_v1.kill_toplevel handle=%u: bad pid=%d from wl_client_get_credentials", handle, (int)pid);
        return;
    }
    //
    // Stage 1: polite ask.
    //
    xdg_toplevel_send_close(tl);
    log_info("meek_shell_v1.kill_toplevel handle=%u pid=%d -> xdg_toplevel.close sent (2s grace)", handle, (int)pid);

    //
    // Stage 2: schedule unconditional SIGKILL 2s later.
    //
    if (_meek_shell_v1_internal__display == NULL)
    {
        log_warn("meek_shell_v1.kill_toplevel: no display reference, killing immediately");
        kill(pid, SIGKILL);
        return;
    }
    struct wl_event_loop* loop = wl_display_get_event_loop(_meek_shell_v1_internal__display);
    if (loop == NULL)
    {
        log_warn("meek_shell_v1.kill_toplevel: wl_display_get_event_loop returned NULL, killing immediately");
        kill(pid, SIGKILL);
        return;
    }
    _meek_shell_v1_internal__kill_ctx* ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL)
    {
        log_warn("meek_shell_v1.kill_toplevel: calloc kill_ctx failed, killing immediately");
        kill(pid, SIGKILL);
        return;
    }
    ctx->pid    = pid;
    ctx->handle = handle;
    ctx->timer  = wl_event_loop_add_timer(loop, _meek_shell_v1_internal__on_kill_timer, ctx);
    if (ctx->timer == NULL)
    {
        log_warn("meek_shell_v1.kill_toplevel: add_timer failed, killing immediately");
        free(ctx);
        kill(pid, SIGKILL);
        return;
    }
    wl_event_source_timer_update(ctx->timer, 2000);
}

static void _meek_shell_v1_internal__on_ime_commit_string(
    struct wl_client* c, struct wl_resource* r, const char* text)
{
    (void)c; (void)r;
    log_trace("meek_shell_v1.ime_commit_string text='%s'", text ? text : "");
    text_input_v3__forward_commit_string(text);
}

static const struct meek_shell_v1_interface _meek_shell_v1_internal__impl = {
    .destroy                  = _meek_shell_v1_internal__on_destroy,
    .announce_ready           = _meek_shell_v1_internal__on_announce_ready,
    .route_pointer_motion     = _meek_shell_v1_internal__on_route_pointer_motion,
    .route_pointer_button     = _meek_shell_v1_internal__on_route_pointer_button,
    .route_key                = _meek_shell_v1_internal__on_route_key,
    .route_touch_down         = _meek_shell_v1_internal__on_route_touch_down,
    .route_touch_motion       = _meek_shell_v1_internal__on_route_touch_motion,
    .route_touch_up           = _meek_shell_v1_internal__on_route_touch_up,
    .close_toplevel           = _meek_shell_v1_internal__on_close_toplevel,
    .kill_toplevel            = _meek_shell_v1_internal__on_kill_toplevel,
    .ime_commit_string        = _meek_shell_v1_internal__on_ime_commit_string,
    .route_keyboard_key       = _meek_shell_v1_internal__on_route_keyboard_key,
    .route_keyboard_modifiers = _meek_shell_v1_internal__on_route_keyboard_modifiers,
};

void meek_shell_v1__fire_ime_request_on(uint32_t app_handle)
{
    if (_meek_shell_v1_internal__bound == NULL || !_meek_shell_v1_internal__announced) return;
    meek_shell_v1_send_ime_request_on(_meek_shell_v1_internal__bound, app_handle);
    struct wl_client* c = wl_resource_get_client(_meek_shell_v1_internal__bound);
    if (c != NULL) { wl_client_flush(c); }
}

void meek_shell_v1__fire_ime_request_off(uint32_t app_handle)
{
    if (_meek_shell_v1_internal__bound == NULL || !_meek_shell_v1_internal__announced) return;
    meek_shell_v1_send_ime_request_off(_meek_shell_v1_internal__bound, app_handle);
    struct wl_client* c = wl_resource_get_client(_meek_shell_v1_internal__bound);
    if (c != NULL) { wl_client_flush(c); }
}

//============================================================================
// resource + bind
//============================================================================

static void _meek_shell_v1_internal__resource_destroy(struct wl_resource* r)
{
    //
    // Client disconnected or shell called destroy. Clear the
    // one-at-a-time slot so a respawn can re-bind.
    //
    if (_meek_shell_v1_internal__bound == r)
    {
        _meek_shell_v1_internal__bound = NULL;
        log_info("meek_shell_v1: bound resource destroyed; slot free for rebind");
    }
}

static void _meek_shell_v1_internal__bind(
    struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    (void)data;

    //
    // PID + exe check. If libwayland doesn't give us credentials
    // (some unusual setups) we fail closed rather than open.
    //
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, NULL, NULL);
    if (pid <= 0)
    {
        log_error("meek_shell_v1: wl_client_get_credentials returned pid=%d; refusing bind",
                  (int)pid);
        wl_client_post_no_memory(client);
        return;
    }

    char exe[512] = {0};
    if (_meek_shell_v1_internal__read_exe(pid, exe, sizeof(exe)) != 0 ||
        !_meek_shell_v1_internal__exe_allowed(exe))
    {
        //
        // Post the not_allowed error and give the client back a
        // resource it can use to receive the error before we close
        // it. If we disconnect without creating a resource the
        // error message is silently lost.
        //
        struct wl_resource* err_res =
            wl_resource_create(client, &meek_shell_v1_interface, version, id);
        if (err_res != NULL)
        {
            wl_resource_post_error(err_res,
                                   MEEK_SHELL_V1_ERROR_NOT_ALLOWED,
                                   "meek_shell_v1 bind denied: pid=%d exe=%s",
                                   (int)pid, exe);
        }
        log_warn("meek_shell_v1: rejected bind from pid=%d exe=%s", (int)pid, exe);
        return;
    }

    //
    // Only one shell at a time. If already bound, reject.
    //
    if (_meek_shell_v1_internal__bound != NULL)
    {
        struct wl_resource* err_res =
            wl_resource_create(client, &meek_shell_v1_interface, version, id);
        if (err_res != NULL)
        {
            wl_resource_post_error(err_res,
                                   MEEK_SHELL_V1_ERROR_ALREADY_READY,
                                   "meek_shell_v1 already bound to another client");
        }
        log_warn("meek_shell_v1: rejected bind from pid=%d (another shell is bound)",
                 (int)pid);
        return;
    }

    struct wl_resource* r = wl_resource_create(client, &meek_shell_v1_interface, version, id);
    if (r == NULL)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r,
                                   &_meek_shell_v1_internal__impl,
                                   NULL,
                                   _meek_shell_v1_internal__resource_destroy);
    _meek_shell_v1_internal__bound = r;

    //
    // Demote any toplevels the shell process already created before
    // this bind. They'd been assigned handles because at creation
    // time we didn't know this client was the shell. Clear those
    // handles so we don't accidentally forward shell-owned windows
    // to the shell itself.
    //
    xdg_shell__demote_client_toplevels(client);

    log_info("meek_shell_v1: bound by pid=%d exe=%s (v=%u id=%u)",
             (int)pid, exe, version, id);
}

//============================================================================
// public
//============================================================================

void meek_shell_v1__register(struct wl_display* display)
{
    static int registered = 0;
    if (registered)
    {
        log_warn("meek_shell_v1__register called twice; ignoring");
        return;
    }
    registered = 1;

    struct wl_global* g = wl_global_create(display,
                                           &meek_shell_v1_interface,
                                           /*version*/ 1,
                                           /*data*/    NULL,
                                           _meek_shell_v1_internal__bind);
    if (g == NULL)
    {
        log_fatal("wl_global_create(meek_shell_v1) failed");
        return;
    }
    _meek_shell_v1_internal__display = display;
    log_info("meek_shell_v1 registered (v1, PID-gated bind)");
}

//
// Accessor for the currently-bound shell client. Declared in
// meek_shell_v1.h; documented there. Returns NULL if no shell is
// bound. Used by globals.c's wl_surface commit dispatch to
// identify scanout-surface candidates and by output_drm.c for the
// same reason.
//
struct wl_client* meek_shell_v1__get_shell_client(void)
{
    if (_meek_shell_v1_internal__bound == NULL)
    {
        return NULL;
    }
    return wl_resource_get_client(_meek_shell_v1_internal__bound);
}

// ---------------------------------------------------------------------------
// toplevel event forwarders (compositor -> shell)
// ---------------------------------------------------------------------------
//
// All four guard on "_bound non-null AND _announced == 1". Before
// announce_ready we drop events on purpose: on_announce_ready
// below replays the current toplevel list via
// xdg_shell__foreach_toplevel_for_replay, so a late-binding shell
// doesn't miss state. The intermediate events (title changes,
// buffer commits) that happen before announce_ready are discarded;
// the most recent state is what the replay carries forward.
//
// All helpers are safe no-ops when no shell is bound at all.
//

void meek_shell_v1__fire_toplevel_added(uint32_t handle, const char* app_id, const char* title)
{
    if (_meek_shell_v1_internal__bound == NULL || !_meek_shell_v1_internal__announced) return;
    meek_shell_v1_send_toplevel_added(
        _meek_shell_v1_internal__bound,
        handle,
        app_id != NULL ? app_id : "",
        title  != NULL ? title  : "");
    log_trace("meek_shell_v1.toplevel_added sent handle=%u app_id=\"%s\" title=\"%s\"",
              handle, app_id ? app_id : "", title ? title : "");
}

void meek_shell_v1__fire_toplevel_title_changed(uint32_t handle, const char* title)
{
    if (_meek_shell_v1_internal__bound == NULL || !_meek_shell_v1_internal__announced) return;
    meek_shell_v1_send_toplevel_title_changed(
        _meek_shell_v1_internal__bound,
        handle,
        title != NULL ? title : "");
    log_trace("meek_shell_v1.toplevel_title_changed sent handle=%u title=\"%s\"",
              handle, title ? title : "");
}

void meek_shell_v1__fire_toplevel_removed(uint32_t handle)
{
    if (_meek_shell_v1_internal__bound == NULL || !_meek_shell_v1_internal__announced) return;
    meek_shell_v1_send_toplevel_removed(_meek_shell_v1_internal__bound, handle);
    log_trace("meek_shell_v1.toplevel_removed sent handle=%u", handle);
}

//
// =====================================================================
// CRITICAL: fd-lifecycle policy for toplevel_buffer forwarding.
// =====================================================================
//
// Context. For every app-window commit, we forward the dmabuf fd to
// the shell process so it can import it as a GL texture and render
// the window as a thumbnail. The fd crosses the process boundary via
// SCM_RIGHTS on the Wayland socket, so we need our own dup() of the
// plane-0 fd (the original stays with linux_dmabuf for the buffer's
// lifetime; the dup is the one libwayland transmits and closes).
//
// The leak-trap. libwayland queues fd-carrying events and only sends
// them (and closes the dup) when the connection is flushed. Default
// flush cadence is "once per event-loop iteration" which is fine at
// modest rates but not when clients commit fast: we saw a naive loop
// of demo-settings commit at ~250 Hz (see note below about missing
// frame callbacks) which accumulated ~1000 un-flushed fds in a few
// seconds and hit RLIMIT_NOFILE. Once dup() fails, the whole
// meek_shell_v1 resource tears down and the shell loses its
// privileged channel -- apps stop being forwarded, input stops
// reaching the shell, etc.
//
// Approaches considered:
//   (a) Pointer-diff gate (only forward when s->current.buffer !=
//       s->last_forwarded_buffer). Ineffective standalone: triple-
//       buffered clients rotate through 2-3 wl_buffers so the
//       pointer changes every commit even when content is the
//       same. Kept as a cheap upstream check in globals.c because
//       it does catch literal same-buffer-twice no-ops.
//   (b) Time-based throttle (100 ms cadence per handle). Works
//       mechanically but blocks real-time thumbnails and papers
//       over the real problem. Rejected.
//   (c) Explicit wl_client_flush after send (current). Forces
//       libwayland to transmit + close the dup immediately, keeping
//       fds-in-flight at ~1 per handle instead of queuing. Allows
//       unlimited forward rate.
//
// Residual issue + real root cause. Even with (c) we occasionally
// see a single dup() failure during startup bursts. The real fix is
// to send wl_surface.frame "done" callbacks so clients throttle
// themselves to the display rate (60 Hz) instead of spinning as
// fast as the CPU will let them. That's a compositor-wide change
// tracked separately; once it lands, the commit rate drops ~4x and
// the burst disappears. For now, (c) is sufficient for stable runs.
//
// If you're tempted to "simplify" this by removing the flush, read
// the fd-exhaustion post-mortem above first.
//
//
// Forward an shm-backed wl_buffer to the shell via the
// toplevel_buffer_shm event. wl_shm doesn't expose the pool fd in
// the server-side API, so we can't zero-copy-dup it across; copy
// the pixels into a fresh memfd per commit and send that fd.
//
// Bandwidth: one 1080x2246 ARGB8888 buffer = ~9.7 MB. Foot and
// similar CPU-rendered clients commit at ~1-10 Hz (on cursor
// blink, on text change), so total throughput is modest.
//
// Life cycle of the fd: libwayland's wl_closure_send dups the fd
// into its SCM_RIGHTS batch (per crucial_fixes #7), so we close
// our own fd immediately after the flush. Shell receives a fresh
// fd on its side and is responsible for closing it after the
// glTexImage2D upload + cache insert.
//
static void _meek_shell_v1_internal__fire_toplevel_buffer_shm(uint32_t handle, struct wl_resource* buffer, struct wl_shm_buffer* shm_buf)
{
    int32_t w      = wl_shm_buffer_get_width(shm_buf);
    int32_t h      = wl_shm_buffer_get_height(shm_buf);
    int32_t stride = wl_shm_buffer_get_stride(shm_buf);
    uint32_t fmt   = wl_shm_buffer_get_format(shm_buf);
    if (w <= 0 || h <= 0 || stride < w * 4)
    {
        log_warn("meek_shell_v1: shm handle=%u bad dims %dx%d stride=%d; skip",
                 handle, w, h, stride);
        return;
    }

    size_t bytes = (size_t)stride * (size_t)h;

    //
    // Allocate a fresh memfd each commit. Sized EXACTLY to the
    // pixel region so the shell's mmap covers the full payload
    // without over-reading.
    //
    int fd = memfd_create("meek-shm-forward", MFD_CLOEXEC);
    if (fd < 0)
    {
        log_error("meek_shell_v1: memfd_create for shm forward failed: %s", strerror(errno));
        return;
    }
    if (ftruncate(fd, (off_t)bytes) < 0)
    {
        log_error("meek_shell_v1: ftruncate(%zu) failed: %s", bytes, strerror(errno));
        close(fd);
        return;
    }
    void* dst = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (dst == MAP_FAILED)
    {
        log_error("meek_shell_v1: mmap memfd %zu bytes failed: %s", bytes, strerror(errno));
        close(fd);
        return;
    }

    //
    // wl_shm_buffer_begin_access / end_access brackets: without
    // them, a client that destroys its shm pool mid-access
    // would SIGBUS us. The libwayland-server calls install a
    // SIGBUS handler for the duration.
    //
    wl_shm_buffer_begin_access(shm_buf);
    const void* src = wl_shm_buffer_get_data(shm_buf);
    if (src != NULL)
    {
        memcpy(dst, src, bytes);
    }
    wl_shm_buffer_end_access(shm_buf);

    munmap(dst, bytes);

    uint32_t buffer_id = _meek_shell_v1_internal__buffer_id_for(buffer);
    meek_shell_v1_send_toplevel_buffer_shm(
        _meek_shell_v1_internal__bound,
        handle,
        buffer_id,
        fd,
        w, h, stride,
        fmt);

    struct wl_client* shell_client = wl_resource_get_client(_meek_shell_v1_internal__bound);
    if (shell_client != NULL) { wl_client_flush(shell_client); }

    //
    // Same fd-close rule as toplevel_buffer (crucial_fixes #7):
    // libwayland dup'd our fd into the SCM_RIGHTS batch; we close
    // our copy so we don't leak 1 fd per commit.
    //
    close(fd);

    //
    // Record buffer dims for route_touch coord translation (same
    // rationale as the dmabuf path above).
    //
    {
        struct wl_resource* surf = xdg_shell__find_wl_surface_by_handle(handle);
        if (surf != NULL)
        {
            xdg_shell__record_buffer_size(surf, w, h);
        }
    }

    log_trace("meek_shell_v1.toplevel_buffer_shm sent handle=%u %dx%d stride=%d fmt=%u bytes=%zu",
              handle, w, h, stride, fmt, bytes);
}

void meek_shell_v1__fire_toplevel_buffer(uint32_t handle, struct wl_resource* buffer)
{
    if (_meek_shell_v1_internal__bound == NULL || !_meek_shell_v1_internal__announced) return;
    if (handle == 0 || buffer == NULL) return;

    //
    // shm vs dmabuf branch. wl_shm_buffer_get returns non-NULL only
    // for shm-backed wl_buffers; dmabufs live in a separate table.
    // Foot + simple shm-based clients use shm; gtk4 / chromium / mpv
    // use dmabuf.
    //
    struct wl_shm_buffer* shm_buf = wl_shm_buffer_get(buffer);
    if (shm_buf != NULL)
    {
        _meek_shell_v1_internal__fire_toplevel_buffer_shm(handle, buffer, shm_buf);
        return;
    }

    struct linux_dmabuf_buffer_info info;
    if (!linux_dmabuf__get_buffer_info(buffer, &info))
    {
        log_trace("meek_shell_v1: toplevel handle=%u buffer is neither shm nor dmabuf; skip", handle);
        return;
    }
    if (info.plane_count < 1 || info.planes[0].fd < 0)
    {
        log_warn("meek_shell_v1: handle=%u dmabuf info invalid (plane_count=%d plane0_fd=%d)",
                 handle, info.plane_count, info.planes[0].fd);
        return;
    }

    //
    // Dup the plane-0 fd so the shell gets its own fd it can close
    // independently. Our original stays alive for the buffer's
    // lifetime (see linux_dmabuf.c). SCM_RIGHTS on the Wayland
    // socket carries the dup across the process boundary.
    //
    int dup_fd = dup(info.planes[0].fd);
    if (dup_fd < 0)
    {
        //
        // See the fd-lifecycle comment block above. If this fires
        // repeatedly, the flush isn't keeping up -- likely because
        // frame callbacks aren't yet implemented and a client is
        // committing in a tight loop.
        //
        log_error("meek_shell_v1: dup() failed on plane-0 fd: %s", strerror(errno));
        return;
    }

    //
    // Modifier is 64-bit; XML splits into two 32-bit args. Use
    // explicit casts to make the narrowing intentional.
    //
    uint32_t mod_hi = (uint32_t)(info.planes[0].modifier >> 32);
    uint32_t mod_lo = (uint32_t)(info.planes[0].modifier & 0xFFFFFFFFu);

    uint32_t buffer_id = _meek_shell_v1_internal__buffer_id_for(buffer);

    meek_shell_v1_send_toplevel_buffer(
        _meek_shell_v1_internal__bound,
        handle,
        buffer_id,
        dup_fd,
        info.width,
        info.height,
        (int32_t)info.planes[0].stride,
        info.fourcc,
        mod_hi,
        mod_lo);

    //
    // Flush now so libwayland ACTUALLY sends the event (with its own
    // internal dup of our fd) via SCM_RIGHTS before we close below.
    //
    struct wl_client* shell_client = wl_resource_get_client(_meek_shell_v1_internal__bound);
    if (shell_client != NULL) { wl_client_flush(shell_client); }

    //
    // Close our dup'd fd. wl_closure_send() inside libwayland
    // duplicates the fd again into its SCM_RIGHTS batch; our
    // `dup_fd` is never transferred to libwayland. Without this
    // close, every toplevel_buffer leaks 1 fd in the compositor --
    // ~4000 leaks in ~67s at 60 Hz, which trips RLIMIT_NOFILE and
    // disconnects the shell with errno=EPIPE. See crucial_fixes.md
    // entry #7.
    //
    close(dup_fd);

    //
    // Record the buffer dimensions on the toplevel so route_touch_*
    // coord translation can compute the buffer-to-logical ratio.
    // Shell passes tap coords scaled to buffer size; we convert to
    // surface-local using the xdg_toplevel's current configure size.
    // See session/design_level2_fractional_scaling.md.
    //
    {
        struct wl_resource* surf = xdg_shell__find_wl_surface_by_handle(handle);
        if (surf != NULL)
        {
            xdg_shell__record_buffer_size(surf, info.width, info.height);
        }
    }

    log_trace("meek_shell_v1.toplevel_buffer sent handle=%u %dx%d fourcc=0x%x mod=%lx",
              handle, info.width, info.height, info.fourcc, (unsigned long)info.planes[0].modifier);
}

//
// Replay callback for xdg_shell__foreach_toplevel_for_replay. Called
// once per live non-shell toplevel during on_announce_ready. Fires
// one toplevel_added per toplevel; subsequent title_changed /
// buffer events during the replay window use _announced which
// we've already set to 1 before calling.
//
static void _meek_shell_v1_internal__replay_cb(uint32_t handle, const char* app_id, const char* title, void* userdata)
{
    (void)userdata;
    meek_shell_v1_send_toplevel_added(
        _meek_shell_v1_internal__bound,
        handle,
        app_id != NULL ? app_id : "",
        title  != NULL ? title  : "");
    log_trace("meek_shell_v1: replay toplevel_added handle=%u", handle);
}

//
// Raw-touch forwarders (Phase 5). input.c calls these in addition
// to the existing wl_touch dispatch via wl_seat. The shell runs
// its gesture recognizer off these; wl_touch still exists for
// widgets that just want focused-surface touches.
//
// We deliberately do NOT gate on `announced` here the way the
// toplevel_* fire_* helpers do: if a shell is bound but hasn't
// announce_ready'd, dropping touches means the first few moments
// of user input vanish. Send them anyway; the shell's recognizer
// either processes them or ignores them, but they arrive.
//
void meek_shell_v1__fire_touch_down_raw(uint32_t time_ms, int32_t id, int32_t x, int32_t y)
{
    if (_meek_shell_v1_internal__bound == NULL) return;
    meek_shell_v1_send_touch_down_raw(_meek_shell_v1_internal__bound, time_ms, id, x, y);
}

void meek_shell_v1__fire_touch_motion_raw(uint32_t time_ms, int32_t id, int32_t x, int32_t y)
{
    if (_meek_shell_v1_internal__bound == NULL) return;
    meek_shell_v1_send_touch_motion_raw(_meek_shell_v1_internal__bound, time_ms, id, x, y);
}

void meek_shell_v1__fire_touch_up_raw(uint32_t time_ms, int32_t id)
{
    if (_meek_shell_v1_internal__bound == NULL) return;
    meek_shell_v1_send_touch_up_raw(_meek_shell_v1_internal__bound, time_ms, id);
}
