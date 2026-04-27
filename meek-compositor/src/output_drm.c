//
// output_drm.c - direct DRM/KMS compositor output. See output_drm.h.
//
// INVARIANTS / ASSUMPTIONS:
//
//   - /dev/dri/card0 is the primary display device. Later passes
//     should probe /dev/dri/card* but hardcoding card0 is fine for
//     the first pass (matches meek-ui/platform_linux_drm.c and every
//     phone we care about).
//   - We become DRM master for the entire lifetime of the
//     compositor. drmSetMaster either succeeds (no other compositor
//     owns the screen) or fails with EBUSY (someone else has it,
//     user needs to stop them). We never try to be polite and share.
//   - The first connected connector is the one we drive. For single-
//     output devices (phones, kiosks) that's the right answer. For
//     multi-output we'll come back to this.
//   - The connector's first mode is the preferred mode. That's what
//     "display's native resolution" means on every consumer panel.
//
// PAGE-FLIP MODEL:
//
//   Initial: render frame into EGL + GBM surface, lock the front BO,
//   register it as a DRM framebuffer, drmModeSetCrtc to program the
//   display. That's the "establish mode" step.
//
//   Steady state:
//     render frame -> eglSwapBuffers -> lock new front BO ->
//     drmModeAddFB (cached per-BO via gbm_bo_set_user_data so we
//     don't repeat the kernel-side allocation every frame) ->
//     drmModePageFlip(PAGE_FLIP_EVENT).
//
//     When the kernel acks the flip (drm_fd becomes readable),
//     drmHandleEvent dispatches _on_page_flip: release the BO we
//     were showing before the flip (GBM recycles it), promote
//     pending_bo to current_bo, and render the NEXT frame.
//
//   This gives us vsync for free -- the panel's vblank rate is
//   whatever drives our render cadence.
//
// COMPARISON TO meek-ui/platform_linux_drm.c:
//
//   That file is a full meek-ui platform backend (input, render via
//   scene, font/asset setup). We're not -- we're just an output
//   surface that clears to a color. Same DRM/GBM/EGL dance, one
//   tenth the code.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <signal.h>     //sig_atomic_t for the screenshot flag.

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>   //GLeglImageOES type + glEGLImageTargetTexture2DOES proto.

#include <wayland-server-core.h>

#include "types.h"
#include "third_party/log.h"
#include "linux_dmabuf.h"      //linux_dmabuf__get_buffer_info() for scanout.
#include "globals.h"           //globals__fire_frame_callbacks at vblank
#include "output_drm.h"

// ---------------------------------------------------------------------------
// state (one global; only ever one compositor output at a time)
// ---------------------------------------------------------------------------

typedef struct _output_drm_internal__state
{
    //-- DRM / KMS --
    int              drm_fd;
    uint32_t         connector_id;
    uint32_t         crtc_id;
    drmModeModeInfo  mode;
    drmModeCrtc*     saved_crtc;

    //-- GBM --
    struct gbm_device*  gbm_dev;
    struct gbm_surface* gbm_surface;

    //-- EGL --
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig  egl_config;

    //-- frame bookkeeping --
    //
    // current_bo: the BO the panel is currently scanning out.
    // pending_bo: the BO we've queued a page-flip TO. After the
    //             flip-ack fires we release current_bo (GBM reuses
    //             it) and pending_bo becomes current_bo.
    //
    struct gbm_bo* current_bo;
    struct gbm_bo* pending_bo;
    int            crtc_set;
    int            waiting_flip;
    uint32_t       frame_count;

    //
    // Damage gate: render+page-flip is only triggered when this is
    // non-zero. Set by output_drm__schedule_frame() (called from
    // surface.c when the shell commits, and from elsewhere when a
    // wake-up is needed). Cleared by _on_page_flip after a render
    // completes.
    //
    // Without this gate the page-flip handler ran render+flip
    // unconditionally on every vblank ack at 60 Hz, even when the
    // shell hadn't committed anything new since the last flip --
    // matches the burn pattern documented in
    // session/bugs_to_investigate.md entry #3. The gate brings us
    // in line with the standard "render only when something has
    // actually changed" approach.
    //
    int            needs_frame;


    //-- wl_event_loop integration --
    struct wl_display*      wl_display;
    struct wl_event_source* drm_source;

    //-- startup wallclock (for pulsing-color math) --
    struct timespec t0;

    //-- scanout (C6): a texture imported from meek-shell's committed
    //-- buffer. When valid=1, _render_and_present draws it as a full-
    //-- panel textured quad INSTEAD of the pulsing-gradient fallback.
    //-- egl_image is kept so we can eglDestroyImage it when the shell
    //-- commits a new buffer.
    GLuint   scanout_texture;
    EGLImage scanout_egl_image;
    int      scanout_valid;

    //-- textured-quad shader program (for scanout blit) --
    GLuint   tex_program;
    GLint    tex_u_sampler;        //location of u_tex uniform.
    GLuint   tex_vao;              //empty VAO; GLES3 needs ONE bound for draw.
} _output_drm_internal__state;

static _output_drm_internal__state g_state = {
    .drm_fd            = -1,
    .egl_display       = EGL_NO_DISPLAY,
    .egl_context       = EGL_NO_CONTEXT,
    .egl_surface       = EGL_NO_SURFACE,
    .scanout_egl_image = EGL_NO_IMAGE,
    .tex_u_sampler     = -1,

    //
    // Start with needs_frame=1 so the very first call to
    // _render_and_present (kicked from output_drm__init's tail)
    // actually runs. Without this, the gate would short-circuit
    // the bootstrap render and the panel would never light up.
    //
    .needs_frame       = 1,
};

//
// Function-pointer cache for EGL/GLES extension entry points we
// need for dmabuf re-import in our context. Resolved once at init;
// NULL means the extension wasn't available and the scanout path
// will log + bail (compositor keeps rendering the fallback).
//
typedef EGLImage (*_fncp_eglCreateImage)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib*);
typedef EGLBoolean (*_fncp_eglDestroyImage)(EGLDisplay, EGLImage);
typedef void (*_fncp_glEGLImageTargetTexture2DOES)(GLenum target, GLeglImageOES image);

static _fncp_eglCreateImage                _fncp_create_image   = NULL;
static _fncp_eglDestroyImage               _fncp_destroy_image  = NULL;
static _fncp_glEGLImageTargetTexture2DOES  _fncp_tex_from_image = NULL;

