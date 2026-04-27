//
//egl_ctx.c - EGL + GLES3 context for client-buffer import.
//
//Uses the standard modern-compositor EGL bring-up pattern:
//eglGetPlatformDisplayEXT + EGL_PLATFORM_GBM_KHR + EGL_NO_CONFIG_KHR
//+ GLES 3.2-or-3.0 context. We skip the EGL_PLATFORM_DEVICE_EXT
//branch (we always have a gbm device) and skip IMG_context_priority
//+ KHR_context_flush_control (not needed for an import-only
//context).
//
//Render-node selection: open /dev/dri/renderD128 first (the normal
//non-privileged render node on phones and most desktops). If that
//fails, fall back to /dev/dri/card0. renderD* nodes are preferred
//because they don't require DRM master -- we can open them as a
//normal user for EGL-only work, and only take master via libseat
//when the DRM/KMS output backend comes online (pass A4/C10).
//

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <gbm.h>

#include "types.h"
#include "third_party/log.h"

#include "egl_ctx.h"

//
//module-level state. Owned start-to-finish by this TU.
//
static int                _egl_ctx_internal__drm_fd    = -1;
static struct gbm_device* _egl_ctx_internal__gbm_dev   = NULL;
static EGLDisplay         _egl_ctx_internal__egl_disp  = EGL_NO_DISPLAY;
static EGLContext         _egl_ctx_internal__egl_ctx   = EGL_NO_CONTEXT;
static struct egl_ctx_ext _egl_ctx_internal__exts      = {0};

//
//forward decls for file-local statics
//
static int  _egl_ctx_internal__open_render_node(void);
static int  _egl_ctx_internal__has_client_extension(const char* exts, const char* name);
static int  _egl_ctx_internal__has_display_extension(EGLDisplay d, const char* name);
static void _egl_ctx_internal__log_gl_info(void);

//
//try /dev/dri/renderD128 then /dev/dri/card0. Returns fd or -1.
//
static int _egl_ctx_internal__open_render_node(void)
{
    static const char* const candidates[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        "/dev/dri/card0",
        NULL,
    };

    for (const char* const* p = candidates; *p != NULL; ++p)
    {
        int fd = open(*p, O_RDWR | O_CLOEXEC);
        if (fd >= 0)
        {
            log_info("egl_ctx: opened %s (fd=%d)", *p, fd);
            return fd;
        }
        log_trace("egl_ctx: open(%s) failed", *p);
    }
    log_error("egl_ctx: no DRM render node found (tried renderD128/129/card0)");
    return -1;
}

//
//Cheap space-separated word match on the EGL extension string.
//Matches the whole word, not a substring, so "EGL_KHR_foo" doesn't
//accidentally match "EGL_KHR_foo_bar".
//
static int _egl_ctx_internal__has_client_extension(const char* exts, const char* name)
{
    if (exts == NULL || name == NULL)
    {
        return 0;
    }

    const size_t n = strlen(name);
    const char*  p = exts;
    while (*p)
    {
        const char* end = strchr(p, ' ');
        size_t      len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && strncmp(p, name, n) == 0)
        {
            return 1;
        }
        if (end == NULL)
        {
            break;
        }
        p = end + 1;
    }
    return 0;
}

static int _egl_ctx_internal__has_display_extension(EGLDisplay d, const char* name)
{
    return _egl_ctx_internal__has_client_extension(eglQueryString(d, EGL_EXTENSIONS), name);
}

static void _egl_ctx_internal__log_gl_info(void)
{
    const GLubyte* vendor   = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version  = glGetString(GL_VERSION);
    log_info("egl_ctx: GL_VENDOR=%s",   vendor   ? (const char*)vendor   : "(null)");
    log_info("egl_ctx: GL_RENDERER=%s", renderer ? (const char*)renderer : "(null)");
    log_info("egl_ctx: GL_VERSION=%s",  version  ? (const char*)version  : "(null)");
}

