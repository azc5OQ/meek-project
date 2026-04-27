//
//output_x11.c - nested X11 output backend.
//
//Opens a regular-looking window on the host X session, gets an
//EGL rendering context on it, and drives a ~60Hz render tick from
//the wl_event_loop. Temporary: gutted in pass C10 when the real
//DRM/KMS backend comes online.
//
//DESIGN NOTE: this module owns an ENTIRELY SEPARATE EGL display
//from egl_ctx.c. egl_ctx is on an EGL_PLATFORM_GBM_KHR display
//(used for importing client dmabufs via eglCreateImage). We use
//EGL_PLATFORM_X11_EXT (needed for eglCreateWindowSurface against
//an X11 Window). EGL forbids sharing resources across displays --
//so these two contexts are independent. Calling eglMakeCurrent
//switches whichever one is active on this thread, and each render
//tick below has to switch to ours.
//
//TEXTURE-SHARING CONSEQUENCE: client dmabuf textures live in
//egl_ctx's context. This module can't sample from them. In A4
//that's fine because we only glClear + swap. When A5+ wants to
//draw client windows, we'll either (a) move egl_ctx to X11
//platform too so a single context handles both, or (b) pipe
//client buffers through dmabuf re-export + re-import into this
//context. Decision deferred.
//

#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <wayland-server-core.h>

#include "types.h"
#include "third_party/log.h"
#include "output_x11.h"

//
//module-level state. Owned start-to-finish by this TU.
//
static struct wl_display*      _output_x11_internal__wl_display = NULL;
static Display*                _output_x11_internal__x11_display = NULL;
static Window                  _output_x11_internal__x11_window  = 0;
static Atom                    _output_x11_internal__wm_delete   = None;
static int                     _output_x11_internal__width       = 0;
static int                     _output_x11_internal__height      = 0;

static EGLDisplay              _output_x11_internal__egl_display = EGL_NO_DISPLAY;
static EGLContext              _output_x11_internal__egl_context = EGL_NO_CONTEXT;
static EGLSurface              _output_x11_internal__egl_surface = EGL_NO_SURFACE;

static struct wl_event_source* _output_x11_internal__render_timer = NULL;
static struct wl_event_source* _output_x11_internal__xconn_source = NULL;

//
//forward decls for file-local statics
//
static int  _output_x11_internal__on_render_tick(void* data);
static int  _output_x11_internal__on_xconn_readable(int fd, uint32_t mask, void* data);
static void _output_x11_internal__drain_xevents(void);
static int  _output_x11_internal__make_current(void);

//
//Switches the current thread's EGL current context to ours. Needs
//to run at the top of any function that makes GL calls targeting
//our output, because egl_ctx's context may have been made-current
//since our last tick.
//
static int _output_x11_internal__make_current(void)
{
    if (!eglMakeCurrent(_output_x11_internal__egl_display,
                        _output_x11_internal__egl_surface,
                        _output_x11_internal__egl_surface,
                        _output_x11_internal__egl_context))
    {
        log_error("output_x11: eglMakeCurrent failed (0x%x)", eglGetError());
        return -1;
    }
    return 0;
}

//
//Drain any pending X events. In A4 we only care about
//WM_DELETE_WINDOW (user closed the window -> terminate
//compositor) and ConfigureNotify (window resized -> update GL
//viewport). Expose / keypress / etc. are ignored for now; real
//input will come from wl_seat forwarding, not the host X server.
//
static void _output_x11_internal__drain_xevents(void)
{
    while (XPending(_output_x11_internal__x11_display))
    {
        XEvent ev;
        XNextEvent(_output_x11_internal__x11_display, &ev);
        switch (ev.type)
        {
            case ClientMessage:
                //
                //WM sending us WM_DELETE_WINDOW when user clicks
                //the close button. Convention -- we subscribe via
                //XSetWMProtocols.
                //
                if ((Atom)ev.xclient.data.l[0] == _output_x11_internal__wm_delete)
                {
                    log_info("output_x11: window closed by WM; terminating compositor");
                    wl_display_terminate(_output_x11_internal__wl_display);
                }
                break;

            case ConfigureNotify:
                if (ev.xconfigure.width  != _output_x11_internal__width ||
                    ev.xconfigure.height != _output_x11_internal__height)
                {
                    _output_x11_internal__width  = ev.xconfigure.width;
                    _output_x11_internal__height = ev.xconfigure.height;
                    log_info("output_x11: window resized to %dx%d",
                             _output_x11_internal__width,
                             _output_x11_internal__height);
                }
                break;

            default:
                break;
        }
    }
}

//
//wl_event_loop fd callback. Fires when the X11 connection fd has
//data to read -- which means X events are queued. Drains them.
//Registered because without it we'd only notice X events at
//render-tick time, which means WM_DELETE_WINDOW could sit for up
//to ~16ms.
//
static int _output_x11_internal__on_xconn_readable(int fd, uint32_t mask, void* data)
{
    (void)fd; (void)mask; (void)data;
    _output_x11_internal__drain_xevents();
    return 0;
}