//
// Screenshot request flag. sig_atomic_t so the signal handler's
// write is a single, atomic step. "volatile" keeps the compiler
// from optimizing out reads in the render loop.
//
static volatile sig_atomic_t g_screenshot_flag = 0;

// ---------------------------------------------------------------------------
// forward decls
// ---------------------------------------------------------------------------

static int  _output_drm_internal__open_device(void);
static int  _output_drm_internal__pick_connector_and_mode(void);
static int  _output_drm_internal__init_gbm_egl(void);
static int  _output_drm_internal__init_scanout_shader(void);
static int  _output_drm_internal__resolve_egl_ext(void);
static int  _output_drm_internal__render_and_present(void);
static int  _output_drm_internal__on_drm_event(int fd, uint32_t mask, void* data);
static void _output_drm_internal__on_page_flip(int fd, unsigned frame, unsigned tv_sec, unsigned tv_usec, void* data);
static void _output_drm_internal__fb_destroy_cb(struct gbm_bo* bo, void* data);
static uint32_t _output_drm_internal__get_or_add_fb(struct gbm_bo* bo);
static float _output_drm_internal__seconds_since_init(void);
static void _output_drm_internal__render_scanout(void);
static void _output_drm_internal__render_fallback(void);
static void _output_drm_internal__release_scanout(void);
static void _output_drm_internal__capture_screenshot_if_requested(void);
static GLuint _output_drm_internal__compile_program(const char* vs_src, const char* fs_src);

// ---------------------------------------------------------------------------
// shaders -- full-screen textured quad
// ---------------------------------------------------------------------------

//
// Vertex shader. No VBO: we use gl_VertexID to generate the four
// corners of a triangle-strip quad in NDC. This keeps the
// pipeline self-contained (no attribute layout, no vertex buffer
// management) for the one-and-only draw call we need here.
//
// Mapping: gl_VertexID 0/1/2/3 -> (0,0) / (1,0) / (0,1) / (1,1).
// NDC = position * 2 - 1. UV y-flipped because GL origin is
// bottom-left and the scanout texture is top-left.
//
static const char _output_drm_internal__vs_scanout[] =
"#version 300 es\n"
"precision mediump float;\n"
"out vec2 v_uv;\n"
"void main() {\n"
"    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
"    v_uv = vec2(p.x, 1.0 - p.y);\n"
"    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";

//
// Fragment shader. sampler2D works for RGB dmabufs on
// freedreno/Mesa without needing GL_OES_EGL_image_external (which
// is only mandatory for YUV). samplerExternalOES was an earlier
// attempt that provoked EGL_BAD_SURFACE on swap, unrelated to
// this shader as it turned out -- the root cause was context
// switching between linux_dmabuf's validation import and our
// render. See _render_and_present's eglMakeCurrent guard.
//
static const char _output_drm_internal__fs_scanout[] =
"#version 300 es\n"
"precision mediump float;\n"
"in vec2 v_uv;\n"
"out vec4 f_color;\n"
"uniform sampler2D u_tex;\n"
"void main() {\n"
"    f_color = texture(u_tex, v_uv);\n"
"}\n";

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

int output_drm__init(struct wl_display* display)
{
    g_state.wl_display = display;
    clock_gettime(CLOCK_MONOTONIC, &g_state.t0);

    if (_output_drm_internal__open_device() != 0)
    {
        log_error("output_drm: failed to open /dev/dri/card0 (is it present? do we have perms? is another compositor running?)");
        goto fail;
    }

    if (_output_drm_internal__pick_connector_and_mode() != 0)
    {
        log_error("output_drm: no connected connector found");
        goto fail;
    }

    if (_output_drm_internal__init_gbm_egl() != 0)
    {
        log_error("output_drm: GBM/EGL bring-up failed");
        goto fail;
    }

    //
    // Resolve EGL/GLES extension entry points we'll need for
    // scanout dmabuf re-import. If missing, scanout is unavailable
    // but the fallback pulsing-gradient render still works, so
    // this is logged as a warning, not an error -- compositor
    // keeps running.
    //
    if (_output_drm_internal__resolve_egl_ext() != 0)
    {
        log_warn("output_drm: EGL dmabuf-import ext entry points missing; "
                 "scanout unavailable, fallback gradient only");
    }

    //
    // Compile the textured-quad shader + create an empty VAO for
    // the draw. Same "non-fatal if it fails" policy -- if shader
    // compile breaks we fall back to glClear.
    //
    if (_output_drm_internal__init_scanout_shader() != 0)
    {
        log_warn("output_drm: scanout shader init failed; fallback gradient only");
    }

    //
    // Initial frame: render once, promote to scanout via SetCrtc.
    // PageFlip can't run until the CRTC is configured (kernel
    // returns -EINVAL). After this call succeeds every subsequent
    // frame is a PageFlip.
    //
    if (_output_drm_internal__render_and_present() != 0)
    {
        log_error("output_drm: first render/present failed");
        goto fail;
    }

    //
    // Wire the DRM fd into wl_event_loop. Page-flip acks arrive as
    // readable data on the fd; drmHandleEvent parses them and
    // calls our _on_page_flip callback.
    //
    struct wl_event_loop* loop = wl_display_get_event_loop(display);
    g_state.drm_source = wl_event_loop_add_fd(
        loop, g_state.drm_fd, WL_EVENT_READABLE,
        _output_drm_internal__on_drm_event, NULL);
    if (g_state.drm_source == NULL)
    {
        log_error("output_drm: wl_event_loop_add_fd failed");
        goto fail;
    }

    log_info("output_drm: connector %u, mode %ux%u@%u, page-flip loop armed",
             g_state.connector_id,
             g_state.mode.hdisplay, g_state.mode.vdisplay,
             g_state.mode.vrefresh);
    return 0;

fail:
    output_drm__shutdown();
    return -1;
}

