//
//linux_dmabuf.c - zwp_linux_dmabuf_v1 global.
//
//Advertises which DRM format / modifier pairs we can import as
//textures via EGL. Actual import happens in globals.c's
//wl_surface.commit handler (pass A3.3). This file is the protocol-
//level advertise + create-params mediator.
//
//Uses the standard EGL query pattern for advertised (format,
//modifiers) pairs: query the EGL display for supported formats,
//then per format query the modifier list. See the
//zwp_linux_dmabuf_v1 protocol spec (wayland-protocols/staging/
//linux-dmabuf/linux-dmabuf-v1.xml) for the send_format /
//send_modifier event semantics.
//
//Protocol version: we advertise v3. v4 adds a "feedback" object
//that replaces the per-bind format burst with a more efficient
//shared-fd mechanism; not worth the extra code until we have
//measurable bind latency issues.
//
//Fallback path: if the GPU driver doesn't support
//EGL_EXT_image_dma_buf_import_modifiers, we advertise ARGB8888 +
//XRGB8888 with DRM_FORMAT_MOD_INVALID (implicit / linear). Clients
//still get dmabuf support, just without tile-mode sharing.
//

#include <stdlib.h>
#include <string.h>
#include <unistd.h> //close() for plane-fd leak prevention in params_on_add.

#include <wayland-server-core.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h> //glEGLImageTargetTexture2DOES prototype.

#include <drm_fourcc.h>

#include "linux-dmabuf-v1-protocol.h"

#include "types.h"
#include "third_party/log.h"
#include "clib/memory_manager.h"

#include "egl_ctx.h"
#include "linux_dmabuf.h"

//
//Format list cached at register time. One entry per DRM fourcc the
//GPU driver accepts; each entry owns a modifier-list (with
//DRM_FORMAT_MOD_INVALID appended to allow implicit-modifier import
//even when the driver advertises an explicit modifier list).
//
struct _linux_dmabuf_internal__format_entry
{
    uint32_t  drm_fourcc;
    int32_t   num_modifiers;
    uint64_t* modifiers; //GUI_MALLOC_T(..., MM_TYPE_RENDERER)-owned.
};

static struct _linux_dmabuf_internal__format_entry* _linux_dmabuf_internal__formats = NULL;
static int32_t                                      _linux_dmabuf_internal__n_formats = 0;

//
//Extension entry points resolved at register time.
//
//The two query_* functions come from EGL_EXT_image_dma_buf_import_modifiers.
//The two image_* functions come from EGL 1.5 core OR from
//EGL_KHR_image_base; we load via eglGetProcAddress to stay
//portable across both.
//glEGLImageTargetTexture2DOES is from GL_OES_EGL_image.
//
typedef EGLBoolean (*_fncp_eglQueryDmaBufFormatsEXT)(EGLDisplay, EGLint, EGLint*, EGLint*);
typedef EGLBoolean (*_fncp_eglQueryDmaBufModifiersEXT)(EGLDisplay, EGLint, EGLint,
                                                      EGLuint64KHR*, EGLBoolean*, EGLint*);
typedef EGLImage   (*_fncp_eglCreateImage)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib*);
typedef EGLBoolean (*_fncp_eglDestroyImage)(EGLDisplay, EGLImage);
typedef void       (*_fncp_glEGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);

static _fncp_eglQueryDmaBufFormatsEXT      _linux_dmabuf_internal__query_formats   = NULL;
static _fncp_eglQueryDmaBufModifiersEXT    _linux_dmabuf_internal__query_modifiers = NULL;
static _fncp_eglCreateImage                _linux_dmabuf_internal__create_image    = NULL;
static _fncp_eglDestroyImage               _linux_dmabuf_internal__destroy_image   = NULL;
static _fncp_glEGLImageTargetTexture2DOES  _linux_dmabuf_internal__target_tex_2d   = NULL;

//
//forward decls for file-local statics
//
static void _linux_dmabuf_internal__query_egl_formats(void);
static void _linux_dmabuf_internal__free_formats(void);
static void _linux_dmabuf_internal__dmabuf_bind(struct wl_client* c, void* data,
                                                uint32_t version, uint32_t id);
static void _linux_dmabuf_internal__dmabuf_on_destroy(struct wl_client* c, struct wl_resource* r);
static void _linux_dmabuf_internal__dmabuf_on_create_params(struct wl_client* c, struct wl_resource* r,
                                                            uint32_t id);
static void _linux_dmabuf_internal__dmabuf_on_get_default_feedback(struct wl_client* c,
    struct wl_resource* r, uint32_t id);
static void _linux_dmabuf_internal__dmabuf_on_get_surface_feedback(struct wl_client* c,
    struct wl_resource* r, uint32_t id, struct wl_resource* surface);

//-- params forward decl (needed before prototypes that take struct _linux_dmabuf_internal__params*) --
struct _linux_dmabuf_internal__params;

