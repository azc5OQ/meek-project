//
//surface.c -- wl_surface + wl_region + frame-callback subsystem.
//
//Moved out of globals.c (which had grown past 1700 lines). Owns:
//  * struct _globals_internal__surface and its list
//  * struct _globals_internal__region
//  * wl_surface.frame deferred callback plumbing
//  * wl_surface.commit -> output_drm / meek_shell_v1 forwarding
//  * per-surface role hooks (xdg_shell registers via
//    globals__wl_surface_set_role_hook, which delegates here)
//
//Naming kept as _globals_internal__ to minimize churn in cross-
//references; eventually renaming to _surface__ would be tidier.
//

#include <stdlib.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "types.h"
#include "third_party/log.h"
#include "clib/memory_manager.h"

#include "globals.h"
#include "surface.h"
#include "xdg_shell.h"
#include "egl_ctx.h"
#include "meek_shell_v1.h"
#include "output_drm.h"

//
//module-private surfaces list. Surfaces insert themselves here at
//create_surface time and remove themselves at destroy. Seat code
//queries via surface__first_resource_for_client().
//
static struct wl_list _globals_internal__surfaces;

//
//Per-surface state. See the comment block in the pre-split globals.c
//for the design rationale; briefly: pending + current buffer fields
//(atomic swap on commit), destroy listeners so we can null our
//pointers if a buffer resource dies before we're ready, a role-hook
//for xdg_shell to piggy-back on commits, a forwarded-this-vblank
//coalesce flag to cap toplevel_buffer emission at vblank rate, and
//two frame-callback lists (pending + current) for wl_surface.frame
//deferred done dispatch.
//
struct _globals_internal__surface
{
    struct wl_resource* resource;

    struct
    {
        struct wl_resource* buffer;
    } pending, current;

    struct wl_listener pending_buffer_destroy;
    struct wl_listener current_buffer_destroy;

    struct wl_resource* last_forwarded_buffer;

    int forwarded_this_vblank;
    //
    // wl_surface.enter is supposed to fire once when the surface
    // becomes visible on an output. We fire on the first commit
    // that attaches a buffer, then latch this flag so we don't
    // spam the event every frame.
    //
    int entered_output_sent;

    fncp_wl_surface_role_hook role_on_commit;
    void*                     role_data;

    struct wl_list link;

    struct wl_list frame_cbs_pending;
    struct wl_list frame_cbs_current;
};

struct _globals_internal__frame_cb
{
    struct wl_resource* resource;
    struct wl_list      link;
};

struct _globals_internal__region
{
    struct wl_resource* resource;
};

//-- forward decls for file-local statics --
static void _globals_internal__surface__destroy_resource(struct wl_resource* resource);
static void _globals_internal__surface__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _globals_internal__surface__on_attach(struct wl_client* c, struct wl_resource* r, struct wl_resource* buffer, int32_t x, int32_t y);
static void _globals_internal__surface__on_damage(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h);
static void _globals_internal__surface__on_frame(struct wl_client* c, struct wl_resource* r, uint32_t callback);
static void _globals_internal__surface__on_set_opaque_region(struct wl_client* c, struct wl_resource* r, struct wl_resource* region);
static void _globals_internal__surface__on_set_input_region(struct wl_client* c, struct wl_resource* r, struct wl_resource* region);
static void _globals_internal__surface__on_commit(struct wl_client* c, struct wl_resource* r);
static void _globals_internal__surface__on_set_buffer_transform(struct wl_client* c, struct wl_resource* r, int32_t transform);
static void _globals_internal__surface__on_set_buffer_scale(struct wl_client* c, struct wl_resource* r, int32_t scale);
static void _globals_internal__surface__on_damage_buffer(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h);
static void _globals_internal__surface__on_offset(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y);
static void _globals_internal__on_pending_buffer_destroy(struct wl_listener* l, void* data);
static void _globals_internal__on_current_buffer_destroy(struct wl_listener* l, void* data);
static void _globals_internal__frame_cb__destroy_resource(struct wl_resource* r);

