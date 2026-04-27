//
//scene_render.c -- draw emission split out of scene.c. Owns:
//  * scene__submit_fitted_image   (public helper for fit modes)
//  * scene__border_width          (public helper to query border width)
//  * scene__emit_border           (public helper)
//  * scene__emit_default_bg       (public helper widgets call)
//  * recursive tree walk with z-index sort + opacity cascade
//
//scene__emit_draws itself stays in scene.c (it drives root + overlay).
//Widgets call scene__emit_default_bg / scene__emit_border from their
//own vtable emit_draws; the recursive walker here calls vt->emit_draws
//for each node that has one, or scene__emit_default_bg otherwise.
//

#include <stdlib.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "renderer.h"
#include "scene.h"
#include "scene_render.h"
#include "widgets/widget_image_cache.h"

//
// Default bg emission. Honors box-shadow (behind bg), bg-gradient
// (replaces bg when set), and opacity (multiplies every alpha).
//
static void _scene_render_internal__emit_default(gui_node* n, float scale)
{
    scene__emit_default_bg(n, scale);
}

void scene__submit_fitted_image(gui_rect bounds, void* tex, int natural_w, int natural_h, int fit, gui_color tint, float radius)
{
    if (tex == NULL)
    {
        gui_color a; a.r = 0.20f; a.g = 0.22f; a.b = 0.27f; a.a = tint.a;
        gui_color b; b.r = 0.10f; b.g = 0.11f; b.b = 0.14f; b.a = tint.a;
        renderer__submit_rect_gradient(bounds, a, b, (int)GUI_GRADIENT_DIAGONAL_TL, radius);
        return;
    }

    gui_rect dest = bounds;
    boole   need_scissor = FALSE;

    if (natural_w > 0 && natural_h > 0 && bounds.w > 0.0f && bounds.h > 0.0f)
    {
        float nw = (float)natural_w;
        float nh = (float)natural_h;
        if (fit == GUI_FIT_CONTAIN)
        {
            float sx = bounds.w / nw;
            float sy = bounds.h / nh;
            float s  = (sx < sy) ? sx : sy;
            float dw = nw * s;
            float dh = nh * s;
            dest.x = bounds.x + (bounds.w - dw) * 0.5f;
            dest.y = bounds.y + (bounds.h - dh) * 0.5f;
            dest.w = dw;
            dest.h = dh;
        }
        else if (fit == GUI_FIT_COVER)
        {
            float sx = bounds.w / nw;
            float sy = bounds.h / nh;
            float s  = (sx > sy) ? sx : sy;
            float dw = nw * s;
            float dh = nh * s;
            dest.x = bounds.x + (bounds.w - dw) * 0.5f;
            dest.y = bounds.y + (bounds.h - dh) * 0.5f;
            dest.w = dw;
            dest.h = dh;
            need_scissor = TRUE;
        }
        else if (fit == GUI_FIT_NONE)
        {
            dest.x = bounds.x + (bounds.w - nw) * 0.5f;
            dest.y = bounds.y + (bounds.h - nh) * 0.5f;
            dest.w = nw;
            dest.h = nh;
            need_scissor = (boole)(nw > bounds.w || nh > bounds.h);
        }
    }

    if (need_scissor) { renderer__push_scissor(bounds); }
    renderer__submit_image(dest, tex, tint);
    if (need_scissor) { renderer__pop_scissor(); }
    (void)radius;
}

float scene__border_width(gui_node* n, float scale)
{
    if (n == NULL) { return 0.0f; }
    gui_style* s = &n->resolved;
    if (s->border_width <= 0.0f)            { return 0.0f; }
    if (s->border_style == GUI_BORDER_NONE) { return 0.0f; }
    if (!s->has_border_color)               { return 0.0f; }
    float bw = s->border_width * scale;
    if (bw < 0.0f) { bw = 0.0f; }
    return bw;
}

void scene__emit_border(gui_node* n, gui_rect bounds, float scale)
{
    if (n == NULL) { return; }
    gui_style* s = &n->resolved;

    if (s->border_width <= 0.0f)            { return; }
    if (s->border_style == GUI_BORDER_NONE) { return; }
    if (!s->has_border_color)               { return; }

    float bw = s->border_width * scale;
    if (bw <= 0.0f) { return; }

    float half_min = (bounds.w < bounds.h ? bounds.w : bounds.h) * 0.5f;
    if (bw > half_min) { bw = half_min; }

    float outer_r = s->radius * scale;
    float inner_r = outer_r - bw;
    if (inner_r < 0.0f) { inner_r = 0.0f; }

    float op = n->effective_opacity;
    gui_color border = s->border_color;
    border.a *= op;

    if (s->has_border_gradient)
    {
        gui_color from = s->border_gradient_from;
        gui_color to   = s->border_gradient_to;
        from.a *= op;
        to.a   *= op;
        renderer__submit_rect_gradient(bounds, from, to, (int)s->border_gradient_dir, outer_r);
    }
    else
    {
        renderer__submit_rect(bounds, border, outer_r);
    }

    gui_rect inner;
    inner.x = bounds.x + bw;
    inner.y = bounds.y + bw;
    inner.w = bounds.w - 2.0f * bw;
    inner.h = bounds.h - 2.0f * bw;
    if (inner.w <= 0.0f || inner.h <= 0.0f) { return; }

    if (s->has_background_color)
    {
        gui_color fill = s->background_color;
        fill.a *= op;
        renderer__submit_rect(inner, fill, inner_r);
    }
    else
    {
        gui_color clear_c = { 0.0f, 0.0f, 0.0f, 0.0f };
        renderer__submit_rect(inner, clear_c, inner_r);
    }
}