int egl_ctx__init(void)
{
    //
    //1. Render node + gbm device.
    //
    _egl_ctx_internal__drm_fd = _egl_ctx_internal__open_render_node();
    if (_egl_ctx_internal__drm_fd < 0)
    {
        return -1;
    }

    _egl_ctx_internal__gbm_dev = gbm_create_device(_egl_ctx_internal__drm_fd);
    if (_egl_ctx_internal__gbm_dev == NULL)
    {
        log_error("egl_ctx: gbm_create_device failed");
        close(_egl_ctx_internal__drm_fd);
        _egl_ctx_internal__drm_fd = -1;
        return -1;
    }

    //
    //2. Check client-side EGL extensions (queried against
    //EGL_NO_DISPLAY). EGL_EXT_platform_base is needed for
    //eglGetPlatformDisplayEXT; EGL_KHR_platform_gbm (or the
    //equivalent MESA-namespace ext) is needed for the
    //EGL_PLATFORM_GBM_KHR enum.
    //
    const char* client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (client_exts == NULL ||
        !_egl_ctx_internal__has_client_extension(client_exts, "EGL_EXT_platform_base") ||
        (!_egl_ctx_internal__has_client_extension(client_exts, "EGL_KHR_platform_gbm") &&
         !_egl_ctx_internal__has_client_extension(client_exts, "EGL_MESA_platform_gbm")))
    {
        log_error("egl_ctx: EGL client lacks platform_base + platform_gbm extensions");
        egl_ctx__shutdown();
        return -1;
    }

    //
    //3. Platform display on top of the GBM device. Function pointer
    //is resolved via eglGetProcAddress because it's an extension
    //entry point, not a symbol linked in libEGL.so's ABI.
    //
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display == NULL)
    {
        log_error("egl_ctx: eglGetPlatformDisplayEXT not resolvable");
        egl_ctx__shutdown();
        return -1;
    }

    _egl_ctx_internal__egl_disp = get_platform_display(EGL_PLATFORM_GBM_KHR,
                                                       _egl_ctx_internal__gbm_dev,
                                                       NULL);
    if (_egl_ctx_internal__egl_disp == EGL_NO_DISPLAY)
    {
        log_error("egl_ctx: eglGetPlatformDisplayEXT(GBM) returned EGL_NO_DISPLAY");
        egl_ctx__shutdown();
        return -1;
    }

    //
    //4. Initialize. Logs the EGL version so we can spot old stacks.
    //
    EGLint egl_major = 0, egl_minor = 0;
    if (!eglInitialize(_egl_ctx_internal__egl_disp, &egl_major, &egl_minor))
    {
        log_error("egl_ctx: eglInitialize failed (0x%x)", eglGetError());
        egl_ctx__shutdown();
        return -1;
    }
    log_info("egl_ctx: EGL %d.%d initialized", egl_major, egl_minor);

    //
    //5. Display-side extensions. We REQUIRE
    //EGL_EXT_image_dma_buf_import (otherwise we can't import client
    //buffers, which is the whole point). The others are optional
    //but we check and log so debugging is easier.
    //
    _egl_ctx_internal__exts.ext_image_dma_buf_import =
        _egl_ctx_internal__has_display_extension(_egl_ctx_internal__egl_disp,
                                                 "EGL_EXT_image_dma_buf_import");
    _egl_ctx_internal__exts.ext_image_dma_buf_import_modifiers =
        _egl_ctx_internal__has_display_extension(_egl_ctx_internal__egl_disp,
                                                 "EGL_EXT_image_dma_buf_import_modifiers");
    _egl_ctx_internal__exts.khr_no_config_context =
        _egl_ctx_internal__has_display_extension(_egl_ctx_internal__egl_disp,
                                                 "EGL_KHR_no_config_context");
    _egl_ctx_internal__exts.khr_surfaceless_context =
        _egl_ctx_internal__has_display_extension(_egl_ctx_internal__egl_disp,
                                                 "EGL_KHR_surfaceless_context");

    if (!_egl_ctx_internal__exts.ext_image_dma_buf_import)
    {
        log_error("egl_ctx: EGL_EXT_image_dma_buf_import missing -- driver too old? "
                  "Compositor can't import client dmabufs without it.");
        egl_ctx__shutdown();
        return -1;
    }
    if (!_egl_ctx_internal__exts.ext_image_dma_buf_import_modifiers)
    {
        log_warn("egl_ctx: EGL_EXT_image_dma_buf_import_modifiers missing -- "
                 "we can still import linear dmabufs but modern clients using tiled "
                 "buffers will fall back to shm");
    }
    if (!_egl_ctx_internal__exts.khr_surfaceless_context)
    {
        log_warn("egl_ctx: EGL_KHR_surfaceless_context missing -- "
                 "eglMakeCurrent(EGL_NO_SURFACE,...) may fail; A4 will need to "
                 "create a pbuffer surface as fallback");
    }

    //
    //6. GLES API + context. Try 3.2 first; fall back to 3.0 for
    //older drivers. EGL_NO_CONFIG_KHR skips the config-matching step
    //entirely, which is what we want for an import-only context.
    //
    if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
        log_error("egl_ctx: eglBindAPI(GLES) failed (0x%x)", eglGetError());
        egl_ctx__shutdown();
        return -1;
    }

    EGLConfig cfg = EGL_NO_CONFIG_KHR;
    if (!_egl_ctx_internal__exts.khr_no_config_context)
    {
        //
        //No-config contexts not supported; pick any GLES3-capable
        //config so eglCreateContext doesn't fail. We won't actually
        //render to this config's surface -- we're surfaceless.
        //
        EGLint cfg_attrs[] = {
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE,   8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE,  8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLint n = 0;
        if (!eglChooseConfig(_egl_ctx_internal__egl_disp, cfg_attrs, &cfg, 1, &n) || n == 0)
        {
            log_error("egl_ctx: eglChooseConfig failed (no KHR_no_config_context)");
            egl_ctx__shutdown();
            return -1;
        }
    }

    EGLint ctx32_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2,
        EGL_NONE,
    };
    _egl_ctx_internal__egl_ctx = eglCreateContext(_egl_ctx_internal__egl_disp,
                                                  cfg,
                                                  EGL_NO_CONTEXT,
                                                  ctx32_attrs);
    if (_egl_ctx_internal__egl_ctx == EGL_NO_CONTEXT)
    {
        log_warn("egl_ctx: GLES 3.2 context failed (0x%x), trying 3.0",
                 eglGetError());
        EGLint ctx30_attrs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 0,
            EGL_NONE,
        };
        _egl_ctx_internal__egl_ctx = eglCreateContext(_egl_ctx_internal__egl_disp,
                                                      cfg,
                                                      EGL_NO_CONTEXT,
                                                      ctx30_attrs);
    }
    if (_egl_ctx_internal__egl_ctx == EGL_NO_CONTEXT)
    {
        log_error("egl_ctx: eglCreateContext failed for both 3.2 and 3.0 (0x%x)",
                  eglGetError());
        egl_ctx__shutdown();
        return -1;
    }

    //
    //7. Make current on no surface. Requires KHR_surfaceless_context;
    //if that's missing this will fail and A4 has to retrofit a
    //pbuffer surface. We log a warn above but try anyway in case the
    //driver honors it without advertising the extension (some do).
    //
    if (!eglMakeCurrent(_egl_ctx_internal__egl_disp,
                        EGL_NO_SURFACE, EGL_NO_SURFACE,
                        _egl_ctx_internal__egl_ctx))
    {
        log_error("egl_ctx: eglMakeCurrent(NO_SURFACE) failed (0x%x)", eglGetError());
        egl_ctx__shutdown();
        return -1;
    }

    _egl_ctx_internal__log_gl_info();
    return 0;
}

