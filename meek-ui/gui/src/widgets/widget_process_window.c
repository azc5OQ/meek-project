//
// widget_process_window.c - <process-window/> draws another process's
// rendered buffer as a textured rectangle inside our scene.
//
// Canonical consumer: meek-shell. meek-shell receives dmabuf fds from
// meek-compositor via the meek_shell_v1 extension, imports them into
// GL textures (in meek-ui's EGL context so we can sample them here),
// and attaches each live texture to a <process-window> node placed
// wherever the shell wants the "app's window" to appear -- as a full-
// screen foreground, as a miniaturized task-switcher tile, whatever.
//
// Unlike widget_image, this widget does NOT own the texture. The
// texture lifecycle belongs to meek-shell's toplevel_registry. We
// just store the handle, the dimensions, and draw it on demand. If
// the registry replaces the texture (every commit from the source
// app, i.e. ~60 Hz), shell calls widget_process_window__set_texture
// on this node to point at the new GLuint. Old handles are freed by
// the registry, not by us.
//
// Pass: E4 (Phase 3 of the task-switcher + gestures feature pack).
// See meek-shell/session/roadmap.md section "Track E".
//

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "renderer.h"
#include "scene.h"   //scene__emit_default_bg + scene__frame_time_ms.
#include "third_party/log.h"
#include "clib/memory_manager.h"
#include "widget_process_window.h"

//
// Per-node state. `gl_texture == 0` means "no buffer yet" -- we draw
// the node's bg rect in that case so a freshly-spawned tile isn't
// suddenly a black void before the first commit arrives.
//
typedef struct _widget_process_window_internal__state
{
    uint32_t gl_texture;   //GL name cast from meek-shell's EGLImage import. 0 = unset.
    int      width;        //source texture width in pixels.
    int      height;       //source texture height in pixels.
    uint32_t handle;       //meek_shell_v1 toplevel handle this widget tracks (informational, not used for draw).
} _widget_process_window_internal__state;

static _widget_process_window_internal__state* _widget_process_window_internal__state_of(gui_node* n)
{
    if (n->user_data == NULL)
    {
        //
        // Lazy alloc, same pattern as widget_image. Nodes that never
        // get a handle set don't leak an empty state struct.
        //
        n->user_data = GUI_CALLOC_T(1, sizeof(_widget_process_window_internal__state), MM_TYPE_NODE);
    }
    return (_widget_process_window_internal__state*)n->user_data;
}

//
// Parse a positive integer out of a decimal string. Returns 0 on
// parse failure, which is our sentinel for "no handle set" -- a
// malformed handle="" attribute is functionally indistinguishable
// from an unset one, which is fine for our purposes.
//
static uint32_t _widget_process_window_internal__parse_u32(const char* s)
{
    if (s == NULL) return 0;
    uint32_t acc = 0;
    for (const char* p = s; *p != 0; p++)
    {
        if (*p < '0' || *p > '9') return 0;
        acc = acc * 10u + (uint32_t)(*p - '0');
    }
    return acc;
}

static boole process_window_apply_attribute(gui_node* n, char* name, char* value)
{
    //
    // `handle="42"` binds this node to the meek_shell_v1 toplevel
    // with that handle. The texture isn't available until the shell
    // calls widget_process_window__set_texture -- this just records
    // which process the node is waiting for.
    //
    if (strcmp(name, "handle") == 0)
    {
        _widget_process_window_internal__state* st = _widget_process_window_internal__state_of(n);
        if (st == NULL) return TRUE;
        st->handle = _widget_process_window_internal__parse_u32(value);
        return TRUE;
    }
    return FALSE;
}