//
//Render tick. Runs at ~60Hz via wl_event_source_timer. Drains X,
//clears to a diagnostic color, swaps. Re-arms for the next tick.
//
static int _output_x11_internal__on_render_tick(void* data)
{
    (void)data;

    _output_x11_internal__drain_xevents();

    if (_output_x11_internal__make_current() != 0)
    {
        //
        //If we can't make our context current, we can't render.
        //Don't re-arm -- silent failure is worse than a visibly
        //frozen window. Compositor keeps serving protocol.
        //
        return 0;
    }

    glViewport(0, 0,
               _output_x11_internal__width,
               _output_x11_internal__height);

    //
    //A4 diagnostic clear. Blue-ish gray so it's visually obvious
    //we're rendering (and distinguishable from the default X
    //root-window colors). A5 replaces this with the client-quad
    //render loop.
    //
    glClearColor(0.10f, 0.18f, 0.32f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!eglSwapBuffers(_output_x11_internal__egl_display,
                        _output_x11_internal__egl_surface))
    {
        log_error("output_x11: eglSwapBuffers failed (0x%x)", eglGetError());
    }

    //
    //16 ms ~ 60 Hz. This is a cheap timer scheduler; real
    //presentation-time scheduling waits on an X sync event, which
    //A5 might wire in. For now we just spin.
    //
    wl_event_source_timer_update(_output_x11_internal__render_timer, 16);
    return 0;
}

int output_x11__init(struct wl_display* display, int width, int height)
{
    _output_x11_internal__wl_display = display;
    _output_x11_internal__width  = width;
    _output_x11_internal__height = height;

    //
    //1. X11 connection. NULL means $DISPLAY (inherited from the
    //parent shell). If that's unset (running under a pure tty)
    //the open fails cleanly and we log-and-skip.
    //
    _output_x11_internal__x11_display = XOpenDisplay(NULL);
    if (_output_x11_internal__x11_display == NULL)
    {
        log_error("output_x11: XOpenDisplay failed ($DISPLAY=%s)",
                  getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return -1;
    }

    //
    //2. Create + map a window on the default screen's root. No
    //attribute mask trickery; we want a plain ~1080p viewport. The
    //X server picks the window's visual (usually 24-bit TrueColor);
    //EGL asks for a matching config below.
    //
    Window root = DefaultRootWindow(_output_x11_internal__x11_display);
    _output_x11_internal__x11_window = XCreateSimpleWindow(
        _output_x11_internal__x11_display,
        root,
        /*x*/ 0, /*y*/ 0,
        width, height,
        /*border_width*/ 0,
        /*border*/ 0,
        /*background*/ 0);
    if (_output_x11_internal__x11_window == 0)
    {
        log_error("output_x11: XCreateSimpleWindow failed");
        output_x11__shutdown();
        return -1;
    }
    XStoreName(_output_x11_internal__x11_display,
               _output_x11_internal__x11_window,
               "meek-compositor (nested)");

    //
    //3. Subscribe to WM_DELETE_WINDOW so "close window" doesn't
    //kill us uncleanly and event types we care about.
    //
    _output_x11_internal__wm_delete = XInternAtom(
        _output_x11_internal__x11_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(_output_x11_internal__x11_display,
                    _output_x11_internal__x11_window,
                    &_output_x11_internal__wm_delete, 1);
    XSelectInput(_output_x11_internal__x11_display,
                 _output_x11_internal__x11_window,
                 StructureNotifyMask | ExposureMask);

    XMapWindow(_output_x11_internal__x11_display,
               _output_x11_internal__x11_window);
    XFlush(_output_x11_internal__x11_display);

    //
    //4. EGL display on the X11 platform. Uses the extension
    //entry point because the enum EGL_PLATFORM_X11_EXT isn't in
    //the base ABI.
    //
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display == NULL)
    {
        log_error("output_x11: eglGetPlatformDisplayEXT not resolvable");
        output_x11__shutdown();
        return -1;
    }
    _output_x11_internal__egl_display = get_platform_display(
        EGL_PLATFORM_X11_EXT,
        _output_x11_internal__x11_display,
        NULL);
    if (_output_x11_internal__egl_display == EGL_NO_DISPLAY)
    {
        log_error("output_x11: eglGetPlatformDisplayEXT(X11) returned EGL_NO_DISPLAY");
        output_x11__shutdown();
        return -1;
    }

    EGLint egl_major = 0, egl_minor = 0;
    if (!eglInitialize(_output_x11_internal__egl_display, &egl_major, &egl_minor))
    {
        log_error("output_x11: eglInitialize failed (0x%x)", eglGetError());
        output_x11__shutdown();
        return -1;
    }
    log_info("output_x11: EGL %d.%d initialized on X11 platform",
             egl_major, egl_minor);

    if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
        log_error("output_x11: eglBindAPI(GLES) failed");
        output_x11__shutdown();
        return -1;
    }

    //
    //5. Pick a GLES3-capable, window-targeted config. Asking for
    //an RGBA8 config is the most common path; EGL may end up
    //handing us RGB8-over-24bit-X-visual if alpha isn't
    //available, which is fine -- we don't composite with the
    //host's background.
    //
    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint    n_cfg = 0;
    if (!eglChooseConfig(_output_x11_internal__egl_display, cfg_attrs, &cfg, 1, &n_cfg) ||
        n_cfg == 0)
    {
        log_error("output_x11: eglChooseConfig found no matching configs");
        output_x11__shutdown();
        return -1;
    }

    EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE,
    };
    _output_x11_internal__egl_context = eglCreateContext(
        _output_x11_internal__egl_display,
        cfg,
        EGL_NO_CONTEXT,
        ctx_attrs);
    if (_output_x11_internal__egl_context == EGL_NO_CONTEXT)
    {
        log_error("output_x11: eglCreateContext failed (0x%x)", eglGetError());
        output_x11__shutdown();
        return -1;
    }

    _output_x11_internal__egl_surface = eglCreateWindowSurface(
        _output_x11_internal__egl_display,
        cfg,
        _output_x11_internal__x11_window,
        NULL);
    if (_output_x11_internal__egl_surface == EGL_NO_SURFACE)
    {
        log_error("output_x11: eglCreateWindowSurface failed (0x%x)", eglGetError());
        output_x11__shutdown();
        return -1;
    }

    if (_output_x11_internal__make_current() != 0)
    {
        output_x11__shutdown();
        return -1;
    }

    //
    //Enable vsync so eglSwapBuffers blocks until the next vblank.
    //Without this we tear + burn CPU spinning the timer; with it
    //the 16 ms timer ends up paced by the display's refresh rate.
    //Returns FALSE on drivers that don't support the interval; not
    //fatal (we'd just tear under those).
    //
    if (!eglSwapInterval(_output_x11_internal__egl_display, 1))
    {
        log_warn("output_x11: eglSwapInterval(1) failed (0x%x); tearing possible",
                 eglGetError());
    }

    const GLubyte* gl_renderer = glGetString(GL_RENDERER);
    log_info("output_x11: GL_RENDERER=%s", gl_renderer ? (const char*)gl_renderer : "?");

    //
    //6. Integrate into wl_event_loop.
    //  * X11 fd -> drain events when readable (WM_DELETE_WINDOW
    //    and resize events don't have to wait for the render tick).
    //  * Render timer -> fires every ~16ms, does one frame of
    //    clear+swap, re-arms itself.
    //
    struct wl_event_loop* loop = wl_display_get_event_loop(display);

    int x_fd = ConnectionNumber(_output_x11_internal__x11_display);
    _output_x11_internal__xconn_source = wl_event_loop_add_fd(
        loop,
        x_fd,
        WL_EVENT_READABLE,
        _output_x11_internal__on_xconn_readable,
        NULL);
    if (_output_x11_internal__xconn_source == NULL)
    {
        log_error("output_x11: wl_event_loop_add_fd failed");
        output_x11__shutdown();
        return -1;
    }

    _output_x11_internal__render_timer = wl_event_loop_add_timer(
        loop,
        _output_x11_internal__on_render_tick,
        NULL);
    if (_output_x11_internal__render_timer == NULL)
    {
        log_error("output_x11: wl_event_loop_add_timer failed");
        output_x11__shutdown();
        return -1;
    }
    wl_event_source_timer_update(_output_x11_internal__render_timer, 16);

    log_info("output_x11: %dx%d window mapped; render tick armed (~60 Hz)",
             width, height);
    return 0;
}