//-- params vtable --
static void _linux_dmabuf_internal__params_destroy_resource(struct wl_resource* r);
static void _linux_dmabuf_internal__params_on_destroy(struct wl_client* c, struct wl_resource* r);
static void _linux_dmabuf_internal__params_on_add(struct wl_client* c, struct wl_resource* r,
                                                  int32_t fd, uint32_t plane_idx,
                                                  uint32_t offset, uint32_t stride,
                                                  uint32_t modifier_hi, uint32_t modifier_lo);
static void _linux_dmabuf_internal__params_on_create(struct wl_client* c, struct wl_resource* r,
                                                     int32_t w, int32_t h,
                                                     uint32_t fourcc, uint32_t flags);
static void _linux_dmabuf_internal__params_on_create_immed(struct wl_client* c, struct wl_resource* r,
                                                           uint32_t buffer_id,
                                                           int32_t w, int32_t h,
                                                           uint32_t fourcc, uint32_t flags);

//-- wl_buffer impl for imported dmabufs --
static void _linux_dmabuf_internal__buffer_destroy_resource(struct wl_resource* r);
static void _linux_dmabuf_internal__buffer_on_destroy(struct wl_client* c, struct wl_resource* r);

//-- import helper --
static int _linux_dmabuf_internal__do_import(
    struct _linux_dmabuf_internal__params* p,
    int32_t w, int32_t h, uint32_t fourcc,
    EGLImage* out_image, GLuint* out_tex);

//
//Query EGL for every (format, modifiers) pair the GPU supports.
//Populates the module-level _formats array. On failure or when the
//required extension is missing, falls back to ARGB8888 + XRGB8888
//with implicit-modifier-only.
//
static void _linux_dmabuf_internal__query_egl_formats(void)
{
    //
    //Double-call guard. linux_dmabuf__register is idempotent by the
    //log_warn at its entry, but the format-cache population code
    //assumes a clean slate; free any earlier allocation before
    //rebuilding so there's no leak if something ever does call
    //twice.
    //
    if (_linux_dmabuf_internal__formats != NULL)
    {
        log_warn("linux_dmabuf: query_egl_formats called with formats already cached; "
                 "freeing previous cache");
        _linux_dmabuf_internal__free_formats();
    }

    const struct egl_ctx_ext* exts = egl_ctx__extensions();
    EGLDisplay egl_disp = egl_ctx__display();

    //
    //Without EXT_image_dma_buf_import_modifiers we can't query the
    //driver's format list. Advertise a conservative fallback --
    //two formats, one implicit modifier each.
    //
    if (!exts->ext_image_dma_buf_import_modifiers)
    {
        log_warn("linux_dmabuf: EGL_EXT_image_dma_buf_import_modifiers missing; "
                 "advertising fallback ARGB8888/XRGB8888 with implicit modifiers only");
        _linux_dmabuf_internal__n_formats = 2;
        _linux_dmabuf_internal__formats = GUI_CALLOC_T(2, sizeof(*_linux_dmabuf_internal__formats),
                                                       MM_TYPE_RENDERER);
        if (_linux_dmabuf_internal__formats == NULL)
        {
            _linux_dmabuf_internal__n_formats = 0;
            return;
        }

        uint64_t* m0 = GUI_MALLOC_T(sizeof(uint64_t), MM_TYPE_RENDERER);
        uint64_t* m1 = GUI_MALLOC_T(sizeof(uint64_t), MM_TYPE_RENDERER);
        if (m0 == NULL || m1 == NULL)
        {
            if (m0) GUI_FREE(m0);
            if (m1) GUI_FREE(m1);
            GUI_FREE(_linux_dmabuf_internal__formats);
            _linux_dmabuf_internal__formats = NULL;
            _linux_dmabuf_internal__n_formats = 0;
            return;
        }
        m0[0] = DRM_FORMAT_MOD_INVALID;
        m1[0] = DRM_FORMAT_MOD_INVALID;
        _linux_dmabuf_internal__formats[0] = (struct _linux_dmabuf_internal__format_entry){
            .drm_fourcc = DRM_FORMAT_ARGB8888, .num_modifiers = 1, .modifiers = m0 };
        _linux_dmabuf_internal__formats[1] = (struct _linux_dmabuf_internal__format_entry){
            .drm_fourcc = DRM_FORMAT_XRGB8888, .num_modifiers = 1, .modifiers = m1 };
        return;
    }

    //
    //Resolve the query entry points. Separate from egl_ctx.c's
    //resolution because these are extension-only, not needed unless
    //we're running the dmabuf path.
    //
    _linux_dmabuf_internal__query_formats =
        (_fncp_eglQueryDmaBufFormatsEXT)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    _linux_dmabuf_internal__query_modifiers =
        (_fncp_eglQueryDmaBufModifiersEXT)eglGetProcAddress("eglQueryDmaBufModifiersEXT");

    if (_linux_dmabuf_internal__query_formats == NULL ||
        _linux_dmabuf_internal__query_modifiers == NULL)
    {
        log_warn("linux_dmabuf: extension functions missing despite extension being advertised; "
                 "skipping dmabuf registration");
        return;
    }

    //
    //Step 1: how many formats are there?
    //
    EGLint fmt_count = 0;
    if (!_linux_dmabuf_internal__query_formats(egl_disp, 0, NULL, &fmt_count) || fmt_count <= 0)
    {
        log_error("linux_dmabuf: eglQueryDmaBufFormatsEXT count call failed");
        return;
    }

    EGLint* raw_formats = GUI_CALLOC_T(fmt_count, sizeof(EGLint), MM_TYPE_RENDERER);
    if (raw_formats == NULL)
    {
        return;
    }
    if (!_linux_dmabuf_internal__query_formats(egl_disp, fmt_count, raw_formats, &fmt_count))
    {
        GUI_FREE(raw_formats);
        log_error("linux_dmabuf: eglQueryDmaBufFormatsEXT data call failed");
        return;
    }

    //
    //Step 2: for each format, query its modifier list. Append
    //DRM_FORMAT_MOD_INVALID to every list so implicit-modifier
    //clients work even when the driver only exposes explicit
    //modifiers.
    //
    _linux_dmabuf_internal__formats = GUI_CALLOC_T(fmt_count, sizeof(*_linux_dmabuf_internal__formats),
                                                   MM_TYPE_RENDERER);
    if (_linux_dmabuf_internal__formats == NULL)
    {
        GUI_FREE(raw_formats);
        return;
    }

    for (EGLint i = 0; i < fmt_count; ++i)
    {
        uint32_t fourcc = (uint32_t)raw_formats[i];
        EGLint   mod_count = 0;
        _linux_dmabuf_internal__query_modifiers(egl_disp, fourcc, 0, NULL, NULL, &mod_count);

        //
        //+1 slot for the implicit-modifier sentinel.
        //
        int32_t   total   = mod_count + 1;
        uint64_t* mods    = GUI_CALLOC_T(total, sizeof(uint64_t), MM_TYPE_RENDERER);
        if (mods == NULL)
        {
            continue;
        }
        if (mod_count > 0)
        {
            EGLBoolean* external_only = GUI_CALLOC_T(mod_count, sizeof(EGLBoolean),
                                                     MM_TYPE_RENDERER);
            if (external_only == NULL)
            {
                GUI_FREE(mods);
                continue;
            }
            _linux_dmabuf_internal__query_modifiers(egl_disp, fourcc, mod_count,
                                                    (EGLuint64KHR*)mods,
                                                    external_only,
                                                    &mod_count);
            GUI_FREE(external_only);
        }
        mods[mod_count] = DRM_FORMAT_MOD_INVALID;

        _linux_dmabuf_internal__formats[_linux_dmabuf_internal__n_formats++] =
            (struct _linux_dmabuf_internal__format_entry){
                .drm_fourcc    = fourcc,
                .num_modifiers = total,
                .modifiers     = mods,
            };
    }

    GUI_FREE(raw_formats);
    log_info("linux_dmabuf: %d EGL-supported formats cached", _linux_dmabuf_internal__n_formats);
}

