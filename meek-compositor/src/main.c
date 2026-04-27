//
//meek-compositor/src/main.c - Wayland server skeleton.
//
//This is the SCAFFOLDING pass. What works:
//  - wl_display is created.
//  - A Wayland socket (wayland-N under $XDG_RUNTIME_DIR) is exposed.
//  - The event loop runs, dispatching socket-accept and protocol
//    reads, but there are no globals registered yet, so any client
//    that connects gets a clean empty bind and disconnects.
//  - Logging goes through meek-ui's third_party/log.h (rxi logger)
//    instead of raw fprintf, so output is timestamped + level-tagged
//    and future log-to-file / log-to-syslog hooks are a one-liner.
//  - memory_manager is initialized at startup + shut down at exit so
//    any allocations (none yet in this pass) are tracked for leak
//    reporting.
//
//What is INTENTIONALLY missing and lives in the backlog (see
//session/roadmap.md for full plan):
//  - wl_compositor / wl_shm / xdg_wm_base / wl_seat / linux-dmabuf
//    global registration (pass A2, in progress).
//  - EGL context + dmabuf buffer import (pass A3).
//  - Trivial compositor-side render into a nested X11 window
//    (pass A4, temporary -- gutted once meek-shell takes over in C6).
//  - meek_shell_v1 privileged extension (pass C5).
//  - Cross-process buffer forwarding to meek-shell (pass C6).
//
//Architectural choice on record (see roadmap.md for detail):
//  - Three-project split. meek-compositor is a pure protocol server
//    + presentation layer with NO UI library inside it. meek-shell
//    (separate project) runs as a privileged Wayland client (binds
//    meek_shell_v1) and does all the drawing, using meek-ui as a
//    library. This file stays narrow: sockets, protocol dispatch,
//    input forward, present one surface to the screen.
//  - Linking libwayland-server directly (no compositor framework
//    library) and hand-rolling the protocol objects.
//

#include <stdarg.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h> //getenv().
#include <string.h>
#include <strings.h> //strcasecmp().
#include <unistd.h>  //access() for MEEK_COMPOSITOR_OUTPUT auto-detection.
#include <errno.h>
#include <sys/resource.h>

#include <wayland-server-core.h>

#include "types.h"
#include "third_party/log.h"
#include "clib/memory_manager.h"

#include "globals.h"
#include "egl_ctx.h"
#include "linux_dmabuf.h"
#include "output_x11.h"
#include "output_drm.h"
#include "input.h"

//
//MEEK_LOG_LEVEL env var support. Accepts one of
//"trace|debug|info|warn|error|fatal" (case-insensitive). Unset or
//unrecognized falls back to the compile-time default (LOG_TRACE
//in this file's main). Lets operators tune verbosity at runtime
//without rebuilding -- useful over SSH where editing source is
//slower than editing a systemd drop-in.
//
static int _main_internal__parse_log_level(const char* s, int fallback)
{
    if (s == NULL)              { return fallback; }
    if (strcasecmp(s, "trace") == 0) { return LOG_TRACE; }
    if (strcasecmp(s, "debug") == 0) { return LOG_DEBUG; }
    if (strcasecmp(s, "info")  == 0) { return LOG_INFO;  }
    if (strcasecmp(s, "warn")  == 0) { return LOG_WARN;  }
    if (strcasecmp(s, "error") == 0) { return LOG_ERROR; }
    if (strcasecmp(s, "fatal") == 0) { return LOG_FATAL; }
    return fallback;
}

//
//module-level state. kept small on purpose in this pass.
//
static struct wl_display* _main_internal__display = NULL;

//
//forward decls for file-local statics
//
static void _main_internal__on_signal(int signum);
static void _main_internal__on_sigusr1(int signum);
static void _main_internal__wl_log(const char* fmt, va_list args);

//
//SIGINT / SIGTERM handler. wl_display_run() blocks on
//wl_event_loop_dispatch() until wl_display_terminate() is called,
//so we plumb the signal back through the display.
//
static void _main_internal__on_signal(int signum)
{
    (void)signum;
    if (_main_internal__display != NULL)
    {
        wl_display_terminate(_main_internal__display);
    }
}

//
//SIGUSR1 handler -- "screenshot the next rendered frame". The
//body is intentionally minimal (one call into output_drm that
//just sets a volatile sig_atomic_t flag) because that's what's
//safe to do from async signal context. All the actual work
//(glReadPixels, malloc, fopen, fwrite, fclose) happens in the
//render tick which runs in normal program context.
//
static void _main_internal__on_sigusr1(int signum)
{
    (void)signum;
    output_drm__request_screenshot_from_signal();
}

