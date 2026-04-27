#ifndef MEEK_COMPOSITOR_LINUX_DMABUF_H
#define MEEK_COMPOSITOR_LINUX_DMABUF_H

#include <stdint.h>   //fixed-width types used in the info struct below.

//
//linux_dmabuf.h - zwp_linux_dmabuf_v1 global.
//
//The "fast path" for a client handing us a GPU buffer. Instead of
//the client rendering to CPU memory that we'd then upload to a GL
//texture (the wl_shm path), the client allocates a buffer on the
//GPU and hands us the file descriptor to it. We import that fd via
//EGL as a texture -- zero copy, the client's GPU pixels become our
//GPU texture directly.
//
//This module registers the zwp_linux_dmabuf_v1 protocol global and
//advertises which (DRM format, modifier) pairs we can import. The
//advertised list comes from querying the EGL display set up by
//egl_ctx.c; if that EGL display supports EGL_EXT_image_dma_buf_import_modifiers
//we can advertise tile-mode modifiers (AFBC, DCC, etc.); otherwise
//only DRM_FORMAT_MOD_INVALID (implicit linear).
//
//Actual buffer import (eglCreateImage + glEGLImageTargetTexture2DOES)
//runs on wl_surface.commit in globals.c -- this module just mediates
//the protocol-level wl_buffer creation.
//

struct wl_display;
struct wl_resource;

//
//Descriptor used by consumers that want to re-import a dmabuf
//buffer in their own EGL context (e.g. output_drm's scanout path).
//Lives inside the caller; we just fill it out from whatever state
//we stashed during params creation.
//
#define LINUX_DMABUF_INFO_MAX_PLANES 4

struct linux_dmabuf_buffer_info
{
    int      plane_count;
    struct {
        int      fd;
        uint32_t offset;
        uint32_t stride;
        uint64_t modifier;    //DRM_FORMAT_MOD_INVALID when implicit.
    } planes[LINUX_DMABUF_INFO_MAX_PLANES];
    int32_t  width;
    int32_t  height;
    uint32_t fourcc;          //DRM fourcc (XRGB8888, ARGB8888, etc.).
};

//
//Registers the zwp_linux_dmabuf_v1 global on `display`. Must be
//called AFTER egl_ctx__init() so the EGL display is available for
//format queries. If EGL isn't initialized or doesn't support the
//required extension, this logs a warning and returns without
//registering -- the compositor stays alive, dmabuf clients will
//just fall back to shm.
//
void linux_dmabuf__register(struct wl_display* display);

//
//Frees the cached (format, modifier) table allocated at register
//time. Call from main's shutdown path after wl_display_run returns.
//Safe to call even if register wasn't called (or the EGL query
//fell back and allocated nothing) -- idempotent.
//
void linux_dmabuf__shutdown(void);

//
//Query plane info for a wl_buffer resource that was produced by
//this module (i.e. came out of zwp_linux_buffer_params_v1.create
//or .create_immed). Returns non-zero on success, zero if the
//resource isn't one of ours (caller should treat as "not a dmabuf
//buffer, try the shm path"). The fds in `out->planes[*].fd`
//remain owned by us -- caller may use them with eglCreateImage
//(which dup's internally) but must NOT close them.
//
int linux_dmabuf__get_buffer_info(struct wl_resource* buffer_resource,
                                  struct linux_dmabuf_buffer_info* out);

#endif
