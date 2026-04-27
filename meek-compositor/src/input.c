//
// input.c - libinput → wl_seat bridge.
//
// See input.h for scope and rationale.
//
// HIGH-LEVEL FLOW
// ---------------
// 1. libinput_udev_create_context + assign_seat("seat0") gives us
//    a context that enumerates + opens input devices under seat0.
// 2. libinput exposes one fd. We register it on the compositor's
//    wl_event_loop. When it becomes readable, drain + dispatch.
// 3. For each libinput event, translate to Wayland protocol and
//    call into globals.c's wl_touch dispatch helpers.
// 4. Coordinates: libinput reports normalized values; we pass the
//    panel's native dimensions to get_*_transformed() to convert
//    them into screen pixels. Panel dimensions come from
//    output_drm (DRM path). If output_drm isn't active (headless /
//    nested X11) we fall back to the placeholder 1920x1080; touch
//    calibration may be off in that case but it's a dev-only path.
//
// OPEN_RESTRICTED
// ---------------
// libinput asks US to open every /dev/input/event* file via our
// open_restricted callback. Simplest implementation: plain open()
// with the flags libinput gave us. That works when the user has
// ACL/group access to those devices (standard on pmOS with
// elogind). We do NOT integrate libseat here -- that's a bigger
// project and unnecessary for the common case.
//

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libinput.h>
#include <libudev.h>
#include <wayland-server-core.h>

#include "types.h"
#include "third_party/log.h"

#include "globals.h"
#include "output_drm.h"    //for panel native size -> touch coordinate scale.
#include "meek_shell_v1.h" //raw-touch forwarders for the shell gesture recognizer.
#include "input.h"

//
// module-level state.
//
static struct udev*              _input_internal__udev     = NULL;
static struct libinput*          _input_internal__li       = NULL;
static struct wl_event_source*   _input_internal__li_src   = NULL;

//
// forward decls for file-local statics.
//
static int  _input_internal__open_restricted(const char* path, int flags, void* user_data);
static void _input_internal__close_restricted(int fd, void* user_data);
static int  _input_internal__on_libinput_fd(int fd, uint32_t mask, void* data);
static void _input_internal__process_event(struct libinput_event* ev);
static void _input_internal__get_panel_size(int* w, int* h);

//
// libinput_interface: two fn-pointers for device-open + close. We
// pass this to libinput_udev_create_context. libinput invokes
// open_restricted the first time it needs a device fd and
// close_restricted when the device disappears.
//
static const struct libinput_interface _input_internal__li_iface = {
    .open_restricted  = _input_internal__open_restricted,
    .close_restricted = _input_internal__close_restricted,
};

static int _input_internal__open_restricted(const char* path, int flags, void* user_data)
{
    (void)user_data;
    int fd = open(path, flags);
    if (fd < 0)
    {
        log_warn("input: open(%s) failed: %s (this device will be unavailable)",
                 path, strerror(errno));
        return -errno;
    }
    log_trace("input: opened %s -> fd=%d", path, fd);
    return fd;
}

static void _input_internal__close_restricted(int fd, void* user_data)
{
    (void)user_data;
    close(fd);
}

static void _input_internal__get_panel_size(int* w_out, int* h_out)
{
    //
    // Prefer the actual DRM panel size when output_drm is active.
    // When running nested or headless, fall back to a sensible
    // default (1920x1080). Touch coordinates on headless won't
    // map to anything visible anyway; this is just about not
    // dividing by zero / passing garbage to libinput.
    //
    int w = 1920, h = 1080;
    output_drm__get_native_size(&w, &h);
    if (w_out != NULL) *w_out = w;
    if (h_out != NULL) *h_out = h;
}