void egl_ctx__shutdown(void)
{
    if (_egl_ctx_internal__egl_disp != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(_egl_ctx_internal__egl_disp,
                       EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (_egl_ctx_internal__egl_ctx != EGL_NO_CONTEXT)
    {
        eglDestroyContext(_egl_ctx_internal__egl_disp, _egl_ctx_internal__egl_ctx);
        _egl_ctx_internal__egl_ctx = EGL_NO_CONTEXT;
    }
    if (_egl_ctx_internal__egl_disp != EGL_NO_DISPLAY)
    {
        eglTerminate(_egl_ctx_internal__egl_disp);
        _egl_ctx_internal__egl_disp = EGL_NO_DISPLAY;
    }
    if (_egl_ctx_internal__gbm_dev != NULL)
    {
        gbm_device_destroy(_egl_ctx_internal__gbm_dev);
        _egl_ctx_internal__gbm_dev = NULL;
    }
    if (_egl_ctx_internal__drm_fd >= 0)
    {
        close(_egl_ctx_internal__drm_fd);
        _egl_ctx_internal__drm_fd = -1;
    }
    memset(&_egl_ctx_internal__exts, 0, sizeof(_egl_ctx_internal__exts));
}

EGLDisplay egl_ctx__display(void)
{
    return _egl_ctx_internal__egl_disp;
}

EGLContext egl_ctx__context(void)
{
    return _egl_ctx_internal__egl_ctx;
}

const struct egl_ctx_ext* egl_ctx__extensions(void)
{
    return &_egl_ctx_internal__exts;
}

int egl_ctx__make_current(void)
{
    if (_egl_ctx_internal__egl_disp == EGL_NO_DISPLAY ||
        _egl_ctx_internal__egl_ctx  == EGL_NO_CONTEXT)
    {
        log_error("egl_ctx__make_current: context not initialized");
        return -1;
    }
    //
    //Idempotent-ish. If we're already current, eglMakeCurrent is a
    //fast no-op on most Mesa drivers. Slight cost on NVIDIA; still
    //cheaper than rendering with the wrong context.
    //
    if (!eglMakeCurrent(_egl_ctx_internal__egl_disp,
                        EGL_NO_SURFACE, EGL_NO_SURFACE,
                        _egl_ctx_internal__egl_ctx))
    {
        log_error("egl_ctx__make_current: eglMakeCurrent failed (0x%x)",
                  eglGetError());
        return -1;
    }
    return 0;
}