void output_drm__shutdown(void)
{
    //
    // Restore the original CRTC so whoever had the screen before us
    // (other compositor, getty, fbcon) comes back to a sane state
    // instead of a blank panel. Have to do this BEFORE releasing GBM
    // buffers because the saved_crtc references a buffer id that
    // must still exist at the moment of the restore.
    //
    if (g_state.saved_crtc != NULL && g_state.drm_fd >= 0)
    {
        drmModeSetCrtc(
            g_state.drm_fd,
            g_state.saved_crtc->crtc_id,
            g_state.saved_crtc->buffer_id,
            g_state.saved_crtc->x,
            g_state.saved_crtc->y,
            &g_state.connector_id, 1,
            &g_state.saved_crtc->mode);
        drmModeFreeCrtc(g_state.saved_crtc);
        g_state.saved_crtc = NULL;
    }

    if (g_state.drm_source != NULL)
    {
        wl_event_source_remove(g_state.drm_source);
        g_state.drm_source = NULL;
    }

    if (g_state.pending_bo != NULL)
    {
        gbm_surface_release_buffer(g_state.gbm_surface, g_state.pending_bo);
        g_state.pending_bo = NULL;
    }
    if (g_state.current_bo != NULL)
    {
        gbm_surface_release_buffer(g_state.gbm_surface, g_state.current_bo);
        g_state.current_bo = NULL;
    }

    //
    // Scanout texture + shader: must be deleted with OUR EGL
    // context current (they live in it). Do this before releasing
    // the context or surface.
    //
    if (g_state.egl_context != EGL_NO_CONTEXT
        && g_state.egl_display != EGL_NO_DISPLAY
        && g_state.egl_surface != EGL_NO_SURFACE)
    {
        eglMakeCurrent(g_state.egl_display,
                       g_state.egl_surface, g_state.egl_surface,
                       g_state.egl_context);
        _output_drm_internal__release_scanout();
        if (g_state.tex_vao != 0)
        {
            glDeleteVertexArrays(1, &g_state.tex_vao);
            g_state.tex_vao = 0;
        }
        if (g_state.tex_program != 0)
        {
            glDeleteProgram(g_state.tex_program);
            g_state.tex_program = 0;
        }
    }

    if (g_state.egl_surface != EGL_NO_SURFACE)
    {
        eglMakeCurrent(g_state.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_state.egl_display, g_state.egl_surface);
        g_state.egl_surface = EGL_NO_SURFACE;
    }
    if (g_state.egl_context != EGL_NO_CONTEXT)
    {
        eglDestroyContext(g_state.egl_display, g_state.egl_context);
        g_state.egl_context = EGL_NO_CONTEXT;
    }
    if (g_state.egl_display != EGL_NO_DISPLAY)
    {
        eglTerminate(g_state.egl_display);
        g_state.egl_display = EGL_NO_DISPLAY;
    }

    if (g_state.gbm_surface != NULL)
    {
        gbm_surface_destroy(g_state.gbm_surface);
        g_state.gbm_surface = NULL;
    }
    if (g_state.gbm_dev != NULL)
    {
        gbm_device_destroy(g_state.gbm_dev);
        g_state.gbm_dev = NULL;
    }

    if (g_state.drm_fd >= 0)
    {
        drmDropMaster(g_state.drm_fd);
        close(g_state.drm_fd);
        g_state.drm_fd = -1;
    }
}

// ---------------------------------------------------------------------------
// device / connector / mode
// ---------------------------------------------------------------------------

static int _output_drm_internal__open_device(void)
{
    const char* path = "/dev/dri/card0";
    g_state.drm_fd = open(path, O_RDWR | O_CLOEXEC);
    if (g_state.drm_fd < 0)
    {
        log_error("open(%s) failed: %s", path, strerror(errno));
        return -1;
    }

    //
    // Becoming DRM master is the moment things either work or fail
    // fast. EBUSY here is the "another compositor owns it" signal.
    // EACCES is "we're not root and don't have the right
    // capabilities". Both are user-actionable.
    //
    if (drmSetMaster(g_state.drm_fd) != 0)
    {
        log_error("drmSetMaster failed: %s (is another compositor running? do we have perms?)",
                  strerror(errno));
        return -1;
    }

    log_info("output_drm: opened %s (fd=%d) and became DRM master", path, g_state.drm_fd);
    return 0;
}

static int _output_drm_internal__pick_connector_and_mode(void)
{
    drmModeRes* res = drmModeGetResources(g_state.drm_fd);
    if (res == NULL)
    {
        log_error("drmModeGetResources failed: %s", strerror(errno));
        return -1;
    }

    drmModeConnector* conn = NULL;
    for (int i = 0; i < res->count_connectors; i++)
    {
        drmModeConnector* c = drmModeGetConnector(g_state.drm_fd, res->connectors[i]);
        if (c == NULL) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
        {
            conn = c;
            break;
        }
        drmModeFreeConnector(c);
    }

    if (conn == NULL)
    {
        log_error("no connected connector with modes on /dev/dri/card0");
        drmModeFreeResources(res);
        return -1;
    }

    g_state.connector_id = conn->connector_id;
    g_state.mode         = conn->modes[0];    // preferred mode

    //
    // Resolve the CRTC id through the connector's currently-bound
    // encoder. conn->encoder_id is the encoder active on this
    // connector right now (or 0 if none). Walking the full
    // encoder/crtc matrix to find a free CRTC would be more
    // general, but for a single-output panel the currently-bound
    // one is almost always right.
    //
    drmModeEncoder* enc = NULL;
    if (conn->encoder_id != 0)
    {
        enc = drmModeGetEncoder(g_state.drm_fd, conn->encoder_id);
    }
    if (enc == NULL && conn->count_encoders > 0)
    {
        //
        // Fallback: pick the first encoder the connector supports
        // and resolve its crtc_id the same way. Some connectors
        // come up with encoder_id==0 before the first modeset.
        //
        enc = drmModeGetEncoder(g_state.drm_fd, conn->encoders[0]);
    }
    if (enc == NULL)
    {
        log_error("couldn't resolve encoder for connector %u", conn->connector_id);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return -1;
    }

    if (enc->crtc_id != 0)
    {
        g_state.crtc_id = enc->crtc_id;
    }
    else
    {
        //
        // Encoder not currently driving any CRTC. Find a CRTC this
        // encoder can drive via possible_crtcs bitmask.
        //
        g_state.crtc_id = 0;
        for (int i = 0; i < res->count_crtcs; i++)
        {
            if (enc->possible_crtcs & (1 << i))
            {
                g_state.crtc_id = res->crtcs[i];
                break;
            }
        }
        if (g_state.crtc_id == 0)
        {
            log_error("no CRTC available for encoder %u", enc->encoder_id);
            drmModeFreeEncoder(enc);
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            return -1;
        }
    }

    //
    // Stash the current CRTC so shutdown can restore it. If we
    // don't do this, killing the compositor leaves the panel in
    // whatever mode we programmed, which is at best confusing and
    // at worst leaves the user looking at a blank screen.
    //
    g_state.saved_crtc = drmModeGetCrtc(g_state.drm_fd, g_state.crtc_id);

    log_info("output_drm: picked connector %u (%dx%d) crtc %u via encoder %u",
             g_state.connector_id,
             g_state.mode.hdisplay, g_state.mode.vdisplay,
             g_state.crtc_id, enc->encoder_id);

    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return 0;
}