static void _globals_internal__region__destroy_resource(struct wl_resource* resource);
static void _globals_internal__region__on_destroy(struct wl_client* c, struct wl_resource* r);
static void _globals_internal__region__on_add(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h);
static void _globals_internal__region__on_subtract(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h);

// ============================================================
// wl_surface
// ============================================================

static const struct wl_surface_interface _globals_internal__surface_impl = {
    .destroy              = _globals_internal__surface__on_destroy,
    .attach               = _globals_internal__surface__on_attach,
    .damage               = _globals_internal__surface__on_damage,
    .frame                = _globals_internal__surface__on_frame,
    .set_opaque_region    = _globals_internal__surface__on_set_opaque_region,
    .set_input_region     = _globals_internal__surface__on_set_input_region,
    .commit               = _globals_internal__surface__on_commit,
    .set_buffer_transform = _globals_internal__surface__on_set_buffer_transform,
    .set_buffer_scale     = _globals_internal__surface__on_set_buffer_scale,
    .damage_buffer        = _globals_internal__surface__on_damage_buffer,
    .offset               = _globals_internal__surface__on_offset,
};

static void _globals_internal__surface__destroy_resource(struct wl_resource* resource)
{
    struct _globals_internal__surface* s = wl_resource_get_user_data(resource);
    if (s != NULL)
    {
        if (s->pending.buffer != NULL)
        {
            wl_list_remove(&s->pending_buffer_destroy.link);
        }
        if (s->current.buffer != NULL)
        {
            wl_list_remove(&s->current_buffer_destroy.link);
        }
        wl_list_remove(&s->link);

        struct _globals_internal__frame_cb* fb;
        struct _globals_internal__frame_cb* tmp;
        wl_list_for_each_safe(fb, tmp, &s->frame_cbs_pending, link)
        {
            wl_resource_destroy(fb->resource);
        }
        wl_list_for_each_safe(fb, tmp, &s->frame_cbs_current, link)
        {
            wl_resource_destroy(fb->resource);
        }

        log_trace("wl_surface %p destroyed", (void*)s);
        GUI_FREE(s);
    }
}

static void _globals_internal__on_pending_buffer_destroy(struct wl_listener* l, void* data)
{
    (void)data;
    struct _globals_internal__surface* s =
        wl_container_of(l, s, pending_buffer_destroy);
    log_trace("wl_surface %p: pending buffer destroyed (client raced us)", (void*)s);
    s->pending.buffer = NULL;
}

static void _globals_internal__on_current_buffer_destroy(struct wl_listener* l, void* data)
{
    (void)data;
    struct _globals_internal__surface* s =
        wl_container_of(l, s, current_buffer_destroy);
    log_trace("wl_surface %p: current buffer destroyed (client raced us)", (void*)s);
    s->current.buffer = NULL;
    s->last_forwarded_buffer = NULL;
}

static void _globals_internal__surface__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _globals_internal__surface__on_attach(struct wl_client* c, struct wl_resource* r, struct wl_resource* buffer, int32_t x, int32_t y)
{
    (void)c;
    struct _globals_internal__surface* s = wl_resource_get_user_data(r);
    if (s->pending.buffer != NULL)
    {
        wl_list_remove(&s->pending_buffer_destroy.link);
    }
    s->pending.buffer = buffer;
    if (buffer != NULL)
    {
        s->pending_buffer_destroy.notify = _globals_internal__on_pending_buffer_destroy;
        wl_resource_add_destroy_listener(buffer, &s->pending_buffer_destroy);
    }
    log_trace("wl_surface.attach %p buffer=%p x=%d y=%d", (void*)s, (void*)buffer, x, y);
}

static void _globals_internal__surface__on_damage(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r;
    log_trace("wl_surface.damage %d,%d %dx%d", x, y, w, h);
}

static void _globals_internal__frame_cb__destroy_resource(struct wl_resource* r)
{
    struct _globals_internal__frame_cb* fb = wl_resource_get_user_data(r);
    if (fb == NULL) { return; }
    if (fb->link.prev != NULL && fb->link.next != NULL)
    {
        wl_list_remove(&fb->link);
    }
    GUI_FREE(fb);
}