void scene__emit_default_bg(gui_node* n, float scale)
{
    gui_style* s = &n->resolved;
    float r  = s->radius * scale;
    float op = n->effective_opacity;

    if (s->has_shadow && s->shadow_color.a > 0.0f)
    {
        gui_rect sh;
        sh.x = n->bounds.x + s->shadow_dx;
        sh.y = n->bounds.y + s->shadow_dy;
        sh.w = n->bounds.w;
        sh.h = n->bounds.h;
        gui_color sc = s->shadow_color;
        sc.a *= op;
        renderer__submit_shadow(sh, sc, r, s->shadow_blur);
    }

    if (s->has_bg_gradient)
    {
        gui_color a = s->bg_gradient_from;
        gui_color b = s->bg_gradient_to;
        a.a *= op;
        b.a *= op;
        renderer__submit_rect_gradient(n->bounds, a, b, (int)s->bg_gradient_dir, r);
    }
    else if (s->has_background_color)
    {
        gui_color c = s->background_color;
        c.a *= op;
        renderer__submit_rect(n->bounds, c, r);
    }

    scene__emit_border(n, n->bounds, scale);

    if (s->has_background_image && s->background_image[0] != 0)
    {
        int nw = 0, nh = 0;
        void* tex = widget_image__cache_get_or_load(s->background_image, &nw, &nh);
        gui_color tint;
        tint.r = 1.0f; tint.g = 1.0f; tint.b = 1.0f; tint.a = op;
        scene__submit_fitted_image(n->bounds, tex, nw, nh, (int)s->background_size, tint, r);
    }

    if (s->blur_px > 0.0f && op > 0.0f)
    {
        renderer__blur_region(n->bounds, s->blur_px);
    }
}

//
// Sibling-only z-index sort. Gathers children into a scratch array
// (stable insertion sort keeps original order for equal z_index),
// walks in the sorted order. Bounded by MAX_Z_SORT; deeper sibling
// counts fall back to unsorted.
//
#define _SCENE_RENDER_INTERNAL__MAX_Z_SORT 256

static void _scene_render_internal__emit_recursive(gui_node* n, float scale);

static void _scene_render_internal__emit_children_sorted(gui_node* parent, float scale)
{
    int64 count = 0;
    for (gui_node* c = parent->first_child; c != NULL; c = c->next_sibling) { count++; }
    if (count <= 1 || count > _SCENE_RENDER_INTERNAL__MAX_Z_SORT)
    {
        for (gui_node* c = parent->first_child; c != NULL; c = c->next_sibling)
        {
            _scene_render_internal__emit_recursive(c, scale);
        }
        return;
    }

    boole any_z = FALSE;
    for (gui_node* c = parent->first_child; c != NULL; c = c->next_sibling)
    {
        if (c->resolved.z_index != 0) { any_z = TRUE; break; }
    }
    if (!any_z)
    {
        for (gui_node* c = parent->first_child; c != NULL; c = c->next_sibling)
        {
            _scene_render_internal__emit_recursive(c, scale);
        }
        return;
    }

    gui_node* slots[_SCENE_RENDER_INTERNAL__MAX_Z_SORT];
    int64 n = 0;
    for (gui_node* c = parent->first_child; c != NULL; c = c->next_sibling) { slots[n++] = c; }

    for (int64 i = 1; i < n; i++)
    {
        gui_node* key = slots[i];
        int64 j = i - 1;
        while (j >= 0 && slots[j]->resolved.z_index > key->resolved.z_index)
        {
            slots[j + 1] = slots[j];
            j--;
        }
        slots[j + 1] = key;
    }
    for (int64 i = 0; i < n; i++)
    {
        //
        // Z-boundary flush. The renderer batches text into a separate
        // late pass (text-on-top-of-rects within a flush). If we let
        // a whole frame accumulate, a lower-z sibling's text would
        // paint on top of a higher-z sibling's rects, which is the
        // opposite of what the z-index requested. Flushing at every
        // boundary where z_index actually changes keeps each z layer
        // self-contained: rects + their own text, then the next layer
        // paints on top of the composited earlier layer.
        //
        if (i > 0 && slots[i]->resolved.z_index != slots[i - 1]->resolved.z_index)
        {
            renderer__flush_pending_draws();
        }
        _scene_render_internal__emit_recursive(slots[i], scale);
    }
}

static void _scene_render_internal__emit_recursive(gui_node* n, float scale)
{
    if (n == NULL) { return; }
    if (n->resolved.display == GUI_DISPLAY_NONE) { return; }

    float parent_op = 1.0f;
    if (n->parent != NULL) { parent_op = n->parent->effective_opacity; }
    float own_op = 1.0f;
    if (n->resolved.has_opacity)
    {
        own_op = n->resolved.opacity;
        if (own_op < 0.0f) { own_op = 0.0f; }
        if (own_op > 1.0f) { own_op = 1.0f; }
    }
    n->effective_opacity = parent_op * own_op;

    const widget_vtable* vt = widget_registry__get(n->type);

    if (n->resolved.visibility != GUI_VISIBILITY_HIDDEN)
    {
        if (vt != NULL && vt->emit_draws != NULL) { vt->emit_draws(n, scale); }
        else                                       { _scene_render_internal__emit_default(n, scale); }
    }

    _scene_render_internal__emit_children_sorted(n, scale);

    if (n->resolved.visibility != GUI_VISIBILITY_HIDDEN &&
        vt != NULL && vt->emit_draws_post != NULL)
    {
        vt->emit_draws_post(n, scale);
    }
}

void scene_render__emit_tree(gui_node* root, float scale)
{
    if (root == NULL) { return; }
    root->effective_opacity = 1.0f;
    _scene_render_internal__emit_recursive(root, scale);
}