// ---------------------------------------------------------------------------
// GBM + EGL bring-up
// ---------------------------------------------------------------------------

static int _output_drm_internal__init_gbm_egl(void)
{
    g_state.gbm_dev = gbm_create_device(g_state.drm_fd);
    if (g_state.gbm_dev == NULL)
    {
        log_error("gbm_create_device failed");
        return -1;
    }

    //
    // XRGB8888 is the one format every Mesa DRM driver supports for
    // scanout. Plain 32-bit, alpha byte ignored (we'll never use
    // real transparency for the top-level scanout surface anyway).
    //
    g_state.gbm_surface = gbm_surface_create(
        g_state.gbm_dev,
        g_state.mode.hdisplay, g_state.mode.vdisplay,
        GBM_FORMAT_XRGB8888,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (g_state.gbm_surface == NULL)
    {
        log_error("gbm_surface_create failed for %dx%d XRGB8888",
                  g_state.mode.hdisplay, g_state.mode.vdisplay);
        return -1;
    }

    //
    // EGL on GBM platform. Needs the
    // EGL_KHR_platform_gbm (or EGL_MESA_platform_gbm) extension; we
    // fetch the entry point via eglGetProcAddress so we don't
    // assume a specific header-provided version.
    //
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display == NULL)
    {
        log_error("eglGetProcAddress(eglGetPlatformDisplayEXT) returned NULL (EGL too old?)");
        return -1;
    }

    g_state.egl_display = get_platform_display(
        EGL_PLATFORM_GBM_KHR, g_state.gbm_dev, NULL);
    if (g_state.egl_display == EGL_NO_DISPLAY)
    {
        log_error("eglGetPlatformDisplay(GBM) returned EGL_NO_DISPLAY");
        return -1;
    }

    EGLint egl_major, egl_minor;
    if (eglInitialize(g_state.egl_display, &egl_major, &egl_minor) == EGL_FALSE)
    {
        log_error("eglInitialize failed: 0x%x", eglGetError());
        return -1;
    }
    log_info("output_drm: EGL %d.%d initialized on GBM platform", egl_major, egl_minor);

    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE)
    {
        log_error("eglBindAPI(GLES) failed");
        return -1;
    }

    //
    // EGL config: we need GBM to be able to make framebuffers from
    // the BOs EGL picks, so we constrain by EGL_NATIVE_VISUAL_ID =
    // the GBM format. Mesa uses the fourcc as the visual id.
    //
    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,      EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,   EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,          8,
        EGL_GREEN_SIZE,        8,
        EGL_BLUE_SIZE,         8,
        EGL_ALPHA_SIZE,        0,   // XRGB, ignore alpha channel
        EGL_NONE
    };

    EGLint num_configs = 0;
    EGLConfig configs[32];
    if (eglChooseConfig(g_state.egl_display, cfg_attrs,
                        configs, 32, &num_configs) == EGL_FALSE
        || num_configs == 0)
    {
        log_error("eglChooseConfig found nothing for GBM XRGB8888");
        return -1;
    }

    //
    // Walk the returned configs to find one whose native visual id
    // matches GBM_FORMAT_XRGB8888. Without this match,
    // eglCreateWindowSurface will either fail or succeed but
    // produce a surface we can't actually scan out from.
    //
    g_state.egl_config = EGL_NO_CONFIG_KHR;
    for (EGLint i = 0; i < num_configs; i++)
    {
        EGLint visual_id = 0;
        eglGetConfigAttrib(g_state.egl_display, configs[i],
                           EGL_NATIVE_VISUAL_ID, &visual_id);
        if ((uint32_t)visual_id == GBM_FORMAT_XRGB8888)
        {
            g_state.egl_config = configs[i];
            break;
        }
    }
    if (g_state.egl_config == EGL_NO_CONFIG_KHR)
    {
        //
        // Some drivers don't set EGL_NATIVE_VISUAL_ID on configs
        // at all. Fall back to the first config -- Mesa is usually
        // compatible for GBM_FORMAT_XRGB8888 in that case.
        //
        g_state.egl_config = configs[0];
        log_warn("output_drm: no EGL config with matching visual_id; using configs[0]");
    }

    EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    g_state.egl_context = eglCreateContext(
        g_state.egl_display, g_state.egl_config, EGL_NO_CONTEXT, ctx_attrs);
    if (g_state.egl_context == EGL_NO_CONTEXT)
    {
        log_error("eglCreateContext failed: 0x%x", eglGetError());
        return -1;
    }

    g_state.egl_surface = eglCreateWindowSurface(
        g_state.egl_display, g_state.egl_config,
        (EGLNativeWindowType)g_state.gbm_surface, NULL);
    if (g_state.egl_surface == EGL_NO_SURFACE)
    {
        log_error("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return -1;
    }

    if (eglMakeCurrent(g_state.egl_display,
                       g_state.egl_surface, g_state.egl_surface,
                       g_state.egl_context) == EGL_FALSE)
    {
        log_error("eglMakeCurrent failed: 0x%x", eglGetError());
        return -1;
    }

    log_info("output_drm: EGL window surface current; GL_VENDOR=%s GL_RENDERER=%s",
             glGetString(GL_VENDOR), glGetString(GL_RENDERER));
    return 0;
}

// ---------------------------------------------------------------------------
// per-frame: render + present
// ---------------------------------------------------------------------------

//
// Cached FB destroy callback: when GBM destroys a BO it owns, we
// drmModeRmFB the attached FB id so the kernel reclaims it. Keeps
// AddFB calls O(distinct-BOs) instead of O(frames).
//
static void _output_drm_internal__fb_destroy_cb(struct gbm_bo* bo, void* data)
{
    (void)bo;
    uint32_t fb_id = (uint32_t)(uintptr_t)data;
    if (fb_id != 0 && g_state.drm_fd >= 0)
    {
        drmModeRmFB(g_state.drm_fd, fb_id);
    }
}