void output_x11__shutdown(void)
{
    if (_output_x11_internal__render_timer != NULL)
    {
        wl_event_source_remove(_output_x11_internal__render_timer);
        _output_x11_internal__render_timer = NULL;
    }
    if (_output_x11_internal__xconn_source != NULL)
    {
        wl_event_source_remove(_output_x11_internal__xconn_source);
        _output_x11_internal__xconn_source = NULL;
    }
    if (_output_x11_internal__egl_display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(_output_x11_internal__egl_display,
                       EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (_output_x11_internal__egl_surface != EGL_NO_SURFACE)
    {
        eglDestroySurface(_output_x11_internal__egl_display,
                          _output_x11_internal__egl_surface);
        _output_x11_internal__egl_surface = EGL_NO_SURFACE;
    }
    if (_output_x11_internal__egl_context != EGL_NO_CONTEXT)
    {
        eglDestroyContext(_output_x11_internal__egl_display,
                          _output_x11_internal__egl_context);
        _output_x11_internal__egl_context = EGL_NO_CONTEXT;
    }
    if (_output_x11_internal__egl_display != EGL_NO_DISPLAY)
    {
        eglTerminate(_output_x11_internal__egl_display);
        _output_x11_internal__egl_display = EGL_NO_DISPLAY;
    }
    if (_output_x11_internal__x11_display != NULL)
    {
        if (_output_x11_internal__x11_window != 0)
        {
            XDestroyWindow(_output_x11_internal__x11_display,
                           _output_x11_internal__x11_window);
            _output_x11_internal__x11_window = 0;
        }
        XCloseDisplay(_output_x11_internal__x11_display);
        _output_x11_internal__x11_display = NULL;
    }
    _output_x11_internal__wl_display = NULL;
}
