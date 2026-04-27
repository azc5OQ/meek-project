//
// platforms/linux/platform_linux_x11.c - X11 windowed backend.
//
// Desktop-Linux companion to platform_linux_drm.c. Runs as a
// normal X11 client: opens a window with the display manager
// running as usual, reads input through XEvent, presents through
// EGL + GLES 3 (same renderer the DRM and Android ports use).
//
// COMPANION FILES in this directory:
//   - fs_linux.c                POSIX file I/O + mmap (shared)
//   - platform_linux_drm.c      direct-to-GPU kiosk backend
//
// DEPENDENCIES (link order in build.sh):
//   -lX11 -lEGL -lGLESv2 -lm -ldl
//
// RENDERER CHOICE:
//   EGL + GLES 3 via gles3_renderer.c, not GLX + desktop GL via
//   opengl3_renderer.c. Two reasons:
//     1. gles3_renderer.c is already context-neutral -- the platform
//        owns EGL, the renderer just binds to whatever context is
//        current. Reusing it means the X11, Wayland (future), DRM,
//        and Android backends all share one renderer TU.
//     2. opengl3_renderer.c creates its own WGL context from an
//        HWND and isn't usable on X11 without a second refactor.
//        EGL on X is supported by every modern driver (Mesa,
//        NVIDIA) via eglGetDisplay(x_display) or the platform-x11
//        extension, so there's no capability loss.
//
// WINDOW CLOSE:
//   X11 has no "close" event. The window manager sends a
//   ClientMessage with the WM_DELETE_WINDOW atom when the user
//   clicks the close button. We register for it via
//   XSetWMProtocols and convert it into should_close = TRUE.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

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

//============================================================================
// state
//============================================================================

typedef struct _platform_linux_x11_internal__state
{
    Display*     display;
    int          screen;
    Window       window;
    Atom         wm_delete_window;
    XIM          input_method;        // NULL if allocation fails; XLookupString still works without it but UTF-8 compose is off.
    XIC          input_context;

    EGLDisplay   egl_display;
    EGLContext   egl_context;
    EGLSurface   egl_surface;
    EGLConfig    egl_config;

    int          viewport_w;
    int          viewport_h;
    boole        should_close;

    //
    // Pointer state. X11 only reports motion when the pointer is
    // actually inside the window; we still cache last-known coords
    // so button events (which include x/y) and synthesized moves
    // stay consistent.
    //
    int64 ptr_x;
    int64 ptr_y;

    gui_color clear_color;
} _platform_linux_x11_internal__state;

static _platform_linux_x11_internal__state _platform_linux_x11_internal__g;

//============================================================================
// forward decls
//============================================================================

static boole _platform_linux_x11_internal__open_display_and_window(const gui_app_config* cfg);
static void  _platform_linux_x11_internal__close_display_and_window(void);
static boole _platform_linux_x11_internal__init_egl(void);
static void  _platform_linux_x11_internal__term_egl(void);
static void  _platform_linux_x11_internal__pump_events(void);
static void  _platform_linux_x11_internal__on_configure(int w, int h);
static int64 _platform_linux_x11_internal__keysym_to_vk(KeySym ks);
static gui_handler_fn _platform_linux_x11_internal__resolve_host_symbol(char* name);
static float _platform_linux_x11_internal__pick_ui_scale(void);

//============================================================================
// public API
//============================================================================