static void _linux_dmabuf_internal__free_formats(void)
{
    if (_linux_dmabuf_internal__formats == NULL)
    {
        return;
    }
    for (int32_t i = 0; i < _linux_dmabuf_internal__n_formats; ++i)
    {
        if (_linux_dmabuf_internal__formats[i].modifiers != NULL)
        {
            GUI_FREE(_linux_dmabuf_internal__formats[i].modifiers);
        }
    }
    GUI_FREE(_linux_dmabuf_internal__formats);
    _linux_dmabuf_internal__formats   = NULL;
    _linux_dmabuf_internal__n_formats = 0;
}

// ============================================================
// zwp_linux_buffer_params_v1 (stubs; real import in A3.3)
// ============================================================

//
//Per-params state: accumulated plane fds (up to 4, per the spec's
//limit) + a used flag. Each add() call stashes one plane; the
//create/create_immed call then uses the accumulated planes + the
//passed (w, h, fourcc, flags) to drive eglCreateImage.
//
//A params can only be used ONCE per spec -- trying to re-use
//after create() is a protocol error. `used` guards that.
//
#define _LINUX_DMABUF_MAX_PLANES 4

struct _linux_dmabuf_internal__plane
{
    int32_t  fd;          //-1 when unset.
    uint32_t offset;
    uint32_t stride;
    uint64_t modifier;    //DRM_FORMAT_MOD_INVALID when implicit.
};

struct _linux_dmabuf_internal__params
{
    struct wl_resource* resource;
    struct _linux_dmabuf_internal__plane planes[_LINUX_DMABUF_MAX_PLANES];
    int used;
};

//
//Per-client_buffer state for a successfully imported dmabuf.
//wl_resource_set_user_data is (client_buffer*); its destroy
//callback releases the EGLImage + GL texture AND closes the plane
//fds stored below.
//
//Why we keep plane fds alive for the buffer's lifetime (instead of
//closing after the first eglCreateImage): consumers in OTHER EGL
//contexts may want to re-import the same buffer. The scanout path
//in output_drm.c does exactly that -- it re-imports in its own EGL
//display/context, separate from egl_ctx's surfaceless one, because
//EGL textures don't cross displays. Keeping the fds open costs
//~one open-file per live buffer (negligible); the alternative
//would be fd-recv-over-socket dances or consolidating EGL
//displays, both bigger.
//
struct _linux_dmabuf_internal__client_buffer
{
    struct wl_resource* resource;  //wl_buffer.
    EGLImage egl_image;            //validation image (egl_ctx display).
    GLuint   gl_texture;           //validation texture (egl_ctx context).
    int32_t  w, h;
    uint32_t fourcc;