static void _globals_internal__surface__on_frame(struct wl_client* c, struct wl_resource* r, uint32_t callback)
{
    struct _globals_internal__surface* s = wl_resource_get_user_data(r);

    struct _globals_internal__frame_cb* fb = GUI_CALLOC_T(1, sizeof(*fb), MM_TYPE_NODE);
    if (fb == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    fb->resource = wl_resource_create(c, &wl_callback_interface, 1, callback);
    if (fb->resource == NULL)
    {
        GUI_FREE(fb);
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(fb->resource,
                                   NULL,
                                   fb,
                                   _globals_internal__frame_cb__destroy_resource);
    wl_list_insert(s->frame_cbs_pending.prev, &fb->link);
    log_trace("wl_surface.frame callback=%u deferred (surface=%p)", callback, (void*)s);
}

static void _globals_internal__surface__on_set_opaque_region(struct wl_client* c, struct wl_resource* r, struct wl_resource* region)
{
    (void)c; (void)r; (void)region;
    log_trace("wl_surface.set_opaque_region (ignored)");
}

static void _globals_internal__surface__on_set_input_region(struct wl_client* c, struct wl_resource* r, struct wl_resource* region)
{
    (void)c; (void)r; (void)region;
    log_trace("wl_surface.set_input_region (ignored)");
}

static void _globals_internal__surface__on_commit(struct wl_client* c, struct wl_resource* r)
{
    struct _globals_internal__surface* s = wl_resource_get_user_data(r);

    if (s->current.buffer != NULL)
    {
        wl_list_remove(&s->current_buffer_destroy.link);
    }
    if (s->pending.buffer != NULL)
    {
        wl_list_remove(&s->pending_buffer_destroy.link);
    }
    s->current.buffer = s->pending.buffer;
    s->pending.buffer = NULL;
    if (s->current.buffer != NULL)
    {
        s->current_buffer_destroy.notify = _globals_internal__on_current_buffer_destroy;
        wl_resource_add_destroy_listener(s->current.buffer, &s->current_buffer_destroy);
    }

    if (!wl_list_empty(&s->frame_cbs_pending))
    {
        wl_list_insert_list(s->frame_cbs_current.prev, &s->frame_cbs_pending);
        wl_list_init(&s->frame_cbs_pending);
    }

    //
    // Fire wl_surface.enter(output) on the first commit that
    // attached a buffer. Some clients (GTK4 / libadwaita, amberol)
    // resolve pixel-density info from wl_output.scale + the
    // surface-to-output mapping declared by wl_surface.enter. If
    // we skip this, GTK4 tries to render without a resolved output
    // and SIGSEGVs after receiving fractional_scale.preferred_scale.
    //
    // Latched via s->entered_output_sent so we don't repeat it
    // every frame. Multi-output later would un-latch on output
    // change.
    //
    if (s->current.buffer != NULL && !s->entered_output_sent)
    {
        globals__send_output_enter_for_surface(r);
        s->entered_output_sent = 1;
    }

    if (c != NULL && c == meek_shell_v1__get_shell_client())
    {
        output_drm__on_shell_commit(s->current.buffer);
    }
    else if (s->current.buffer != NULL &&
             !s->forwarded_this_vblank)
    {
        //
        // Same-buffer gate: for dmabuf, re-attaching the same
        // wl_buffer means the shell's cached EGLImage is still
        // sampling the current GPU memory -- no need to re-forward.
        // For shm, the client typically re-uses a single shm
        // buffer and writes new pixels in-place every commit
        // (foot does exactly this for its terminal redraw). We
        // MUST re-forward so the compositor can re-copy the fresh
        // pixels into a new memfd for the shell.
        //
        // Detection: wl_shm_buffer_get returns non-NULL for shm
        // buffers only. dmabuf returns NULL.
        //
        boole is_shm = (wl_shm_buffer_get(s->current.buffer) != NULL);
        boole same_buffer = (s->current.buffer == s->last_forwarded_buffer);

        if (is_shm || !same_buffer)
        {
            uint32_t handle = xdg_shell__get_toplevel_handle_for_surface(r);
            if (handle != 0)
            {
                meek_shell_v1__fire_toplevel_buffer(handle, s->current.buffer);
                s->last_forwarded_buffer = s->current.buffer;
                s->forwarded_this_vblank = 1;
            }
        }
    }

    if (s->role_on_commit != NULL)
    {
        s->role_on_commit(s->role_data);
    }

    if (s->current.buffer == NULL)
    {
        log_trace("wl_surface.commit %p (no buffer; probably initial empty commit)", (void*)s);
        return;
    }

    //
    //shm upload path. Kept verbatim from pre-split globals.c. See
    //crucial_fixes.md entry on SCALE / TRANSFORM handling for why
    //we lock make_current + reset row-length after upload.
    //
    struct wl_shm_buffer* shm = wl_shm_buffer_get(s->current.buffer);
    if (shm != NULL)
    {
        int32_t  w      = wl_shm_buffer_get_width (shm);
        int32_t  h      = wl_shm_buffer_get_height(shm);
        int32_t  stride = wl_shm_buffer_get_stride(shm);
        uint32_t fmt    = wl_shm_buffer_get_format(shm);
        log_info("wl_surface.commit %p: shm %dx%d stride=%d fmt=0x%x",
                 (void*)s, w, h, stride, fmt);

        if (egl_ctx__display() != EGL_NO_DISPLAY)
        {
            if (egl_ctx__make_current() != 0)
            {
                log_error("shm upload: egl_ctx__make_current failed; skipping");
                goto shm_upload_done;
            }

            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

            glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);

            wl_shm_buffer_begin_access(shm);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE,
                         wl_shm_buffer_get_data(shm));
            wl_shm_buffer_end_access(shm);

            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

            GLenum err = glGetError();
            if (err == GL_NO_ERROR) { log_info("shm GL upload OK (tex=%u)", tex); }
            else                    { log_error("shm GL upload failed: GL error 0x%x", err); }

            glBindTexture(GL_TEXTURE_2D, 0);
            glDeleteTextures(1, &tex);

            wl_buffer_send_release(s->current.buffer);
        }
        shm_upload_done: ;
    }
    else
    {
        log_info("wl_surface.commit %p: dmabuf buffer (texture already resident)", (void*)s);
        wl_buffer_send_release(s->current.buffer);
    }
}

