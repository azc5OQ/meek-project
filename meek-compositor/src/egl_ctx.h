#ifndef MEEK_COMPOSITOR_EGL_CTX_H
#define MEEK_COMPOSITOR_EGL_CTX_H

//
//egl_ctx.h - EGL + GLES3 context used for client-buffer import.
//
//Sets up a surfaceless EGL context on top of a GBM device opened
//against /dev/dri/renderD128 (falling back to /dev/dri/card0). Not
//intended to render to any output surface in this pass -- its only
//job is to own the EGL display + context so that eglCreateImage
//(for dmabuf import) and glTexImage2D (for shm upload) have a
//current context to work with.
//
//Output-surface EGL setup (presenting frames to X11 or KMS) lands
//in pass A4 and will share THIS display + context. There's one GPU
//and one context per compositor; A4 adds eglCreateWindowSurface /
//eglCreatePbufferSurface, not a new display.
//
//Pattern: EGL_PLATFORM_GBM_KHR + EGL_NO_CONFIG_KHR + GLES3 context
//with fallback from 3.2 to 3.0 -- standard modern-compositor recipe.
//

//
//forward decls
//
#ifndef EGL_VERSION_1_0
typedef void* EGLDisplay;
typedef void* EGLContext;
#endif

//
//egl_ctx__init: open render node, create gbm_device, get EGL
//display, initialize, create GLES3 context, make current.
//Logs GL_VENDOR / GL_RENDERER / GL_VERSION on success.
//
//Returns 0 on success, nonzero on any failure. Partial state is
//cleaned up before returning failure.
//
int egl_ctx__init(void);

//
//egl_ctx__shutdown: destroy context, terminate display, close
//gbm device + render node fd. Safe to call multiple times.
//
void egl_ctx__shutdown(void);

//
//accessors -- useful for linux_dmabuf.c and later output backends
//which need the display for eglCreateImage / eglCreateWindowSurface.
//
EGLDisplay egl_ctx__display(void);
EGLContext egl_ctx__context(void);

//
//Extension flags populated after egl_ctx__init(). Read-only.
//
struct egl_ctx_ext
{
    int ext_image_dma_buf_import;
    int ext_image_dma_buf_import_modifiers;
    int khr_no_config_context;
    int khr_surfaceless_context;
};

const struct egl_ctx_ext* egl_ctx__extensions(void);

//
//Make THIS thread's current EGL context the one owned by egl_ctx
//(the GBM-platform, surfaceless context used for dmabuf import +
//shm upload). Call before any GL work that must land in this
//context's object namespace -- important because output_x11.c has
//its own X11-platform EGL display/context and can leave THAT one
//current mid-tick. Without this guard, a client commit arriving
//between render ticks would accidentally touch output_x11's
//context.
//
//Returns 0 on success, nonzero if eglMakeCurrent fails. Safe to
//call even when already current on this context.
//
int egl_ctx__make_current(void);

#endif