static void _input_internal__process_event(struct libinput_event* ev)
{
    enum libinput_event_type type = libinput_event_get_type(ev);

    switch (type)
    {
    case LIBINPUT_EVENT_DEVICE_ADDED:
    {
        struct libinput_device* dev = libinput_event_get_device(ev);
        const char* name = libinput_device_get_name(dev);
        int has_touch   = libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH);
        int has_pointer = libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER);
        int has_kb      = libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD);
        log_info("input: device added \"%s\" (touch=%d pointer=%d keyboard=%d)",
                 name, has_touch, has_pointer, has_kb);
        break;
    }
    case LIBINPUT_EVENT_DEVICE_REMOVED:
    {
        struct libinput_device* dev = libinput_event_get_device(ev);
        log_info("input: device removed \"%s\"", libinput_device_get_name(dev));
        break;
    }
    case LIBINPUT_EVENT_TOUCH_DOWN:
    {
        struct libinput_event_touch* te = libinput_event_get_touch_event(ev);
        int panel_w, panel_h;
        _input_internal__get_panel_size(&panel_w, &panel_h);

        uint32_t time  = libinput_event_touch_get_time(te);
        int32_t  slot  = libinput_event_touch_get_slot(te);
        double   x     = libinput_event_touch_get_x_transformed(te, panel_w);
        double   y     = libinput_event_touch_get_y_transformed(te, panel_h);

        log_trace("input: touch_down slot=%d t=%u (%.1f, %.1f) panel=%dx%d",
                  slot, time, x, y, panel_w, panel_h);
        globals__seat__send_touch_down(time, slot, (int32_t)x, (int32_t)y);
        //
        // Also forward to the bound shell via meek_shell_v1.touch_*_raw
        // so the shell's gesture recognizer sees every touch, including
        // ones that land outside any focused surface. Safe no-op if no
        // shell is bound.
        //
        meek_shell_v1__fire_touch_down_raw(time, slot, (int32_t)x, (int32_t)y);
        break;
    }
    case LIBINPUT_EVENT_TOUCH_MOTION:
    {
        struct libinput_event_touch* te = libinput_event_get_touch_event(ev);
        int panel_w, panel_h;
        _input_internal__get_panel_size(&panel_w, &panel_h);

        uint32_t time = libinput_event_touch_get_time(te);
        int32_t  slot = libinput_event_touch_get_slot(te);
        double   x    = libinput_event_touch_get_x_transformed(te, panel_w);
        double   y    = libinput_event_touch_get_y_transformed(te, panel_h);

        log_trace("input: touch_motion slot=%d t=%u (%.1f, %.1f)", slot, time, x, y);
        globals__seat__send_touch_motion(time, slot, (int32_t)x, (int32_t)y);
        meek_shell_v1__fire_touch_motion_raw(time, slot, (int32_t)x, (int32_t)y);
        break;
    }
    case LIBINPUT_EVENT_TOUCH_UP:
    {
        struct libinput_event_touch* te = libinput_event_get_touch_event(ev);
        uint32_t time = libinput_event_touch_get_time(te);
        int32_t  slot = libinput_event_touch_get_slot(te);
        log_trace("input: touch_up slot=%d t=%u", slot, time);
        globals__seat__send_touch_up(time, slot);
        meek_shell_v1__fire_touch_up_raw(time, slot);
        break;
    }
    case LIBINPUT_EVENT_TOUCH_FRAME:
    {
        //
        // libinput groups related touch events into frames. Clients
        // typically only act on a frame boundary -- so forwarding
        // the frame event matters for input coherence.
        //
        globals__seat__send_touch_frame();
        break;
    }
    case LIBINPUT_EVENT_TOUCH_CANCEL:
    {
        log_trace("input: touch_cancel");
        globals__seat__send_touch_cancel();
        break;
    }
    default:
        //
        // Pointer / keyboard / gesture / tablet events get ignored
        // silently for now. Easy to wire later; scope creep
        // otherwise.
        //
        break;
    }
}

static int _input_internal__on_libinput_fd(int fd, uint32_t mask, void* data)
{
    (void)fd;
    (void)mask;
    (void)data;

    //
    // libinput requires an explicit dispatch call before
    // get_event can return anything. It reads from the kernel fd
    // and buffers events internally.
    //
    if (libinput_dispatch(_input_internal__li) != 0)
    {
        log_warn("input: libinput_dispatch returned error: %s", strerror(errno));
        return 0;
    }

    struct libinput_event* ev;
    while ((ev = libinput_get_event(_input_internal__li)) != NULL)
    {
        _input_internal__process_event(ev);
        libinput_event_destroy(ev);
    }
    return 0;
}

int input__init(struct wl_display* display)
{
    _input_internal__udev = udev_new();
    if (_input_internal__udev == NULL)
    {
        log_error("input: udev_new failed");
        return -1;
    }

    _input_internal__li = libinput_udev_create_context(
        &_input_internal__li_iface,
        /*user_data*/ NULL,
        _input_internal__udev);
    if (_input_internal__li == NULL)
    {
        log_error("input: libinput_udev_create_context failed");
        udev_unref(_input_internal__udev);
        _input_internal__udev = NULL;
        return -1;
    }

    //
    // libseat-managed system / direct login / sudo all end up on
    // seat0 by default; pmOS + sxmo follow this convention.
    //
    if (libinput_udev_assign_seat(_input_internal__li, "seat0") != 0)
    {
        log_error("input: libinput_udev_assign_seat(\"seat0\") failed");
        libinput_unref(_input_internal__li);
        _input_internal__li = NULL;
        udev_unref(_input_internal__udev);
        _input_internal__udev = NULL;
        return -1;
    }

    int fd = libinput_get_fd(_input_internal__li);
    struct wl_event_loop* loop = wl_display_get_event_loop(display);
    _input_internal__li_src = wl_event_loop_add_fd(
        loop, fd, WL_EVENT_READABLE,
        _input_internal__on_libinput_fd, NULL);
    if (_input_internal__li_src == NULL)
    {
        log_error("input: wl_event_loop_add_fd failed");
        libinput_unref(_input_internal__li);
        _input_internal__li = NULL;
        udev_unref(_input_internal__udev);
        _input_internal__udev = NULL;
        return -1;
    }

    //
    // Drain device-added events synchronously at startup so the
    // "input: device added ..." log lines appear near compositor
    // startup rather than at the first user touch.
    //
    _input_internal__on_libinput_fd(fd, WL_EVENT_READABLE, NULL);
    log_info("input: libinput ready on seat0 (fd=%d)", fd);
    return 0;
}

void input__shutdown(void)
{
    if (_input_internal__li_src != NULL)
    {
        wl_event_source_remove(_input_internal__li_src);
        _input_internal__li_src = NULL;
    }
    if (_input_internal__li != NULL)
    {
        libinput_unref(_input_internal__li);
        _input_internal__li = NULL;
    }
    if (_input_internal__udev != NULL)
    {
        udev_unref(_input_internal__udev);
        _input_internal__udev = NULL;
    }
    log_info("input: shutdown complete");
}