static void _globals_internal__surface__on_set_buffer_transform(struct wl_client* c, struct wl_resource* r, int32_t transform)
{
    (void)c;
    if (transform < 0 || transform > WL_OUTPUT_TRANSFORM_FLIPPED_270)
    {
        wl_resource_post_error(r,
                               WL_SURFACE_ERROR_INVALID_TRANSFORM,
                               "wl_surface.set_buffer_transform: invalid transform %d (must be 0..7)",
                               transform);
        return;
    }
    log_trace("wl_surface.set_buffer_transform %d", transform);
}

static void _globals_internal__surface__on_set_buffer_scale(struct wl_client* c, struct wl_resource* r, int32_t scale)
{
    (void)c;
    if (scale < 1)
    {
        wl_resource_post_error(r,
                               WL_SURFACE_ERROR_INVALID_SCALE,
                               "wl_surface.set_buffer_scale: invalid scale %d (must be >= 1)",
                               scale);
        return;
    }
    log_trace("wl_surface.set_buffer_scale %d", scale);
}

static void _globals_internal__surface__on_damage_buffer(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r;
    log_trace("wl_surface.damage_buffer %d,%d %dx%d", x, y, w, h);
}

static void _globals_internal__surface__on_offset(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y)
{
    (void)c; (void)r;
    log_trace("wl_surface.offset %d,%d", x, y);
}

// ============================================================
// wl_region
// ============================================================

static const struct wl_region_interface _globals_internal__region_impl = {
    .destroy  = _globals_internal__region__on_destroy,
    .add      = _globals_internal__region__on_add,
    .subtract = _globals_internal__region__on_subtract,
};

static void _globals_internal__region__destroy_resource(struct wl_resource* resource)
{
    struct _globals_internal__region* rg = wl_resource_get_user_data(resource);
    if (rg != NULL)
    {
        log_trace("wl_region %p destroyed", (void*)rg);
        GUI_FREE(rg);
    }
}