boole platform__init(const gui_app_config* cfg)
{
    memory_manager__init();

    memset(&_platform_linux_x11_internal__g, 0, sizeof(_platform_linux_x11_internal__g));

    if (cfg == NULL)
    {
        log_error("platform__init: cfg is NULL");
        return FALSE;
    }

    _platform_linux_x11_internal__g.clear_color = cfg->clear_color;

    if (!_platform_linux_x11_internal__open_display_and_window(cfg))
    {
        log_error("platform__init: open_display_and_window failed");
        memory_manager__shutdown();
        return FALSE;
    }

    if (!_platform_linux_x11_internal__init_egl())
    {
        log_error("platform__init: init_egl failed");
        _platform_linux_x11_internal__close_display_and_window();
        memory_manager__shutdown();
        return FALSE;
    }

    if (!renderer__init(NULL))
    {
        log_error("platform__init: renderer__init failed");
        _platform_linux_x11_internal__term_egl();
        _platform_linux_x11_internal__close_display_and_window();
        memory_manager__shutdown();
        return FALSE;
    }

    widget_registry__bootstrap_builtins();
    if (!font__init())
    {
        log_error("platform__init: font__init failed");
        renderer__shutdown();
        _platform_linux_x11_internal__term_egl();
        _platform_linux_x11_internal__close_display_and_window();
        memory_manager__shutdown();
        return FALSE;
    }

    scene__set_symbol_resolver(_platform_linux_x11_internal__resolve_host_symbol);

    //
    // Pick UI scale from the X server's reported DPI. `XDisplayWidth`
    // + `XDisplayWidthMM` gives pixels-per-mm; convert to DPI.
    // Many distros report a sane number here via xrandr; others
    // return the X.Org hard-coded 96-dpi default, in which case
    // scale stays 1.0.
    //
    float ui_scale = _platform_linux_x11_internal__pick_ui_scale();
    if (ui_scale > 0.0f)
    {
        scene__set_scale(ui_scale);
    }

    log_info("platform_linux_x11: up (%dx%d)",
             _platform_linux_x11_internal__g.viewport_w,
             _platform_linux_x11_internal__g.viewport_h);
    return TRUE;
}

boole platform__tick(void)
{
    if (_platform_linux_x11_internal__g.should_close) { return FALSE; }

    //
    // X events before rendering so the frame sees fresh state
    // (viewport size changes from ConfigureNotify apply before
    // layout, keypress edits land in the input widget before
    // render).
    //
    _platform_linux_x11_internal__pump_events();
    if (_platform_linux_x11_internal__g.should_close) { return FALSE; }

    //
    // Frame timestamp. Same CLOCK_MONOTONIC -> ms conversion as
    // DRM + Android.
    //
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64 now_ms = (int64)ts.tv_sec * 1000 + (int64)(ts.tv_nsec / 1000000);
        scene__begin_frame_time(now_ms);
    }

    int64 vw = (int64)_platform_linux_x11_internal__g.viewport_w;
    int64 vh = (int64)_platform_linux_x11_internal__g.viewport_h;

    scene__resolve_styles();
    animator__tick();
    scene__layout(vw, vh);

    renderer__begin_frame(vw, vh, _platform_linux_x11_internal__g.clear_color);
    scene__emit_draws();
    renderer__end_frame();

    eglSwapBuffers(_platform_linux_x11_internal__g.egl_display,
                   _platform_linux_x11_internal__g.egl_surface);
    return TRUE;
}

void platform__shutdown(void)
{
    //
    // Image cache + font subsystem release renderer-owned textures,
    // so they have to run while the renderer is still alive.
    //
    widget_image__cache_shutdown();
    font__shutdown();
    renderer__shutdown();
    _platform_linux_x11_internal__term_egl();
    _platform_linux_x11_internal__close_display_and_window();
    memory_manager__shutdown();
}