static uint32_t _output_drm_internal__get_or_add_fb(struct gbm_bo* bo)
{
    uint32_t fb_id = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
    if (fb_id != 0) return fb_id;

    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t width  = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);

    //
    // drmModeAddFB (legacy, 24-bit depth + 32-bit bpp): the standard
    // path for XRGB8888 on every Mesa/Adreno/i915/amdgpu I've tried.
    // AddFB2 is more flexible (multi-plane, modifiers) but we don't
    // need those for the top-level scanout BO.
    //
    if (drmModeAddFB(g_state.drm_fd, width, height,
                     24, 32, stride, handle, &fb_id) != 0)
    {
        log_error("drmModeAddFB failed: %s", strerror(errno));
        return 0;
    }
    gbm_bo_set_user_data(bo, (void*)(uintptr_t)fb_id,
                         _output_drm_internal__fb_destroy_cb);
    return fb_id;
}

static float _output_drm_internal__seconds_since_init(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (float)(now.tv_sec  - g_state.t0.tv_sec)
         + (float)(now.tv_nsec - g_state.t0.tv_nsec) * 1e-9f;
}

static int _output_drm_internal__render_and_present(void)
{
    //
    // Force our EGL context current. Other code paths in the same
    // process (linux_dmabuf validation imports, egl_ctx surfaceless
    // utility) may have switched the current context to their own
    // display+context since our last render. eglSwapBuffers
    // against the "wrong" surface is what gave us
    // EGL_BAD_SURFACE (0x300d) during C6 testing -- this makeCurrent
    // is the guard.
    //
    if (eglMakeCurrent(g_state.egl_display,
                       g_state.egl_surface, g_state.egl_surface,
                       g_state.egl_context) == EGL_FALSE)
    {
        log_error("eglMakeCurrent at frame start failed: 0x%x", eglGetError());
        return -1;
    }

    glViewport(0, 0, g_state.mode.hdisplay, g_state.mode.vdisplay);

    //
    // Render source selection: if the shell has committed a valid
    // scanout texture, blit that; otherwise show the "waiting for
    // shell" pulsing gradient. Swap happens unconditionally so the
    // page-flip loop keeps pacing frames.
    //
    if (g_state.scanout_valid && g_state.tex_program != 0)
    {
        _output_drm_internal__render_scanout();
    }
    else
    {
        _output_drm_internal__render_fallback();
    }

    //
    // Capture before swap. After eglSwapBuffers, the back buffer
    // is undefined per GL spec (GBM has taken ownership). We'd
    // read garbage.
    //
    _output_drm_internal__capture_screenshot_if_requested();

    if (eglSwapBuffers(g_state.egl_display, g_state.egl_surface) == EGL_FALSE)
    {
        log_error("eglSwapBuffers failed: 0x%x", eglGetError());
        return -1;
    }

    //
    // The BO we just drew into is now GBM's "front" -- lock it so
    // we can reference it from a DRM framebuffer. lock_front can
    // return NULL if there's no buffer to hand out (first frame
    // edge case where SwapBuffers somehow didn't produce one), but
    // in practice after a clean SwapBuffers it always succeeds.
    //
    struct gbm_bo* new_bo = gbm_surface_lock_front_buffer(g_state.gbm_surface);
    if (new_bo == NULL)
    {
        log_error("gbm_surface_lock_front_buffer returned NULL");
        return -1;
    }

    uint32_t fb_id = _output_drm_internal__get_or_add_fb(new_bo);
    if (fb_id == 0)
    {
        gbm_surface_release_buffer(g_state.gbm_surface, new_bo);
        return -1;
    }

    if (!g_state.crtc_set)
    {
        //
        // First present: SetCrtc to program the mode. PageFlip
        // doesn't work until a CRTC is configured.
        //
        if (drmModeSetCrtc(g_state.drm_fd, g_state.crtc_id, fb_id,
                           0, 0,
                           &g_state.connector_id, 1,
                           &g_state.mode) != 0)
        {
            log_error("drmModeSetCrtc failed: %s", strerror(errno));
            gbm_surface_release_buffer(g_state.gbm_surface, new_bo);
            return -1;
        }
        g_state.crtc_set   = 1;
        g_state.current_bo = new_bo;
        log_info("output_drm: initial modeset complete (frame 0 visible)");
        //
        // For the first frame we're not waiting on a flip event
        // from the kernel (SetCrtc is synchronous). Immediately
        // schedule the next frame via another render+flip round so
        // the event-loop feedback loop kicks off.
        //
        return _output_drm_internal__render_and_present();
    }
    else
    {
        //
        // Steady-state: queue a page flip. The kernel will ack via
        // drm_fd becoming readable, where drmHandleEvent dispatches
        // to _on_page_flip, which releases the PREVIOUS current_bo
        // (the one the panel was showing until now), promotes
        // pending_bo to current_bo, and triggers the next render.
        //
        if (drmModePageFlip(g_state.drm_fd, g_state.crtc_id, fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0)
        {
            log_error("drmModePageFlip failed: %s", strerror(errno));
            gbm_surface_release_buffer(g_state.gbm_surface, new_bo);
            return -1;
        }
        g_state.pending_bo   = new_bo;
        g_state.waiting_flip = 1;
        g_state.frame_count++;
        return 0;
    }
}

// ---------------------------------------------------------------------------
// DRM event loop integration
// ---------------------------------------------------------------------------

static int _output_drm_internal__on_drm_event(int fd, uint32_t mask, void* data)
{
    (void)fd; (void)mask; (void)data;
    drmEventContext evctx = {
        .version           = 2,
        .page_flip_handler = _output_drm_internal__on_page_flip,
    };
    //
    // drmHandleEvent reads one or more completed events off the fd
    // and calls our page_flip_handler for each. Return 0 to keep
    // the event source armed.
    //
    drmHandleEvent(g_state.drm_fd, &evctx);
    return 0;
}

static void _output_drm_internal__on_page_flip(int fd, unsigned frame, unsigned tv_sec, unsigned tv_usec, void* data)
{
    (void)fd; (void)data;

    //
    // The flip is now complete. current_bo (what the panel was
    // showing until this moment) can be recycled by GBM; pending_bo
    // (what just appeared) becomes the new current.
    //
    if (g_state.current_bo != NULL)
    {
        gbm_surface_release_buffer(g_state.gbm_surface, g_state.current_bo);
    }
    g_state.current_bo   = g_state.pending_bo;
    g_state.pending_bo   = NULL;
    g_state.waiting_flip = 0;

    //
    // Log a heartbeat every 60 frames (~1/sec at 60Hz) so the log
    // isn't noisy but we can still see the loop's alive.
    //
    if ((g_state.frame_count % 60) == 0)
    {
        log_trace("output_drm: flip ack, frame %u at %u.%06u",
                  g_state.frame_count, tv_sec, tv_usec);
    }
    (void)frame;

    //
    // Drain parked wl_surface.frame callbacks. time_ms is synthesized
    // from the kernel's page-flip timestamp (tv_sec + tv_usec, in the
    // monotonic clock). This is the tick that paces conforming clients:
    // they committed a frame, asked for a frame callback, and are
    // blocked until we send done. Firing here caps their next commit
    // to the next vblank; fd production slows to vblank rate.
    //
    // Frame callbacks fire EVERY flip ack regardless of the damage
    // gate below: if a client requested a callback, we owe it the
    // notification that the buffer it committed is on screen --
    // even if we're about to go idle.
    //
    uint32_t time_ms = (uint32_t)(tv_sec * 1000u + tv_usec / 1000u);
    globals__fire_frame_callbacks(time_ms);

    //
    // Damage gate: if nothing's marked the output dirty since the
    // last flip, return WITHOUT scheduling another render. The
    // panel keeps showing whatever current_bo holds (the kernel
    // doesn't need a fresh flip to keep the image visible -- the
    // last scanout sticks until we replace it). Compositor goes
    // idle until output_drm__schedule_frame() is called (typically
    // from surface.c on the next shell commit, or from any other
    // wake-up source we add later).
    //
    // Pre-Phase-1 we rendered+flipped unconditionally on every
    // ack; that's the CPU-burn pattern in
    // session/bugs_to_investigate.md entry #3. Render only when
    // needed.
    //
    if (g_state.needs_frame == 0)
    {
        return;
    }
    g_state.needs_frame = 0;

    //
    // Schedule the next frame. Errors here don't tear down the
    // compositor -- we'll try again on the next iteration (the
    // event loop keeps running).
    //
    if (_output_drm_internal__render_and_present() != 0)
    {
        log_warn("output_drm: render_and_present failed; will retry next event");
    }
}

// ---------------------------------------------------------------------------
// scanout: shader, extension resolve, draw paths
// ---------------------------------------------------------------------------

//
// Compile one shader stage. Returns the GL shader object (or 0 on
// failure, with error already logged). Caller deletes.
//
static GLuint _output_drm_internal__compile_stage(GLenum stage, const char* src, const char* label)
{
    GLuint s = glCreateShader(stage);
    if (s == 0)
    {
        log_error("output_drm: glCreateShader(%s) returned 0", label);
        return 0;
    }
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char buf[1024];
        GLint got = 0;
        glGetShaderInfoLog(s, (GLsizei)sizeof(buf) - 1, &got, buf);
        buf[got < (GLint)sizeof(buf) ? got : (GLint)sizeof(buf) - 1] = '\0';
        log_error("output_drm: %s compile failed: %s", label, buf);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

//
// Compile + link the full-screen textured-quad program. On
// success stores the program id in g_state.tex_program and the
// sampler uniform location; returns 0. On failure cleans up and
// returns -1.
//
static GLuint _output_drm_internal__compile_program(const char* vs_src, const char* fs_src)
{
    GLuint vs = _output_drm_internal__compile_stage(GL_VERTEX_SHADER,   vs_src, "scanout.vs");
    if (vs == 0) return 0;
    GLuint fs = _output_drm_internal__compile_stage(GL_FRAGMENT_SHADER, fs_src, "scanout.fs");
    if (fs == 0) { glDeleteShader(vs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    //
    // Shader objects can be deleted immediately after attach+link;
    // the program keeps its own reference until the program itself
    // is deleted. Standard GL pattern.
    //
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char buf[1024];
        GLint got = 0;
        glGetProgramInfoLog(prog, (GLsizei)sizeof(buf) - 1, &got, buf);
        buf[got < (GLint)sizeof(buf) ? got : (GLint)sizeof(buf) - 1] = '\0';
        log_error("output_drm: scanout program link failed: %s", buf);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static int _output_drm_internal__init_scanout_shader(void)
{
    g_state.tex_program = _output_drm_internal__compile_program(
        _output_drm_internal__vs_scanout,
        _output_drm_internal__fs_scanout);
    if (g_state.tex_program == 0)
    {
        return -1;
    }
    g_state.tex_u_sampler = glGetUniformLocation(g_state.tex_program, "u_tex");
    //
    // GLES 3.0 requires a VAO to be bound for glDrawArrays to work
    // at all (unlike desktop GL compatibility profile). One empty
    // VAO is enough; no attributes needed because our vertex
    // shader generates positions from gl_VertexID.
    //
    glGenVertexArrays(1, &g_state.tex_vao);
    log_info("output_drm: scanout shader compiled, program=%u vao=%u u_tex=%d",
             g_state.tex_program, g_state.tex_vao, g_state.tex_u_sampler);
    return 0;
}

//
// Resolve EGL/GLES extension function pointers we need for the
// dmabuf re-import. Returns 0 on success. Missing entry points
// leave the pointers NULL and return -1 -- the scanout path then
// refuses to import new buffers (but the fallback render works
// because it doesn't use these).
//
static int _output_drm_internal__resolve_egl_ext(void)
{
    _fncp_create_image =
        (_fncp_eglCreateImage)eglGetProcAddress("eglCreateImage");
    _fncp_destroy_image =
        (_fncp_eglDestroyImage)eglGetProcAddress("eglDestroyImage");
    _fncp_tex_from_image =
        (_fncp_glEGLImageTargetTexture2DOES)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (_fncp_create_image   == NULL ||
        _fncp_destroy_image  == NULL ||
        _fncp_tex_from_image == NULL)
    {
        return -1;
    }
    return 0;
}

//
// Tear down the current scanout resources (texture + EGLImage).
// Used by both the import path (before swapping in a new buffer)
// and shutdown. EGL context must already be current.
//
static void _output_drm_internal__release_scanout(void)
{
    if (g_state.scanout_texture != 0)
    {
        glDeleteTextures(1, &g_state.scanout_texture);
        g_state.scanout_texture = 0;
    }
    if (g_state.scanout_egl_image != EGL_NO_IMAGE && _fncp_destroy_image != NULL)
    {
        _fncp_destroy_image(g_state.egl_display, g_state.scanout_egl_image);
        g_state.scanout_egl_image = EGL_NO_IMAGE;
    }
    g_state.scanout_valid = 0;
}

//
// Fallback render: the same pulsing gradient from before the
// scanout work. Reached when no shell is connected / the shell
// hasn't committed / the shader isn't available.
//
static void _output_drm_internal__render_fallback(void)
{
    float t = _output_drm_internal__seconds_since_init();
    float r = 0.25f + 0.15f * sinf(t * 0.7f);
    float g = 0.25f + 0.15f * sinf(t * 0.7f + 2.0f);
    float b = 0.30f + 0.20f * sinf(t * 0.7f + 4.0f);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

//
// Scanout render: glClear (to catch any edge pixels the quad
// doesn't cover), then draw a full-screen textured quad sampling
// g_state.scanout_texture.
//
//
// GL error check helper. Logs the error code + a caller-supplied
// label; returns 1 if an error was found (0 otherwise). Used
// around every stage in render_scanout so a failure is pinned to
// the exact operation that caused it.
//
static int _output_drm_internal__check_gl(const char* where)
{
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) return 0;
    log_error("output_drm: GL error 0x%04x at %s", err, where);
    //
    // Drain any other pending errors so the NEXT check's report
    // isn't muddled by this one's leftovers.
    //
    while (glGetError() != GL_NO_ERROR) { }
    return 1;
}

static void _output_drm_internal__render_scanout(void)
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    _output_drm_internal__check_gl("clear");

    glUseProgram(g_state.tex_program);
    _output_drm_internal__check_gl("useProgram");

    glBindVertexArray(g_state.tex_vao);
    _output_drm_internal__check_gl("bindVAO");

    //
    // Bind the scanout texture to texture unit 0 and point the
    // sampler uniform at it. glUniform1i(sampler, 0) is standard
    // GL idiom; it's safe to call every frame even though the
    // value doesn't change.
    //
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_state.scanout_texture);
    _output_drm_internal__check_gl("bindTexture");

    if (g_state.tex_u_sampler >= 0)
    {
        glUniform1i(g_state.tex_u_sampler, 0);
        _output_drm_internal__check_gl("uniform1i");
    }

    //
    // Four vertices, triangle-strip, gl_VertexID picks corners.
    // No depth/stencil writes; blend disabled (scanout texture
    // is already the final image).
    //
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    _output_drm_internal__check_gl("drawArrays");

    glBindVertexArray(0);
    glUseProgram(0);
}

// ---------------------------------------------------------------------------
// screenshot capture (SIGUSR1-triggered)
// ---------------------------------------------------------------------------

//
// Signal-safe trigger. Called from main.c's SIGUSR1 handler.
// Writing to volatile sig_atomic_t is guaranteed async-signal-safe
// by POSIX.
//
void output_drm__request_screenshot_from_signal(void)
{
    g_screenshot_flag = 1;
}

//
// Native-size getter. Succeeds only once the initial modeset has
// run (crtc_set is 1) and the mode has valid dimensions. Returns
// 0 when DRM isn't the active backend at all (nested X11 dev)
// since g_state.crtc_set stays 0 in that case.
//
int output_drm__get_native_size(int* w_out, int* h_out)
{
    if (!g_state.crtc_set) return 0;
    if (g_state.mode.hdisplay <= 0 || g_state.mode.vdisplay <= 0) return 0;
    if (w_out != NULL) *w_out = g_state.mode.hdisplay;
    if (h_out != NULL) *h_out = g_state.mode.vdisplay;
    return 1;
}

//
// Capture the current EGL back buffer as a PPM (P6 binary RGB).
// Runs from render_and_present AFTER the scanout/fallback draw
// but BEFORE eglSwapBuffers -- at that moment the back buffer
// holds the pixels we're about to scan out, and glReadPixels
// still has access to them. PPM is chosen for simplicity: no
// library needed, trivial to convert on the host side (`convert`
// or just renaming + viewing with feh/display).
//
static void _output_drm_internal__capture_screenshot_if_requested(void)
{
    if (!g_screenshot_flag) return;
    g_screenshot_flag = 0;

    int w = g_state.mode.hdisplay;
    int h = g_state.mode.vdisplay;
    if (w <= 0 || h <= 0) return;

    size_t nbytes = (size_t)w * (size_t)h * 4u;
    uint8_t* px = (uint8_t*)malloc(nbytes);
    if (px == NULL)
    {
        log_error("output_drm: screenshot: out of memory (%zu bytes)", nbytes);
        return;
    }
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (_output_drm_internal__check_gl("readPixels") != 0)
    {
        free(px);
        return;
    }

    //
    // Fixed path. Overwriting is fine -- the caller (user over
    // SSH) can rename/copy between captures. Keeps the contract
    // simple: "look at /tmp/meek-screenshot.ppm after USR1".
    //
    const char* path = "/tmp/meek-screenshot.ppm";
    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        log_error("output_drm: screenshot: fopen(%s) failed: %s",
                  path, strerror(errno));
        free(px);
        return;
    }

    fprintf(f, "P6\n%d %d\n255\n", w, h);
    //
    // OpenGL reads bottom-up; PPM stores top-down. Flip rows on
    // write. Also drop the alpha channel (PPM is RGB).
    //
    for (int y = h - 1; y >= 0; --y)
    {
        const uint8_t* row = &px[(size_t)y * (size_t)w * 4u];
        for (int x = 0; x < w; ++x)
        {
            fwrite(&row[(size_t)x * 4u], 1, 3, f);
        }
    }
    fclose(f);
    free(px);
    log_info("output_drm: screenshot captured to %s (%dx%d, PPM P6)", path, w, h);
}

// ---------------------------------------------------------------------------
// public entry -- damage gate: mark the output as needing a fresh frame
// ---------------------------------------------------------------------------

void output_drm__schedule_frame(void)
{
    //
    // Backend-not-active early-out: when running under nested X11
    // or headless, output_drm__init was never called, drm_fd stays
    // -1, and there's nothing to schedule. Safe no-op.
    //
    if (g_state.drm_fd < 0)
    {
        return;
    }

    g_state.needs_frame = 1;

    //
    // If a flip is already in progress, the in-flight ack will see
    // needs_frame=1 in _on_page_flip and render the next frame. No
    // need (or way) to start another render now -- DRM only allows
    // one outstanding flip per CRTC.
    //
    if (g_state.waiting_flip)
    {
        return;
    }

    //
    // No flip in flight = the loop is currently idle (we returned
    // from a previous flip ack without rendering, because nobody
    // had marked us dirty). Kick the loop back into life by
    // rendering+flipping right now; the next ack will then see
    // whatever's accumulated since.
    //
    if (_output_drm_internal__render_and_present() != 0)
    {
        log_warn("output_drm: render_and_present failed in schedule_frame");
    }
}

// ---------------------------------------------------------------------------
// public entry -- called when the shell commits a new buffer
// ---------------------------------------------------------------------------

void output_drm__on_shell_commit(struct wl_resource* buffer)
{
    //
    // Cheap early-outs. output_drm may not be the active output
    // backend (nested X11 dev), in which case ALL state is zero
    // and we should just do nothing. Also: if the shell committed
    // with no buffer attached, revert to the fallback.
    //
    if (g_state.egl_display == EGL_NO_DISPLAY)
    {
        return;
    }
    if (buffer == NULL)
    {
        //
        // Shell detached its buffer -- go back to showing the
        // fallback pulsing gradient on the next frame. EGL context
        // needs to be current to delete GL objects.
        //
        eglMakeCurrent(g_state.egl_display,
                       g_state.egl_surface, g_state.egl_surface,
                       g_state.egl_context);
        _output_drm_internal__release_scanout();
        log_trace("output_drm: shell committed with no buffer; reverting to fallback");
        //
        // Wake the render loop so the fallback paints over what was
        // the shell's last texture; without this the panel keeps
        // showing the stale image until the next damage source.
        //
        output_drm__schedule_frame();
        return;
    }
    if (_fncp_create_image == NULL || _fncp_tex_from_image == NULL)
    {
        //
        // EGL dmabuf-import ext missing; scanout permanently
        // unavailable on this display. Stay on the fallback.
        //
        return;
    }

    //
    // Query plane info from linux_dmabuf. If the buffer isn't a
    // dmabuf (e.g. shm), we don't support scanout yet -- stay on
    // the fallback. Future passes will add the shm upload path.
    //
    struct linux_dmabuf_buffer_info info;
    if (!linux_dmabuf__get_buffer_info(buffer, &info))
    {
        log_trace("output_drm: shell committed non-dmabuf buffer; "
                  "scanout unsupported in this path, fallback only");
        return;
    }
    if (info.plane_count < 1)
    {
        log_warn("output_drm: dmabuf info had 0 planes -- malformed?");
        return;
    }

    //
    // Make our EGL context current BEFORE any GL/EGL ops below --
    // egl_ctx's surfaceless context may be current (from the
    // validation import in linux_dmabuf.c) and we must switch
    // before creating textures in OUR context.
    //
    if (eglMakeCurrent(g_state.egl_display,
                       g_state.egl_surface, g_state.egl_surface,
                       g_state.egl_context) == EGL_FALSE)
    {
        log_error("output_drm: eglMakeCurrent(scanout import) failed: 0x%x",
                  eglGetError());
        return;
    }

    //
    // Release whatever scanout texture we had from a previous
    // commit. One buffer in flight at a time is fine for the
    // first pass -- we don't try to double-buffer scanout
    // textures. If shell commits faster than we can import we'll
    // just keep replacing.
    //
    _output_drm_internal__release_scanout();

    //
    // Build the EGL attribute list for plane 0. Single-plane only
    // for v1 (protocol says so). Modifier gets split hi/lo as
    // per EGL_EXT_image_dma_buf_import_modifiers.
    //
    EGLAttrib attrs[32];
    int ai = 0;
    attrs[ai++] = EGL_LINUX_DRM_FOURCC_EXT;      attrs[ai++] = info.fourcc;
    attrs[ai++] = EGL_WIDTH;                     attrs[ai++] = info.width;
    attrs[ai++] = EGL_HEIGHT;                    attrs[ai++] = info.height;
    attrs[ai++] = EGL_DMA_BUF_PLANE0_FD_EXT;     attrs[ai++] = info.planes[0].fd;
    attrs[ai++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attrs[ai++] = info.planes[0].offset;
    attrs[ai++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attrs[ai++] = info.planes[0].stride;
    if (info.planes[0].modifier != 0 && info.planes[0].modifier != ((uint64_t)-1))
    {
        attrs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attrs[ai++] = (EGLAttrib)(info.planes[0].modifier & 0xFFFFFFFFULL);
        attrs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attrs[ai++] = (EGLAttrib)(info.planes[0].modifier >> 32);
    }
    attrs[ai++] = EGL_NONE;

    EGLImage img = _fncp_create_image(
        g_state.egl_display, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (img == EGL_NO_IMAGE)
    {
        log_warn("output_drm: eglCreateImage(scanout) failed: 0x%x",
                 eglGetError());
        return;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    _fncp_tex_from_image(GL_TEXTURE_2D, (GLeglImageOES)img);
    //
    // Drain GL errors generated by the bind/target-image. If the
    // import was invalid, this is where we'd see it (e.g.
    // GL_INVALID_OPERATION from glEGLImageTargetTexture2DOES).
    //
    if (_output_drm_internal__check_gl("import target-2d") != 0)
    {
        //
        // Image import failed at the GL level. Drop the texture
        // (but keep scanout_valid=0 so we render fallback). The
        // EGLImage we got back may still be holding the dmabuf
        // open; destroy it so the fds recycle.
        //
        glDeleteTextures(1, &tex);
        if (_fncp_destroy_image != NULL)
        {
            _fncp_destroy_image(g_state.egl_display, img);
        }
        return;
    }

    g_state.scanout_texture   = tex;
    g_state.scanout_egl_image = img;
    g_state.scanout_valid     = 1;

    log_trace("output_drm: scanout imported %dx%d fourcc=0x%x tex=%u",
              info.width, info.height, info.fourcc, tex);

    //
    // Damage gate: shell delivered a fresh buffer -> wake the
    // render loop so the panel updates. Without this the new
    // texture would just sit in scanout_texture until something
    // else triggered a render (nothing else does).
    //
    output_drm__schedule_frame();
}