    //-- plane fds + metadata, kept alive for the buffer's lifetime --
    int plane_count;
    struct _linux_dmabuf_internal__plane planes[_LINUX_DMABUF_MAX_PLANES];
};

static const struct zwp_linux_buffer_params_v1_interface _linux_dmabuf_internal__params_impl = {
    .destroy      = _linux_dmabuf_internal__params_on_destroy,
    .add          = _linux_dmabuf_internal__params_on_add,
    .create       = _linux_dmabuf_internal__params_on_create,
    .create_immed = _linux_dmabuf_internal__params_on_create_immed,
};

static void _linux_dmabuf_internal__params_destroy_resource(struct wl_resource* r)
{
    struct _linux_dmabuf_internal__params* p = wl_resource_get_user_data(r);
    if (p != NULL)
    {
        //
        //If the client destroyed the params without ever calling
        //create/create_immed, any plane fds it added still belong
        //to us. Close them.
        //
        for (int i = 0; i < _LINUX_DMABUF_MAX_PLANES; ++i)
        {
            if (p->planes[i].fd >= 0)
            {
                close(p->planes[i].fd);
            }
        }
        GUI_FREE(p);
    }
}

static void _linux_dmabuf_internal__params_on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _linux_dmabuf_internal__params_on_add(struct wl_client* c, struct wl_resource* r,
                                                  int32_t fd, uint32_t plane_idx,
                                                  uint32_t offset, uint32_t stride,
                                                  uint32_t modifier_hi, uint32_t modifier_lo)
{
    (void)c;
    struct _linux_dmabuf_internal__params* p = wl_resource_get_user_data(r);

    //
    //Double-use check: if params already consumed, adding more
    //planes is a protocol error.
    //
    if (p->used)
    {
        wl_resource_post_error(r,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "dmabuf params: add() called after create/create_immed");
        close(fd);
        return;
    }

    if (plane_idx >= _LINUX_DMABUF_MAX_PLANES)
    {
        wl_resource_post_error(r,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "dmabuf params: plane_idx %u out of range (max %d)",
                               plane_idx, _LINUX_DMABUF_MAX_PLANES - 1);
        close(fd);
        return;
    }

    if (p->planes[plane_idx].fd >= 0)
    {
        wl_resource_post_error(r,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "dmabuf params: plane %u already set", plane_idx);
        close(fd);
        return;
    }

    p->planes[plane_idx].fd       = fd;
    p->planes[plane_idx].offset   = offset;
    p->planes[plane_idx].stride   = stride;
    p->planes[plane_idx].modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;

    log_trace("dmabuf params.add plane=%u fd=%d offset=%u stride=%u mod=0x%016llx",
              plane_idx, fd, offset, stride,
              (unsigned long long)p->planes[plane_idx].modifier);
}