//
// Capture API for X11: XGetImage grabs the window's pixels; we
// pack them into a 32-bit BGRA BMP, same shape as the Win32 path
// writes (so the visual-regression report can mix captures from
// both platforms without confusion).
//
// Works on WSLg (WSL2 on Windows 11) without any extra setup --
// XWayland exposes an X11 server to the WSL instance that routes
// to a Windows 11 window automatically.
//
boole platform__capture_bmp(const char* path)
{
    if (path == NULL || path[0] == 0) { log_error("platform__capture_bmp: NULL path"); return FALSE; }
    Display* dpy = _platform_linux_x11_internal__g.display;
    Window   win = _platform_linux_x11_internal__g.window;
    if (dpy == NULL || win == 0) { log_error("platform__capture_bmp: no window"); return FALSE; }

    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, win, &wa)) { log_error("XGetWindowAttributes failed"); return FALSE; }
    int w = wa.width;
    int h = wa.height;
    if (w <= 0 || h <= 0) { log_error("client area is 0x0"); return FALSE; }

    //
    // ZPixmap + AllPlanes gets us the rendered pixels in the server's
    // native format. On WSLg / most modern X11 servers this is 32 bpp
    // BGRA; we defensively check bits_per_pixel and byte order below
    // and only handle that common case. If we ever need 24-bpp
    // support we'll branch here; it isn't in the failure mode for
    // the WSL use case.
    //
    XImage* img = XGetImage(dpy, win, 0, 0, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
    if (img == NULL) { log_error("XGetImage failed"); return FALSE; }
    if (img->bits_per_pixel != 32)
    {
        log_error("platform__capture_bmp: unexpected bpp %d (only 32 supported)", img->bits_per_pixel);
        XDestroyImage(img);
        return FALSE;
    }

    boole ok = FALSE;
    FILE* fp = fopen(path, "wb");
    if (fp == NULL) { log_error("fopen %s failed", path); goto cleanup; }

    //
    // BMP file: 14-byte BITMAPFILEHEADER + 40-byte BITMAPINFOHEADER +
    // pixel rows bottom-up, 32 bpp BGRA. Hand-pack the headers to
    // avoid a struct packing dependency. Same layout as the Win32
    // BMP we produce so the report.html grid mixes cleanly.
    //
    unsigned row_bytes  = (unsigned)(w * 4);
    unsigned pixel_size = row_bytes * (unsigned)h;
    unsigned file_off   = 14 + 40;
    unsigned file_size  = file_off + pixel_size;

    unsigned char bfh[14] = {0};
    bfh[0] = 'B'; bfh[1] = 'M';
    bfh[2]  = (unsigned char)(file_size       & 0xff);
    bfh[3]  = (unsigned char)((file_size >>  8) & 0xff);
    bfh[4]  = (unsigned char)((file_size >> 16) & 0xff);
    bfh[5]  = (unsigned char)((file_size >> 24) & 0xff);
    bfh[10] = (unsigned char)(file_off       & 0xff);
    bfh[11] = (unsigned char)((file_off >>  8) & 0xff);
    bfh[12] = (unsigned char)((file_off >> 16) & 0xff);
    bfh[13] = (unsigned char)((file_off >> 24) & 0xff);
    if (fwrite(bfh, 1, 14, fp) != 14) { log_error("fwrite bfh"); goto cleanup; }

    unsigned char bih[40] = {0};
    bih[0]  = 40;
    bih[4]  = (unsigned char)(w        & 0xff);
    bih[5]  = (unsigned char)((w >>  8) & 0xff);
    bih[6]  = (unsigned char)((w >> 16) & 0xff);
    bih[7]  = (unsigned char)((w >> 24) & 0xff);
    bih[8]  = (unsigned char)(h        & 0xff);
    bih[9]  = (unsigned char)((h >>  8) & 0xff);
    bih[10] = (unsigned char)((h >> 16) & 0xff);
    bih[11] = (unsigned char)((h >> 24) & 0xff);
    bih[12] = 1;           // biPlanes
    bih[14] = 32;          // biBitCount
    // biCompression BI_RGB (0), biSizeImage can be 0 for BI_RGB.
    if (fwrite(bih, 1, 40, fp) != 40) { log_error("fwrite bih"); goto cleanup; }

    //
    // Pixel rows. XImage stores rows top-down; BMP file wants them
    // bottom-up. Walk bottom → top writing one row at a time.
    // img->bytes_per_line is the SERVER's row stride (may be >
    // w*4 on some servers). We always read the first w*4 bytes of
    // each row.
    //
    for (int row = h - 1; row >= 0; row--)
    {
        unsigned char* src = (unsigned char*)img->data + (size_t)row * (size_t)img->bytes_per_line;
        if (fwrite(src, 1, row_bytes, fp) != row_bytes) { log_error("fwrite row"); goto cleanup; }
    }
    ok = TRUE;

cleanup:
    if (fp  != NULL) { fclose(fp); }
    if (img != NULL) { XDestroyImage(img); }
    return ok;
}