static void process_window_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style* s = &n->resolved;

    //
    // Size priority: explicit style size > source-texture natural
    // size > avail. The source texture's natural size (set via
    // set_texture from the shell) represents the app's preferred
    // window dimensions; we honor them if no style size overrides.
    //
    _widget_process_window_internal__state* st = (_widget_process_window_internal__state*)n->user_data;

    //
    // Size priority: explicit pixel (size_w/h) > percent of parent
    // (width_pct / height_pct) > source-texture natural size > avail.
    // Percent is what the shell uses for fullscreen presentation
    // (".fullscreen-app { width: 100%; height: 100%; }"); without it
    // we fall through to st->width which is the panel-native texture
    // size and can overflow the logical-size shell layout.
    //
    float w;
    if (s->size_w > 0.0f)            { w = s->size_w * scale; }
    else if (s->width_pct > 0.0f)    { w = avail_w * (s->width_pct / 100.0f); }
    else if (st != NULL && st->width > 0)
    {
        //
        // Natural-size fallback. Source textures come from the
        // compositor in panel pixels; do NOT multiply by scale --
        // the source already rendered at its preferred size.
        //
        w = (float)st->width;
    }
    else
    {
        w = avail_w;
    }

    float h;
    if (s->size_h > 0.0f)             { h = s->size_h * scale; }
    else if (s->height_pct > 0.0f)    { h = avail_h * (s->height_pct / 100.0f); }
    else if (st != NULL && st->height > 0)
    {
        h = (float)st->height;
    }
    else
    {
        h = avail_h;
    }

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static void process_window_emit_draws(gui_node* n, float scale)
{
    gui_style* s = &n->resolved;
    _widget_process_window_internal__state* st = (_widget_process_window_internal__state*)n->user_data;

    //
    // Background + box-shadow + border via the standard helper so
    // the runtime effective_opacity cascade reaches us. Calling
    // submit_rect directly skipped the opacity multiply, which made
    // a fading parent (e.g. the task-switcher overlay during a
    // swipe-progress slide) leave the tile bg fully opaque while
    // the surrounding chrome dimmed. With emit_default_bg the bg /
    // shadow / border all multiply in op = effective_opacity, the
    // tile fades together with its backdrop.
    //
    scene__emit_default_bg(n, scale);

    if (st == NULL || st->gl_texture == 0)
    {
        //
        // No texture yet. Render stops at the bg rect. Common case
        // during the single-frame gap between toplevel_added (node
        // created in shell) and the first toplevel_buffer (texture
        // set).
        //
        return;
    }

    //
    // Tint via accent_color (matches widget_image's behavior). Pure
    // black = sentinel for "no tint", use neutral white. Also
    // respect effective_opacity so scene-level fade animations work.
    //
    gui_color tint;
    if (s->accent_color.r == 0.0f && s->accent_color.g == 0.0f && s->accent_color.b == 0.0f && s->accent_color.a == 0.0f)
    {
        tint.r = 1.0f; tint.g = 1.0f; tint.b = 1.0f; tint.a = 1.0f;
    }
    else
    {
        tint = s->accent_color;
    }
    tint.a *= n->effective_opacity;

    //
    // Cast GLuint back to the void* form renderer__submit_image
    // accepts. The gles3 backend stashes a GLuint as (void*)(uintptr_t)
    // and unwraps the same way at sample time -- see
    // renderer__submit_image + renderer__create_texture_rgba in
    // gles3_renderer.c. For backends with no image pipeline (d3d11 /
    // d3d9 stubs return NULL from renderer__create_texture_rgba),
    // submit_image silently no-ops, so the bg rect is all you see.
    //
    void* tex_as_void = (void*)(uintptr_t)st->gl_texture;
    renderer__submit_image(n->bounds, tex_as_void, tint);

    //
    // Throttled diag: log effective_opacity once per ~1s when it's
    // visibly in fade range so we can confirm the cascade reaches
    // the texture (vs being stuck at 1.0). Disabled when fully
    // opaque -- only fires during the swipe-progress reveal.
    //
    {
        static int64 last_log_ms = 0;
        if (n->effective_opacity < 0.99f)
        {
            int64 now_ms = scene__frame_time_ms();
            if (now_ms - last_log_ms > 500)
            {
                last_log_ms = now_ms;
                log_info("[dbg-fade] process-window effective_op=%.3f tint.a=%.3f bounds=(%.0f,%.0f %.0fx%.0f)",
                         n->effective_opacity, tint.a,
                         n->bounds.x, n->bounds.y, n->bounds.w, n->bounds.h);
            }
        }
    }
}

static void process_window_on_destroy(gui_node* n)
{
    //
    // We don't own the texture -- meek-shell's toplevel_registry
    // does. Just free our state struct. Shell's
    // toplevel_registry__remove handles glDeleteTextures when the
    // source process exits.
    //
    if (n->user_data != NULL)
    {
        GUI_FREE(n->user_data);
        n->user_data = NULL;
    }
}

static const widget_vtable g_process_window_vtable = {
    .type_name       = "process-window",
    .apply_attribute = process_window_apply_attribute,
    .layout          = process_window_layout,
    .emit_draws      = process_window_emit_draws,
    .on_destroy      = process_window_on_destroy,
};

void widget_process_window__register(void)
{
    widget_registry__register(GUI_NODE_PROCESS_WINDOW, &g_process_window_vtable);
}

//============================================================================
// public API -- shell calls these to bind a node to a live texture.
//============================================================================

void widget_process_window__set_texture(gui_node* n, uint32_t gl_texture, int width, int height)
{
    if (n == NULL) return;
    if (n->type != GUI_NODE_PROCESS_WINDOW)
    {
        log_warn("widget_process_window__set_texture: node is not <process-window> (type=%d)", (int)n->type);
        return;
    }
    _widget_process_window_internal__state* st = _widget_process_window_internal__state_of(n);
    if (st == NULL) return;
    st->gl_texture = gl_texture;
    st->width      = width;
    st->height     = height;
}

uint32_t widget_process_window__get_handle(gui_node* n)
{
    if (n == NULL || n->type != GUI_NODE_PROCESS_WINDOW) return 0;
    _widget_process_window_internal__state* st = (_widget_process_window_internal__state*)n->user_data;
    return st == NULL ? 0 : st->handle;
}

void widget_process_window__set_handle(gui_node* n, uint32_t handle)
{
    if (n == NULL || n->type != GUI_NODE_PROCESS_WINDOW) return;
    _widget_process_window_internal__state* st = _widget_process_window_internal__state_of(n);
    if (st == NULL) return;
    st->handle = handle;
}