static void _globals_internal__region__on_destroy(struct wl_client* c, struct wl_resource* r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void _globals_internal__region__on_add(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r;
    log_trace("wl_region.add %d,%d %dx%d", x, y, w, h);
}

static void _globals_internal__region__on_subtract(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r;
    log_trace("wl_region.subtract %d,%d %dx%d", x, y, w, h);
}

// ============================================================
// Public entry points (surface.h)
// ============================================================

void surface__init(void)
{
    wl_list_init(&_globals_internal__surfaces);
}

void surface__create(struct wl_client* c, uint32_t version, uint32_t id)
{
    struct _globals_internal__surface* s = GUI_CALLOC_T(1, sizeof(*s), MM_TYPE_NODE);
    if (s == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }

    s->resource = wl_resource_create(c, &wl_surface_interface, version, id);
    if (s->resource == NULL)
    {
        GUI_FREE(s);
        wl_client_post_no_memory(c);
        return;
    }

    wl_resource_set_implementation(s->resource,
                                   &_globals_internal__surface_impl,
                                   s,
                                   _globals_internal__surface__destroy_resource);

    wl_list_init(&s->frame_cbs_pending);
    wl_list_init(&s->frame_cbs_current);
    wl_list_insert(&_globals_internal__surfaces, &s->link);

    log_trace("wl_compositor.create_surface -> %p (id=%u v=%u)", (void*)s, id, version);
}

void surface__create_region(struct wl_client* c, uint32_t version, uint32_t id)
{
    struct _globals_internal__region* rg = GUI_MALLOC_T(sizeof(*rg), MM_TYPE_NODE);
    if (rg == NULL)
    {
        wl_client_post_no_memory(c);
        return;
    }
    rg->resource = wl_resource_create(c, &wl_region_interface, version, id);
    if (rg->resource == NULL)
    {
        GUI_FREE(rg);
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(rg->resource,
                                   &_globals_internal__region_impl,
                                   rg,
                                   _globals_internal__region__destroy_resource);
    log_trace("wl_compositor.create_region -> %p (id=%u v=%u)", (void*)rg, id, version);
}

struct wl_resource* surface__first_resource_for_client(struct wl_client* c)
{
    if (c == NULL) { return NULL; }
    struct _globals_internal__surface* s;
    wl_list_for_each(s, &_globals_internal__surfaces, link)
    {
        if (wl_resource_get_client(s->resource) == c)
        {
            return s->resource;
        }
    }
    return NULL;
}

// ============================================================
// Delegates from globals.h (public API kept stable)
// ============================================================

void globals__wl_surface_set_role_hook(struct wl_resource*        wl_surface,
                                       fncp_wl_surface_role_hook  on_commit,
                                       void*                      role_data)
{
    if (wl_surface == NULL)
    {
        log_error("globals__wl_surface_set_role_hook: NULL wl_surface");
        return;
    }
    struct _globals_internal__surface* s = wl_resource_get_user_data(wl_surface);
    if (s == NULL)
    {
        log_error("globals__wl_surface_set_role_hook: wl_surface user_data is NULL");
        return;
    }
    s->role_on_commit = on_commit;
    s->role_data      = role_data;
}

void globals__fire_frame_callbacks(uint32_t time_ms)
{
    int fired = 0;
    struct _globals_internal__surface* s;
    wl_list_for_each(s, &_globals_internal__surfaces, link)
    {
        s->forwarded_this_vblank = 0;

        struct _globals_internal__frame_cb* fb;
        struct _globals_internal__frame_cb* tmp;
        wl_list_for_each_safe(fb, tmp, &s->frame_cbs_current, link)
        {
            wl_list_remove(&fb->link);
            wl_list_init(&fb->link);
            wl_callback_send_done(fb->resource, time_ms);
            wl_resource_destroy(fb->resource);
            fired++;
        }
    }
    if (fired > 0)
    {
        log_trace("globals: fired %d frame callbacks at t=%u", fired, time_ms);
    }
}