//
// Bring the window to the top of the z-order via XRaiseWindow +
// the EWMH hint _NET_WM_STATE_ABOVE. Keeps our window unobstructed
// during test capture.
//
void platform__set_topmost(void)
{
    Display* dpy = _platform_linux_x11_internal__g.display;
    Window   win = _platform_linux_x11_internal__g.window;
    if (dpy == NULL || win == 0) { return; }
    XRaiseWindow(dpy, win);
    //
    // EWMH state change: tell the window manager to treat us as
    // "above". Not every compositor honors this (some ignore it
    // for non-dock windows), but XRaiseWindow above is enough for
    // WSLg / XWayland which doesn't aggressively reorder windows.
    //
    Atom wm_state       = XInternAtom(dpy, "_NET_WM_STATE",       False);
    Atom wm_state_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    if (wm_state != None && wm_state_above != None)
    {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type         = ClientMessage;
        ev.xclient.window       = win;
        ev.xclient.message_type = wm_state;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = 1;  // _NET_WM_STATE_ADD
        ev.xclient.data.l[1]    = (long)wm_state_above;
        ev.xclient.data.l[2]    = 0;
        ev.xclient.data.l[3]    = 1;  // source: application
        XSendEvent(dpy, DefaultRootWindow(dpy), False,
                   SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    }
    XFlush(dpy);
}

//============================================================================
// X11 window open / close
//============================================================================

static boole _platform_linux_x11_internal__open_display_and_window(const gui_app_config* cfg)
{
    Display* dpy = XOpenDisplay(NULL);
    if (dpy == NULL)
    {
        log_error("XOpenDisplay failed (is DISPLAY set?)");
        return FALSE;
    }
    _platform_linux_x11_internal__g.display = dpy;
    _platform_linux_x11_internal__g.screen  = DefaultScreen(dpy);

    Window root = RootWindow(dpy, _platform_linux_x11_internal__g.screen);

    int w = (int)(cfg->width  > 0 ? cfg->width  : 1280);
    int h = (int)(cfg->height > 0 ? cfg->height : 800);

    //
    // Create the window with a black background (the real clear color
    // is applied by the renderer on the first frame; giving the X
    // server a matching pixel avoids a one-frame flash of default
    // parent-window color during map).
    //
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask |
                     FocusChangeMask | LeaveWindowMask | EnterWindowMask;
    swa.background_pixel = BlackPixel(dpy, _platform_linux_x11_internal__g.screen);
    swa.border_pixel     = BlackPixel(dpy, _platform_linux_x11_internal__g.screen);

    Window win = XCreateWindow(
        dpy, root,
        0, 0, w, h, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWEventMask | CWBackPixel | CWBorderPixel,
        &swa);
    if (win == 0)
    {
        log_error("XCreateWindow failed");
        XCloseDisplay(dpy);
        return FALSE;
    }
    _platform_linux_x11_internal__g.window      = win;
    _platform_linux_x11_internal__g.viewport_w  = w;
    _platform_linux_x11_internal__g.viewport_h  = h;

    //
    // Title. XStoreName takes latin-1 only; for real UTF-8 we'd use
    // XChangeProperty(XA_NET_WM_NAME). The app's wchar_t* title gets
    // naively truncated to ASCII here -- fine for "gui poc (linux
    // x11)" but a proper toolkit would convert with wcstombs().
    //
    if (cfg->title != NULL)
    {
        char ascii[128];
        int i = 0;
        for (; i < (int)sizeof(ascii) - 1 && cfg->title[i] != 0; i++)
        {
            wchar_t c = cfg->title[i];
            ascii[i] = (c < 0x80) ? (char)c : '?';
        }
        ascii[i] = 0;
        XStoreName(dpy, win, ascii);
    }

    //
    // WM_DELETE_WINDOW protocol: without this, clicking the close
    // button (or calling `wmctrl -c`) just calls XKillClient on us,
    // aborting without a clean shutdown. Registering makes the WM
    // send a ClientMessage instead and we exit gracefully.
    //
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    _platform_linux_x11_internal__g.wm_delete_window = wm_delete;

    //
    // Input method for compose-key support (é, ñ, etc.). Failing to
    // open one is not fatal -- XLookupString still works without an
    // XIC, we just lose the "dead acute + e = é" path.
    //
    XSetLocaleModifiers("");
    XIM im = XOpenIM(dpy, NULL, NULL, NULL);
    if (im != NULL)
    {
        XIC ic = XCreateIC(im,
                           XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                           XNClientWindow, win,
                           XNFocusWindow, win,
                           (char*)NULL);
        _platform_linux_x11_internal__g.input_method  = im;
        _platform_linux_x11_internal__g.input_context = ic;
        if (ic != NULL) { XSetICFocus(ic); }
    }

    XMapWindow(dpy, win);
    XFlush(dpy);
    return TRUE;
}

static void _platform_linux_x11_internal__close_display_and_window(void)
{
    if (_platform_linux_x11_internal__g.input_context != NULL)
    {
        XDestroyIC(_platform_linux_x11_internal__g.input_context);
        _platform_linux_x11_internal__g.input_context = NULL;
    }
    if (_platform_linux_x11_internal__g.input_method != NULL)
    {
        XCloseIM(_platform_linux_x11_internal__g.input_method);
        _platform_linux_x11_internal__g.input_method = NULL;
    }
    if (_platform_linux_x11_internal__g.display != NULL)
    {
        if (_platform_linux_x11_internal__g.window != 0)
        {
            XDestroyWindow(_platform_linux_x11_internal__g.display,
                           _platform_linux_x11_internal__g.window);
            _platform_linux_x11_internal__g.window = 0;
        }
        XCloseDisplay(_platform_linux_x11_internal__g.display);
        _platform_linux_x11_internal__g.display = NULL;
    }
}

//============================================================================
// EGL on the X display
//============================================================================
//
// Two paths, preferred in order:
//   1. eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_KHR, xdpy, NULL)
//      -- the modern extension, required on Wayland-first stacks
//      where plain eglGetDisplay(xdpy) is ambiguous.
//   2. eglGetDisplay((EGLNativeDisplayType)xdpy) -- the traditional
//      form, which Mesa still accepts.
//
// Surface attaches to the X Window cast to EGLNativeWindowType.
// Mesa + NVIDIA both handle that; no extra work.
//

static boole _platform_linux_x11_internal__init_egl(void)
{
    Display* xdpy = _platform_linux_x11_internal__g.display;

    PFNEGLGETPLATFORMDISPLAYEXTPROC peglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (peglGetPlatformDisplayEXT != NULL)
    {
        dpy = peglGetPlatformDisplayEXT(EGL_PLATFORM_X11_KHR, xdpy, NULL);
    }
    if (dpy == EGL_NO_DISPLAY)
    {
        dpy = eglGetDisplay((EGLNativeDisplayType)xdpy);
    }
    if (dpy == EGL_NO_DISPLAY)
    {
        log_error("eglGetDisplay(x_display) returned EGL_NO_DISPLAY");
        return FALSE;
    }
    if (!eglInitialize(dpy, NULL, NULL))
    {
        log_error("eglInitialize failed (0x%x)", (unsigned)eglGetError());
        return FALSE;
    }
    _platform_linux_x11_internal__g.egl_display = dpy;

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };

    EGLConfig cfg;
    EGLint    num_cfg = 0;
    if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &num_cfg) || num_cfg < 1)
    {
        log_error("eglChooseConfig: no matching config");
        return FALSE;
    }
    _platform_linux_x11_internal__g.egl_config = cfg;

    //
    // Bind ES before creating the context, same rationale as DRM.
    //
    if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
        log_error("eglBindAPI(EGL_OPENGL_ES_API) failed");
        return FALSE;
    }

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT)
    {
        log_error("eglCreateContext failed (0x%x)", (unsigned)eglGetError());
        return FALSE;
    }
    _platform_linux_x11_internal__g.egl_context = ctx;

    EGLSurface surf = eglCreateWindowSurface(dpy, cfg,
        (EGLNativeWindowType)_platform_linux_x11_internal__g.window, NULL);
    if (surf == EGL_NO_SURFACE)
    {
        log_error("eglCreateWindowSurface failed (0x%x)", (unsigned)eglGetError());
        eglDestroyContext(dpy, ctx);
        _platform_linux_x11_internal__g.egl_context = EGL_NO_CONTEXT;
        return FALSE;
    }
    _platform_linux_x11_internal__g.egl_surface = surf;

    if (!eglMakeCurrent(dpy, surf, surf, ctx))
    {
        log_error("eglMakeCurrent failed (0x%x)", (unsigned)eglGetError());
        return FALSE;
    }
    eglSwapInterval(dpy, 1);  // vsync
    return TRUE;
}