//
//The core import routine. Builds the EGLAttrib list from the
//params state, calls eglCreateImage, wraps the result in a GL
//texture, returns both. Caller is responsible for destroying both
//on cleanup.
//
//Returns 0 on success, -1 on failure. On failure, *out_image /
//*out_tex are untouched.
//
static int _linux_dmabuf_internal__do_import(
    struct _linux_dmabuf_internal__params* p,
    int32_t w, int32_t h, uint32_t fourcc,
    EGLImage* out_image, GLuint* out_tex)
{
    //
    //Fixed-size attribute buffer. Worst case: base 6 entries
    //(fourcc/width/height + 3 per-plane entries for plane 0) + 6
    //per additional plane (EGL_DMA_BUF_PLANE<N>_* family) = ~30
    //EGLAttrib pairs. 64 slots is generous.
    //
    EGLAttrib attrs[64];
    int       a = 0;

    attrs[a++] = EGL_LINUX_DRM_FOURCC_EXT;  attrs[a++] = fourcc;
    attrs[a++] = EGL_WIDTH;                 attrs[a++] = w;
    attrs[a++] = EGL_HEIGHT;                attrs[a++] = h;

    //
    //Per-plane attributes. EGL uses family-of-four enum names per
    //plane index (PLANE0_FD / PLANE0_OFFSET / PLANE0_PITCH / PLANE0_MODIFIER_*).
    //PLANE1..3 have parallel names. Spelled out via a table here
    //because the ext headers don't give a clean array.
    //
    static const EGLAttrib plane_attr_fd[] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    static const EGLAttrib plane_attr_offset[] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    static const EGLAttrib plane_attr_pitch[] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    static const EGLAttrib plane_attr_mod_lo[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    static const EGLAttrib plane_attr_mod_hi[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    int planes_added = 0;
    for (int i = 0; i < _LINUX_DMABUF_MAX_PLANES; ++i)
    {
        if (p->planes[i].fd < 0)
        {
            continue;
        }
        attrs[a++] = plane_attr_fd[i];      attrs[a++] = p->planes[i].fd;
        attrs[a++] = plane_attr_offset[i];  attrs[a++] = p->planes[i].offset;
        attrs[a++] = plane_attr_pitch[i];   attrs[a++] = p->planes[i].stride;
        //
        //Explicit modifier is only legal when DRM_FORMAT_MOD_INVALID
        //is NOT what we got. Implicit (MOD_INVALID) skips the
        //modifier attributes entirely -- the driver picks whatever
        //tile layout the buffer was allocated with.
        //
        if (p->planes[i].modifier != DRM_FORMAT_MOD_INVALID)
        {
            attrs[a++] = plane_attr_mod_lo[i];
            attrs[a++] = (EGLAttrib)(p->planes[i].modifier & 0xffffffffu);
            attrs[a++] = plane_attr_mod_hi[i];
            attrs[a++] = (EGLAttrib)(p->planes[i].modifier >> 32);
        }
        planes_added++;
    }

    if (planes_added == 0)
    {
        log_error("dmabuf import: no planes set");
        return -1;
    }

    attrs[a++] = EGL_NONE;

    //
    //CRITICAL: make egl_ctx's context current before importing.
    //If output_x11's render tick ran just before this commit was
    //dispatched, THAT context would be current, and our
    //eglCreateImage would target the wrong display -- cross-
    //display resource use is UB per EGL spec.
    //
    if (egl_ctx__make_current() != 0)
    {
        log_error("dmabuf import: egl_ctx__make_current failed");
        return -1;
    }

    //
    //Actual import. EGL_NO_CONTEXT here is correct -- the image
    //is standalone; we bind it to a texture below.
    //
    EGLImage img = _linux_dmabuf_internal__create_image(
        egl_ctx__display(),
        EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT,
        NULL,
        attrs);
    if (img == EGL_NO_IMAGE)
    {
        log_error("dmabuf import: eglCreateImage failed (EGL error 0x%x)", eglGetError());
        return -1;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    _linux_dmabuf_internal__target_tex_2d(GL_TEXTURE_2D, (GLeglImageOES)img);

    GLenum gl_err = glGetError();
    if (gl_err != GL_NO_ERROR)
    {
        log_error("dmabuf import: glEGLImageTargetTexture2DOES GL error 0x%x", gl_err);
        glDeleteTextures(1, &tex);
        _linux_dmabuf_internal__destroy_image(egl_ctx__display(), img);
        return -1;
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    *out_image = img;
    *out_tex   = tex;
    return 0;
}

static const struct wl_buffer_interface _linux_dmabuf_internal__buffer_impl = {
    .destroy = _linux_dmabuf_internal__buffer_on_destroy,
};

static void _linux_dmabuf_internal__buffer_destroy_resource(struct wl_resource* r)
{
    struct _linux_dmabuf_internal__client_buffer* b = wl_resource_get_user_data(r);
    if (b == NULL)
    {
        return;
    }
    log_trace("dmabuf buffer destroy %p (tex=%u, %d planes)", (void*)b, b->gl_texture, b->plane_count);
    //
    //Same context-guard as import: glDeleteTextures operates on
    //whatever context is current; if output_x11's is current we'd
    //try to delete a texture that doesn't exist there (silent
    //no-op but the real texture leaks). eglDestroyImage on the
    //EGL display is safe regardless, but we guard uniformly.
    //
    (void)egl_ctx__make_current();
    if (b->gl_texture != 0)
    {
        glDeleteTextures(1, &b->gl_texture);
    }
    if (b->egl_image != EGL_NO_IMAGE)
    {
        _linux_dmabuf_internal__destroy_image(egl_ctx__display(), b->egl_image);
    }
    //
    //Close plane fds we've been holding alive so re-import paths
    //(like output_drm's scanout) could eglCreateImage them in a
    //different context. Note: EGL may hold internal dups, so
    //closing our side doesn't pull the rug out from under already-
    //imported EGLImages on either display.
    //
    for (int i = 0; i < b->plane_count; ++i)
    {
        if (b->planes[i].fd >= 0)
        {
            close(b->planes[i].fd);
            b->planes[i].fd = -1;
        }
    }
    GUI_FREE(b);
}

static void _linux_dmabuf_internal__buffer_on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

//
//Helper: close all plane fds on params. EGL either dup'd them
//(if import succeeded) or we own them (if import failed / params
//destroyed without use). Either way the server's ownership ends
//here.
//
static void _linux_dmabuf_internal__close_plane_fds(struct _linux_dmabuf_internal__params* p)
{
    for (int i = 0; i < _LINUX_DMABUF_MAX_PLANES; ++i)
    {
        if (p->planes[i].fd >= 0)
        {
            close(p->planes[i].fd);
            p->planes[i].fd = -1;
        }
    }
}

static void _linux_dmabuf_internal__params_on_create(struct wl_client* c, struct wl_resource* r,
                                                     int32_t w, int32_t h,
                                                     uint32_t fourcc, uint32_t flags)
{
    (void)c; (void)flags;
    struct _linux_dmabuf_internal__params* p = wl_resource_get_user_data(r);
    if (p->used)
    {
        wl_resource_post_error(r,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "dmabuf params: create() called after already used");
        return;
    }
    p->used = 1;

    EGLImage img = EGL_NO_IMAGE;
    GLuint   tex = 0;
    if (_linux_dmabuf_internal__do_import(p, w, h, fourcc, &img, &tex) != 0)
    {
        _linux_dmabuf_internal__close_plane_fds(p);
        zwp_linux_buffer_params_v1_send_failed(r);
        return;
    }

    struct _linux_dmabuf_internal__client_buffer* b =
        GUI_MALLOC_T(sizeof(*b), MM_TYPE_RENDERER);
    if (b == NULL)
    {
        glDeleteTextures(1, &tex);
        _linux_dmabuf_internal__destroy_image(egl_ctx__display(), img);
        _linux_dmabuf_internal__close_plane_fds(p);
        zwp_linux_buffer_params_v1_send_failed(r);
        return;
    }
    b->egl_image  = img;
    b->gl_texture = tex;
    b->w          = w;
    b->h          = h;
    b->fourcc     = fourcc;

    //
    //Transfer plane-fd ownership from params to the new buffer.
    //Counts non-(-1) fds so consumers know how many planes are real.
    //After this loop, params holds no live fds -- the explicit
    //close_plane_fds call below is a no-op for the transferred
    //entries and closes any straggling ones (there shouldn't be
    //any if add() was called in-order, but safe to call).
    //
    b->plane_count = 0;
    for (int i = 0; i < _LINUX_DMABUF_MAX_PLANES; ++i)
    {
        if (p->planes[i].fd >= 0)
        {
            b->planes[b->plane_count]          = p->planes[i];
            p->planes[i].fd                    = -1;
            b->plane_count++;
        }
    }

    //
    //Buffer id = 0 means server-allocated (libwayland returns a new
    //id). wl_buffer_interface is from wayland-server-protocol.h.
    //
    b->resource = wl_resource_create(c, &wl_buffer_interface, 1, 0);
    if (b->resource == NULL)
    {
        glDeleteTextures(1, &tex);
        _linux_dmabuf_internal__destroy_image(egl_ctx__display(), img);
        //
        //Close the plane fds we just transferred to b, since b is
        //being thrown away. Safe because we NULL'd p's fds above
        //so _close_plane_fds(p) won't double-close.
        //
        for (int i = 0; i < b->plane_count; ++i)
        {
            if (b->planes[i].fd >= 0) close(b->planes[i].fd);
        }
        GUI_FREE(b);
        _linux_dmabuf_internal__close_plane_fds(p);
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(b->resource,
                                   &_linux_dmabuf_internal__buffer_impl,
                                   b,
                                   _linux_dmabuf_internal__buffer_destroy_resource);

    zwp_linux_buffer_params_v1_send_created(r, b->resource);
    _linux_dmabuf_internal__close_plane_fds(p);   //no-op for transferred fds
    log_info("dmabuf params.create: imported %dx%d fourcc=0x%x tex=%u planes=%d",
             w, h, fourcc, tex, b->plane_count);
}

static void _linux_dmabuf_internal__params_on_create_immed(struct wl_client* c, struct wl_resource* r,
                                                           uint32_t buffer_id,
                                                           int32_t w, int32_t h,
                                                           uint32_t fourcc, uint32_t flags)
{
    (void)flags;
    struct _linux_dmabuf_internal__params* p = wl_resource_get_user_data(r);
    if (p->used)
    {
        wl_resource_post_error(r,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "dmabuf params: create_immed() called after already used");
        return;
    }
    p->used = 1;

    EGLImage img = EGL_NO_IMAGE;
    GLuint   tex = 0;
    if (_linux_dmabuf_internal__do_import(p, w, h, fourcc, &img, &tex) != 0)
    {
        _linux_dmabuf_internal__close_plane_fds(p);
        wl_resource_post_error(r,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                               "dmabuf params: import failed (see server log)");
        return;
    }

    struct _linux_dmabuf_internal__client_buffer* b =
        GUI_MALLOC_T(sizeof(*b), MM_TYPE_RENDERER);
    if (b == NULL)
    {
        glDeleteTextures(1, &tex);
        _linux_dmabuf_internal__destroy_image(egl_ctx__display(), img);
        _linux_dmabuf_internal__close_plane_fds(p);
        wl_client_post_no_memory(c);
        return;
    }
    b->egl_image  = img;
    b->gl_texture = tex;
    b->w          = w;
    b->h          = h;
    b->fourcc     = fourcc;

    //
    //Same plane-fd transfer as params_on_create. See comment there
    //for why we keep these alive past the validation import.
    //
    b->plane_count = 0;
    for (int i = 0; i < _LINUX_DMABUF_MAX_PLANES; ++i)
    {
        if (p->planes[i].fd >= 0)
        {
            b->planes[b->plane_count]          = p->planes[i];
            p->planes[i].fd                    = -1;
            b->plane_count++;
        }
    }

    //
    //Client-provided buffer_id this time (immed = synchronous).
    //
    b->resource = wl_resource_create(c, &wl_buffer_interface, 1, buffer_id);
    if (b->resource == NULL)
    {
        glDeleteTextures(1, &tex);
        _linux_dmabuf_internal__destroy_image(egl_ctx__display(), img);
        for (int i = 0; i < b->plane_count; ++i)
        {
            if (b->planes[i].fd >= 0) close(b->planes[i].fd);
        }
        GUI_FREE(b);
        _linux_dmabuf_internal__close_plane_fds(p);
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(b->resource,
                                   &_linux_dmabuf_internal__buffer_impl,
                                   b,
                                   _linux_dmabuf_internal__buffer_destroy_resource);

    _linux_dmabuf_internal__close_plane_fds(p);
    log_info("dmabuf params.create_immed: imported %dx%d fourcc=0x%x tex=%u",
             w, h, fourcc, tex);
}

// ============================================================
// zwp_linux_dmabuf_v1 top-level
// ============================================================

static const struct zwp_linux_dmabuf_v1_interface _linux_dmabuf_internal__dmabuf_impl = {
    .destroy              = _linux_dmabuf_internal__dmabuf_on_destroy,
    .create_params        = _linux_dmabuf_internal__dmabuf_on_create_params,
    .get_default_feedback = _linux_dmabuf_internal__dmabuf_on_get_default_feedback,
    .get_surface_feedback = _linux_dmabuf_internal__dmabuf_on_get_surface_feedback,
};

static void _linux_dmabuf_internal__dmabuf_on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _linux_dmabuf_internal__dmabuf_on_create_params(struct wl_client* c, struct wl_resource* r,
                                                            uint32_t id)
{
    (void)r;
    struct _linux_dmabuf_internal__params* p = GUI_MALLOC_T(sizeof(*p), MM_TYPE_RENDERER);
    if (p == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    p->used = 0;
    //
    //Initialize all plane fds to -1 so params_on_add can detect
    //"already set" and the import helper can detect "not set."
    //
    for (int i = 0; i < _LINUX_DMABUF_MAX_PLANES; ++i)
    {
        p->planes[i].fd = -1;
    }

    uint32_t version = wl_resource_get_version(r);
    p->resource = wl_resource_create(c, &zwp_linux_buffer_params_v1_interface, version, id);
    if (p->resource == NULL)
    {
        GUI_FREE(p);
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(p->resource,
                                   &_linux_dmabuf_internal__params_impl,
                                   p,
                                   _linux_dmabuf_internal__params_destroy_resource);
    log_trace("dmabuf.create_params -> %p (id=%u v=%u)", (void*)p, id, version);
}

static void _linux_dmabuf_internal__dmabuf_on_get_default_feedback(struct wl_client* c,
    struct wl_resource* r, uint32_t id)
{
    (void)c; (void)r; (void)id;
    //
    //v4+ feedback path. We advertise v3 so clients shouldn't call
    //this, but compliant servers still install the stub.
    //
    log_trace("dmabuf.get_default_feedback id=%u (stub)", id);
}

static void _linux_dmabuf_internal__dmabuf_on_get_surface_feedback(struct wl_client* c,
    struct wl_resource* r, uint32_t id, struct wl_resource* surface)
{
    (void)c; (void)r; (void)id; (void)surface;
    log_trace("dmabuf.get_surface_feedback id=%u (stub)", id);
}

//
//Bind handler: install the vtable, then burst out every supported
//format via format + modifier events. Legacy format-only clients
//(binding v1/v2) only see the format events; v3+ clients also get
//the modifier events, which carry the tiling info.
//
static void _linux_dmabuf_internal__dmabuf_bind(struct wl_client* client, void* data,
                                                uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* r = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
    if (r == NULL)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &_linux_dmabuf_internal__dmabuf_impl, NULL, NULL);

    //
    //Burst the format+modifier pairs. On bind, the server is
    //supposed to immediately push everything it can import so the
    //client sees the full list before its first buffer request.
    //
    for (int32_t i = 0; i < _linux_dmabuf_internal__n_formats; ++i)
    {
        struct _linux_dmabuf_internal__format_entry* fe = &_linux_dmabuf_internal__formats[i];

        //
        //Legacy format event: fires on every bound version, even
        //v3+ clients want it as a compatibility marker. Carries
        //just the fourcc (implicit modifier).
        //
        zwp_linux_dmabuf_v1_send_format(r, fe->drm_fourcc);

        //
        //Modifier events: v3-only. Each (fourcc, modifier) pair
        //shows up as one event. Clients use the union to decide
        //which (tile mode, format) they can produce.
        //
        if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION)
        {
            for (int32_t j = 0; j < fe->num_modifiers; ++j)
            {
                uint64_t m  = fe->modifiers[j];
                uint32_t hi = (uint32_t)(m >> 32);
                uint32_t lo = (uint32_t)(m & 0xffffffffu);
                zwp_linux_dmabuf_v1_send_modifier(r, fe->drm_fourcc, hi, lo);
            }
        }
    }
    log_trace("zwp_linux_dmabuf_v1 bind id=%u v=%u (%d formats burst)",
              id, version, _linux_dmabuf_internal__n_formats);
}

void linux_dmabuf__register(struct wl_display* display)
{
    //
    //Gate on EGL being up. Without an EGL display we can't import
    //dmabufs, so registering the global would be a lie.
    //
    if (egl_ctx__display() == EGL_NO_DISPLAY)
    {
        log_warn("linux_dmabuf: EGL not initialized; dmabuf global not registered "
                 "(clients will fall back to shm)");
        return;
    }
    if (!egl_ctx__extensions()->ext_image_dma_buf_import)
    {
        log_warn("linux_dmabuf: EGL_EXT_image_dma_buf_import missing; dmabuf global not registered");
        return;
    }

    //
    //Resolve the EGL + GLES extension entry points we need for
    //import. If any are missing the extension was a lie -- fail
    //register cleanly.
    //
    _linux_dmabuf_internal__create_image =
        (_fncp_eglCreateImage)eglGetProcAddress("eglCreateImage");
    _linux_dmabuf_internal__destroy_image =
        (_fncp_eglDestroyImage)eglGetProcAddress("eglDestroyImage");
    _linux_dmabuf_internal__target_tex_2d =
        (_fncp_glEGLImageTargetTexture2DOES)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (_linux_dmabuf_internal__create_image  == NULL ||
        _linux_dmabuf_internal__destroy_image == NULL ||
        _linux_dmabuf_internal__target_tex_2d == NULL)
    {
        log_warn("linux_dmabuf: required extension entry points missing "
                 "(eglCreateImage/eglDestroyImage/glEGLImageTargetTexture2DOES); "
                 "dmabuf global not registered");
        return;
    }

    _linux_dmabuf_internal__query_egl_formats();
    if (_linux_dmabuf_internal__n_formats == 0)
    {
        log_warn("linux_dmabuf: no advertisable formats; dmabuf global not registered");
        return;
    }

    //
    //v3 covers the modifier event (since v3) which is what modern
    //clients want. v4 adds the feedback object (shared-fd format
    //table); we'd bump when we want to stop bursting formats on
    //every bind.
    //
    struct wl_global* g = wl_global_create(display,
                                           &zwp_linux_dmabuf_v1_interface,
                                           /*version*/ 3,
                                           /*data*/    NULL,
                                           _linux_dmabuf_internal__dmabuf_bind);
    if (g == NULL)
    {
        log_fatal("wl_global_create(zwp_linux_dmabuf_v1) failed");
        _linux_dmabuf_internal__free_formats();
        return;
    }
    log_info("linux_dmabuf registered: zwp_linux_dmabuf_v1 v3, %d formats",
             _linux_dmabuf_internal__n_formats);
}

void linux_dmabuf__shutdown(void)
{
    //
    //Called from main.c after wl_display_run returns. Releases the
    //format+modifier cache allocated at register time. Idempotent:
    //if register bailed early (EGL missing, etc.), _formats is
    //NULL and free_formats is a no-op.
    //
    _linux_dmabuf_internal__free_formats();
}

//
// Public accessor for plane info of a wl_buffer we produced. Used
// by output_drm.c to re-import the buffer in its own EGL context
// (scanout). Returns 1 on a match (out filled in), 0 otherwise.
//
// Identity check: we use the wl_resource's implementation pointer
// equality against our buffer_impl. That's the canonical "was this
// resource made by us?" test in libwayland. A null resource or one
// with a foreign implementation returns 0 cleanly, which is what
// callers (output_drm.c) expect to mean "not a dmabuf, fall back
// to the shm path".
//
int linux_dmabuf__get_buffer_info(struct wl_resource* buffer_resource,
                                  struct linux_dmabuf_buffer_info* out)
{
    if (buffer_resource == NULL || out == NULL)
    {
        return 0;
    }
    if (wl_resource_instance_of(buffer_resource, &wl_buffer_interface,
                                &_linux_dmabuf_internal__buffer_impl) == 0)
    {
        return 0;
    }
    struct _linux_dmabuf_internal__client_buffer* b =
        wl_resource_get_user_data(buffer_resource);
    if (b == NULL)
    {
        return 0;
    }

    out->plane_count = b->plane_count;
    for (int i = 0; i < b->plane_count && i < LINUX_DMABUF_INFO_MAX_PLANES; ++i)
    {
        out->planes[i].fd       = b->planes[i].fd;
        out->planes[i].offset   = b->planes[i].offset;
        out->planes[i].stride   = b->planes[i].stride;
        out->planes[i].modifier = b->planes[i].modifier;
    }
    out->width  = b->w;
    out->height = b->h;
    out->fourcc = b->fourcc;
    return 1;
}