//
//wayland logger. libwayland-server invokes this via
//wl_log_set_handler_server for protocol errors and internal
//warnings. rxi's log_log doesn't have a va_list variant, so we
//format into a stack buffer first and then hand the pre-formatted
//string to log_warn -- libwayland's logger fires on recoverable
//protocol errors, not on info-level noise, so warn is the right
//level.
//
static void _main_internal__wl_log(const char* fmt, va_list args)
{
    char buf[1024];
    int  n = vsnprintf(buf, sizeof(buf), fmt, args);
    //
    //libwayland tends to include a trailing newline in its format
    //strings; log_warn adds one too, so strip ours to avoid
    //double-newlines in output.
    //
    if (n > 0 && n < (int)sizeof(buf) && buf[n - 1] == '\n')
    {
        buf[n - 1] = '\0';
    }
    log_warn("[wl] %s", buf);
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    //
    //Line-buffered stderr so the last log line before any crash/
    //signal actually reaches the file. Default (block-buffered when
    //stderr is redirected to a file) hides the final few messages
    //on abnormal exit, which makes post-mortem debugging guesswork.
    //
    setvbuf(stderr, NULL, _IOLBF, 0);

    //
    //logger + allocation tracker init. Default is LOG_TRACE; env
    //var MEEK_LOG_LEVEL overrides (trace|debug|info|warn|error|fatal).
    //memory_manager is a no-op unless GUI_TRACK_ALLOCATIONS was
    //defined at compile time, so this pair is safe to always call.
    //
    log_set_level(_main_internal__parse_log_level(getenv("MEEK_LOG_LEVEL"),
                                                  LOG_TRACE));
    memory_manager__init();

    //
    //Raise RLIMIT_NOFILE to the hard limit. A Wayland compositor
    //handles a lot of file descriptors: one per connected client's
    //socket, one per dmabuf plane the client attaches (plus the
    //dup we send to the shell via meek_shell_v1.toplevel_buffer),
    //one per input device /dev/input/eventN, DRM card0 + renderD128,
    //pipes for signalfd, hot-reload inotify watchers, etc.
    //
    //The default soft limit is typically 1024 on distros, which is
    //fine for a desktop session but trivially exhausted during
    //startup bursts where multiple clients commit their initial
    //buffers in one event-loop tick (libwayland queues the dup'd
    //fds until the connection flushes; explicit wl_client_flush
    //after send bounds this to ~1 in-flight per handle, but the
    //safety margin matters for multi-app scenes).
    //
    //Bumping to the hard limit (usually 4096 or 1048576) removes
    //this as a failure mode entirely. If the hard limit itself is
    //restrictive (containers, cgroup quotas), we log and continue
    //since the compositor will still run, just less resilient to
    //burst loads.
    //
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
        {
            rlim_t old_soft = rl.rlim_cur;
            rl.rlim_cur = rl.rlim_max;
            if (setrlimit(RLIMIT_NOFILE, &rl) == 0)
            {
                log_info("fd limit: raised %lu -> %lu (hard cap)",
                         (unsigned long)old_soft, (unsigned long)rl.rlim_cur);
            }
            else
            {
                log_warn("fd limit: setrlimit failed (%s); staying at %lu",
                         strerror(errno), (unsigned long)old_soft);
            }
        }
    }

    wl_log_set_handler_server(_main_internal__wl_log);

    //
    //wl_display owns the event loop. everything else (globals,
    //clients, event sources) attaches to it.
    //
    _main_internal__display = wl_display_create();
    if (_main_internal__display == NULL)
    {
        log_fatal("wl_display_create failed");
        memory_manager__shutdown();
        return 1;
    }

    //
    //wl_display_add_socket_auto picks the lowest unused wayland-N
    //under $XDG_RUNTIME_DIR. clients set WAYLAND_DISPLAY to the
    //returned name to connect.
    //
    const char* socket = wl_display_add_socket_auto(_main_internal__display);
    if (socket == NULL)
    {
        log_fatal("wl_display_add_socket_auto failed (XDG_RUNTIME_DIR set?)");
        wl_display_destroy(_main_internal__display);
        _main_internal__display = NULL;
        memory_manager__shutdown();
        return 1;
    }

    log_info("listening on WAYLAND_DISPLAY=%s", socket);

    //
    //EGL + GLES context for client-buffer import. Must be up BEFORE
    //linux-dmabuf is advertised, because advertised format/modifier
    //pairs come from querying this EGL display. If EGL fails we
    //keep running with shm-only clients (dmabuf global just won't
    //be registered); a compositor with no rendering pipeline at all
    //is still useful as a protocol testbed.
    //
    if (egl_ctx__init() != 0)
    {
        log_warn("EGL init failed -- running shm-only; dmabuf clients will reject");
    }

    //
    //Register the standard Wayland globals. zwp_linux_dmabuf_v1
    //follows once buffer-import code lands (pass A3.2/3).
    //
    globals__register_all(_main_internal__display);

    //
    //Output backend selection.
    //
    //MEEK_COMPOSITOR_OUTPUT=drm       -> direct KMS on /dev/dri/card0
    //                                   (phone deployment config 1;
    //                                    needs DRM master so no other
    //                                    compositor may be running)
    //MEEK_COMPOSITOR_OUTPUT=x11       -> nested X11 window (desktop
    //                                    dev)
    //MEEK_COMPOSITOR_OUTPUT=headless  -> no output; compositor is
    //                                    pure protocol broker. Use
    //                                    when another process (e.g.
    //                                    direct-DRM meek-shell) owns
    //                                    the screen and just needs
    //                                    us for Wayland protocol +
    //                                    meek_shell_v1 forwarding.
    //                                    Config 2 in roadmap.md.
    //                                    "none" is an alias.
    //
    //Default selection: prefer drm when we look like we're on a
    //bare console ($DISPLAY unset AND /dev/dri/card0 openable),
    //prefer x11 when $DISPLAY is set. None is the fallback if both
    //options fail.
    //
    const char* output_mode = getenv("MEEK_COMPOSITOR_OUTPUT");
    if (output_mode == NULL || output_mode[0] == '\0')
    {
        if (getenv("DISPLAY") != NULL)
        {
            output_mode = "x11";
        }
        else if (access("/dev/dri/card0", R_OK | W_OK) == 0)
        {
            output_mode = "drm";
        }
        else
        {
            output_mode = "none";
        }
        log_info("MEEK_COMPOSITOR_OUTPUT unset -- auto-selected '%s'", output_mode);
    }

    if (strcasecmp(output_mode, "drm") == 0)
    {
        if (output_drm__init(_main_internal__display) != 0)
        {
            log_warn("output_drm init failed -- running headless (no visible output)");
        }
    }
    else if (strcasecmp(output_mode, "x11") == 0)
    {
        if (output_x11__init(_main_internal__display, 1280, 720) != 0)
        {
            log_warn("output_x11 init failed -- running headless (no visible window)");
        }
    }
    else if (strcasecmp(output_mode, "none") == 0 ||
             strcasecmp(output_mode, "headless") == 0)
    {
        //
        //Headless: no output backend at all. Compositor runs as a
        //pure protocol broker + buffer forwarder. Config 2 in
        //session/roadmap.md terms: used when a DIFFERENT process
        //(typically a meek-shell linked against meek-ui's
        //platform_linux_drm.c) owns the screen directly and this
        //compositor exists only to observe + forward other clients'
        //buffers via meek_shell_v1. Also useful for protocol-
        //compliance testing.
        //
        log_info("MEEK_COMPOSITOR_OUTPUT=%s -- headless mode, no output backend",
                 output_mode);
    }
    else
    {
        log_warn("unknown MEEK_COMPOSITOR_OUTPUT='%s' -- running headless", output_mode);
    }

    //
    //graceful shutdown on Ctrl-C / kill. no sigaction struct
    //ceremony -- a bare signal() is enough for the scaffold and
    //avoids dragging sys/signal.h into this file before we need it.
    //
    signal(SIGINT,  _main_internal__on_signal);
    signal(SIGTERM, _main_internal__on_signal);

    //
    //SIGUSR1 = "capture the next rendered frame as a PPM
    //screenshot to /tmp/meek-screenshot.ppm". Wired only for the
    //DRM backend (nested X11 dev has no reason to screenshot via
    //us -- the host compositor already can). Handler just flips
    //a sig_atomic_t; the actual glReadPixels happens in
    //render_and_present before swap.
    //
    signal(SIGUSR1, _main_internal__on_sigusr1);

    //
    //libinput setup. Must come AFTER globals__register_all so
    //wl_seat is bindable. Non-fatal if it fails (no input devices
    //accessible); compositor continues running with zero input.
    //
    if (input__init(_main_internal__display) != 0)
    {
        log_warn("input: init failed -- running without input capture");
    }

    //
    //blocks until wl_display_terminate is called (from the signal
    //handler above, or later from a protocol error path).
    //
    wl_display_run(_main_internal__display);

    log_info("shutting down");
    //
    // Shut down whichever output backend was running. Each is a
    // no-op if it wasn't init'd, so calling both unconditionally
    // is safe and keeps this block insensitive to whatever the
    // selector chose.
    //
    input__shutdown();
    output_drm__shutdown();
    output_x11__shutdown();
    //
    //linux_dmabuf__shutdown BEFORE egl_ctx__shutdown: the format
    //cache was populated by querying our EGL display, and while the
    //format bytes themselves are fine to free after EGL goes down,
    //keeping the call-order sane makes it easier to add any future
    //EGL-touching teardown (eglDestroyImage on cached images, etc.)
    //without reordering.
    //
    linux_dmabuf__shutdown();
    egl_ctx__shutdown();
    wl_display_destroy(_main_internal__display);
    _main_internal__display = NULL;

    memory_manager__shutdown();
    return 0;
}