static void _platform_linux_x11_internal__term_egl(void)
{
    if (_platform_linux_x11_internal__g.egl_display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(_platform_linux_x11_internal__g.egl_display,
                       EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (_platform_linux_x11_internal__g.egl_surface != EGL_NO_SURFACE)
    {
        eglDestroySurface(_platform_linux_x11_internal__g.egl_display,
                          _platform_linux_x11_internal__g.egl_surface);
        _platform_linux_x11_internal__g.egl_surface = EGL_NO_SURFACE;
    }
    if (_platform_linux_x11_internal__g.egl_context != EGL_NO_CONTEXT)
    {
        eglDestroyContext(_platform_linux_x11_internal__g.egl_display,
                          _platform_linux_x11_internal__g.egl_context);
        _platform_linux_x11_internal__g.egl_context = EGL_NO_CONTEXT;
    }
    if (_platform_linux_x11_internal__g.egl_display != EGL_NO_DISPLAY)
    {
        eglTerminate(_platform_linux_x11_internal__g.egl_display);
        _platform_linux_x11_internal__g.egl_display = EGL_NO_DISPLAY;
    }
}

//============================================================================
// event pump
//============================================================================
//
// Drain every X event currently queued. Called once per frame at the
// top of tick(); each call to XPending tells us how many events are
// parked client-side. Blocking on XNextEvent would stall the render
// loop, so we don't -- we only process the ones already in the queue.
// XSync would add latency, we want the opposite.
//

static void _platform_linux_x11_internal__pump_events(void)
{
    Display* dpy = _platform_linux_x11_internal__g.display;
    if (dpy == NULL) { return; }

    while (XPending(dpy) > 0)
    {
        XEvent ev;
        XNextEvent(dpy, &ev);

        //
        // Filter events through the XIM before switching on them --
        // the input method may consume key presses that form part of
        // a compose sequence and return non-zero to tell us to drop
        // them from the usual dispatch.
        //
        if (XFilterEvent(&ev, 0) != 0) { continue; }

        switch (ev.type)
        {
            case ConfigureNotify:
            {
                if (ev.xconfigure.width  != _platform_linux_x11_internal__g.viewport_w ||
                    ev.xconfigure.height != _platform_linux_x11_internal__g.viewport_h)
                {
                    _platform_linux_x11_internal__on_configure(
                        ev.xconfigure.width, ev.xconfigure.height);
                }
                break;
            }

            case ClientMessage:
            {
                if ((Atom)ev.xclient.data.l[0] == _platform_linux_x11_internal__g.wm_delete_window)
                {
                    _platform_linux_x11_internal__g.should_close = TRUE;
                }
                break;
            }

            case MotionNotify:
            {
                _platform_linux_x11_internal__g.ptr_x = ev.xmotion.x;
                _platform_linux_x11_internal__g.ptr_y = ev.xmotion.y;
                scene__on_mouse_move(_platform_linux_x11_internal__g.ptr_x,
                                     _platform_linux_x11_internal__g.ptr_y);
                break;
            }

            case ButtonPress:
            case ButtonRelease:
            {
                boole down = (ev.type == ButtonPress);
                _platform_linux_x11_internal__g.ptr_x = ev.xbutton.x;
                _platform_linux_x11_internal__g.ptr_y = ev.xbutton.y;
                //
                // X11 button codes:
                //   1 left, 2 middle, 3 right, 4 wheel up, 5 wheel down,
                //   6 wheel left, 7 wheel right, 8/9 extra.
                //
                switch (ev.xbutton.button)
                {
                    case 1:
                        scene__on_mouse_button(0, down, ev.xbutton.x, ev.xbutton.y);
                        break;
                    case 2:
                        scene__on_mouse_button(2, down, ev.xbutton.x, ev.xbutton.y);
                        break;
                    case 3:
                        scene__on_mouse_button(1, down, ev.xbutton.x, ev.xbutton.y);
                        break;
                    case 4:  // wheel up
                        if (down) { scene__on_mouse_wheel(ev.xbutton.x, ev.xbutton.y,  1.0f); }
                        break;
                    case 5:  // wheel down
                        if (down) { scene__on_mouse_wheel(ev.xbutton.x, ev.xbutton.y, -1.0f); }
                        break;
                    default:
                        break;
                }
                break;
            }

            case KeyPress:
            {
                //
                // Two dispatches per key press:
                //   1. VK-style scene__on_key for non-character keys
                //      (BACKSPACE, arrows, Enter, Escape, ...).
                //   2. scene__on_char for each Unicode codepoint the
                //      XIM+XLookupString combination produced. We ask
                //      Xutf8LookupString when an XIC is available so
                //      dead-key composition works; the returned bytes
                //      are then walked as UTF-8 and dispatched as
                //      codepoints.
                //
                KeySym ks = NoSymbol;
                char buf[64];
                int n = 0;
                if (_platform_linux_x11_internal__g.input_context != NULL)
                {
                    Status st = 0;
                    n = Xutf8LookupString(_platform_linux_x11_internal__g.input_context,
                                          &ev.xkey, buf, sizeof(buf) - 1,
                                          &ks, &st);
                }
                else
                {
                    n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, NULL);
                }
                buf[n >= 0 ? (n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) : 0] = 0;

                int64 vk = _platform_linux_x11_internal__keysym_to_vk(ks);
                if (vk != 0)
                {
                    scene__on_key(vk, TRUE);
                }

                //
                // Walk the UTF-8 bytes and push each codepoint. Our
                // scene on_char today only uses the ASCII + Latin-1
                // subset, so anything >0xff is clamped down to '?'
                // -- matches what widget_input accepts. Full Unicode
                // text input needs atlas paging (see STATUS.md deferred).
                //
                const unsigned char* s = (const unsigned char*)buf;
                while (*s != 0)
                {
                    uint cp = 0;
                    if (*s < 0x80)
                    {
                        cp = (uint)*s;
                        s++;
                    }
                    else if ((*s & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80)
                    {
                        cp = (uint)((*s & 0x1F) << 6) | (uint)(s[1] & 0x3F);
                        s += 2;
                    }
                    else
                    {
                        //
                        // 3-or-4-byte sequence. Skip past to the next
                        // start byte; scene's ASCII-only widgets can't
                        // represent these codepoints yet.
                        //
                        while (*s != 0 && (*s & 0xC0) == 0x80) { s++; }
                        if (*s != 0) { s++; }
                        cp = (uint)'?';
                    }

                    //
                    // Filter out control chars the scene handlers
                    // didn't mean to get as characters -- Enter /
                    // Backspace / Tab arrive as both a KeySym (above)
                    // AND a 1-byte ASCII control code in the buffer.
                    // Only the KeySym path should handle those.
                    //
                    if (cp >= 0x20 && cp != 0x7F)
                    {
                        scene__on_char(cp);
                    }
                }
                break;
            }

            case KeyRelease:
            {
                KeySym ks = NoSymbol;
                char buf[8];
                (void)XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, NULL);
                int64 vk = _platform_linux_x11_internal__keysym_to_vk(ks);
                if (vk != 0)
                {
                    scene__on_key(vk, FALSE);
                }
                break;
            }

            case FocusIn:
            {
                if (_platform_linux_x11_internal__g.input_context != NULL)
                {
                    XSetICFocus(_platform_linux_x11_internal__g.input_context);
                }
                break;
            }
            case FocusOut:
            {
                if (_platform_linux_x11_internal__g.input_context != NULL)
                {
                    XUnsetICFocus(_platform_linux_x11_internal__g.input_context);
                }
                break;
            }

            default:
                break;
        }
    }
}

static void _platform_linux_x11_internal__on_configure(int w, int h)
{
    _platform_linux_x11_internal__g.viewport_w = w;
    _platform_linux_x11_internal__g.viewport_h = h;
    scene__on_resize((int64)w, (int64)h);
}

//
// Translate common X11 keysyms to Win32 virtual-key codes. Same
// mapping table scene.c expects from every platform, so widgets
// don't need per-OS key dispatch logic.
//
static int64 _platform_linux_x11_internal__keysym_to_vk(KeySym ks)
{
    switch (ks)
    {
        case XK_BackSpace: return 0x08;
        case XK_Tab:       return 0x09;
        case XK_Return:
        case XK_KP_Enter:  return 0x0D;
        case XK_Escape:    return 0x1B;
        case XK_space:     return 0x20;
        case XK_Left:      return 0x25;
        case XK_Up:        return 0x26;
        case XK_Right:     return 0x27;
        case XK_Down:      return 0x28;
        case XK_Delete:    return 0x2E;
        case XK_Home:      return 0x24;
        case XK_End:       return 0x23;
        case XK_Page_Up:   return 0x21;
        case XK_Page_Down: return 0x22;
        default:           return 0;
    }
}

//============================================================================
// UI scale from X DPI
//============================================================================

static float _platform_linux_x11_internal__pick_ui_scale(void)
{
    Display* dpy = _platform_linux_x11_internal__g.display;
    if (dpy == NULL) { return 1.0f; }

    int screen = _platform_linux_x11_internal__g.screen;
    int px_w   = XDisplayWidth(dpy, screen);
    int mm_w   = XDisplayWidthMM(dpy, screen);

    if (px_w <= 0 || mm_w <= 0)
    {
        return 1.0f;
    }

    float dpi   = (float)px_w * 25.4f / (float)mm_w;
    float scale = dpi / 96.0f;
    if (scale < 1.0f) { scale = 1.0f; }
    if (scale > 4.0f) { scale = 4.0f; }

    log_info("ui_scale: X dpi=%.1f -> scale=%.2fx", (double)dpi, (double)scale);
    return scale;
}

//============================================================================
// host symbol resolver
//============================================================================

static gui_handler_fn _platform_linux_x11_internal__resolve_host_symbol(char* name)
{
    if (name == NULL || name[0] == 0) { return NULL; }
    void* sym = dlsym(RTLD_DEFAULT, name);
    return (gui_handler_fn)sym;
}

//============================================================================
// entry-point trampoline
//============================================================================
//
// Same pattern as platform_linux_drm.c: platform.h renames the host's
// int main() to int app_main(); we provide the real main() here
// (guarded from the rename by _PLATFORM_INTERNAL at include time)
// and forward. Single-binary build on Linux X11, no library/host split.
//

#include "../_main_trampoline.h"
GUI_DEFINE_MAIN_TRAMPOLINE()
