//
// widget_window.c - root container that fills the viewport.
//
// Window's bounds are the full viewport; no per-Window styling
// governs its size. It lays out children as a column inside its
// content rect (honoring its own pad / gap).
//
// SCROLLING (whole-page scrollbar)
// --------------------------------
// Window now honors the same overflow_x / overflow_y style as <div>
// via the shared scroll module. Set `overflow_y: auto` (or `scroll`)
// on Window and content taller than the viewport scrolls with a
// draggable scrollbar on the right edge -- the "browser-style" page
// scrollbar.
//
// Layout flow mirrors widget_div: speculative pass measures content,
// then a real pass with width narrowed by the scrollbar slot if a
// bar will be visible. scroll__clamp keeps scroll_y inside the legal
// range across viewport resizes and content changes.
//
// Default: overflow stays VISIBLE (no scrollbar) so existing scenes
// without an explicit overflow declaration behave exactly as before.
//

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "scroll.h"

//
// Same logic as widget_div's needs_clip: clip whenever a scrollbar is
// (or could be) involved, OR overflow is HIDDEN. Avoids pushing
// scissor when overflow is plain VISIBLE so the renderer's flush
// count stays low for the common case.
//
static boole _widget_window_internal__needs_clip(gui_node* n)
{
    gui_overflow ox = n->resolved.overflow_x;
    gui_overflow oy = n->resolved.overflow_y;
    if (ox == GUI_OVERFLOW_HIDDEN || ox == GUI_OVERFLOW_SCROLL) { return TRUE; }
    if (oy == GUI_OVERFLOW_HIDDEN || oy == GUI_OVERFLOW_SCROLL) { return TRUE; }
    if (ox == GUI_OVERFLOW_AUTO || oy == GUI_OVERFLOW_AUTO)
    {
        return (boole)(scroll__vbar_visible(n) || scroll__hbar_visible(n));
    }
    return FALSE;
}

//
// Scroll-aware column layout. Same shape as widget_div's helper -- if
// we end up needing a third scrollable container we should factor this
// into scene.c, but two copies is cheaper than a premature abstraction.
//
static void _widget_window_internal__layout_children(gui_node* parent, float scale, float content_w)
{
    gui_style* s  = &parent->resolved;
    //
    // Same border-aware shrink as widget_div uses; see comment there.
    //
    float      bw = scene__border_width(parent, scale);
    float      cx = parent->bounds.x + s->pad_l * scale + bw;
    float      cy = parent->bounds.y + s->pad_t * scale + bw;
    float      ch = parent->bounds.h - (s->pad_t + s->pad_b) * scale - 2.0f * bw;
    float      cw = content_w        - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    if (cw < 0.0f) { cw = 0.0f; }
    if (ch < 0.0f) { ch = 0.0f; }

    float x_origin       = cx - parent->scroll_x;
    float y_cursor       = cy - parent->scroll_y;
    float content_top    = cy;
    float content_bottom = cy;
    float content_right  = cx;

    gui_node* c = parent->first_child;
    while (c != NULL)
    {
        //
        // display: none children take no space in the page-level
        // scroll column, same rule as everywhere else.
        //
        if (c->resolved.display == GUI_DISPLAY_NONE)
        {
            c = c->next_sibling;
            continue;
        }
        //
        // position: absolute / fixed children are lifted out of
        // the page-level flow. Skip here; they get a separate
        // post-pass below against the right containing block:
        // absolute -> this window's content rect, fixed -> the
        // viewport (which IS this window's bounds at the root).
        //
        if (c->resolved.position == GUI_POSITION_ABSOLUTE
         || c->resolved.position == GUI_POSITION_FIXED)
        {
            c = c->next_sibling;
            continue;
        }
        //
        // Per-child margins (mirrors scene__layout_children_as_column).
        //
        float m_t = c->resolved.margin_t * scale;
        float m_b = c->resolved.margin_b * scale;
        float m_l = c->resolved.margin_l * scale;
        float m_r = c->resolved.margin_r * scale;
        y_cursor += m_t;

        float child_x  = x_origin + m_l;
        float child_cw = cw - (m_l + m_r);
        if (child_cw < 0.0f) { child_cw = 0.0f; }

        scene__layout_node(c, child_x, y_cursor, child_cw, ch - (y_cursor - cy));
        //
        // position: relative -- shift the laid-out box without
        // affecting the cursor. Mirrors the column-flow pattern in
        // scene_layout.c.
        //
        if (c->resolved.position == GUI_POSITION_RELATIVE)
        {
            float dx_rel = (c->resolved.inset_l - c->resolved.inset_r) * scale;
            float dy_rel = (c->resolved.inset_t - c->resolved.inset_b) * scale;
            if (dx_rel != 0.0f || dy_rel != 0.0f)
            {
                scene__shift_tree(c, dx_rel, dy_rel);
            }
        }
        float child_bottom = c->bounds.y + c->bounds.h + m_b;
        if (child_bottom > content_bottom) { content_bottom = child_bottom; }
        float child_right = (c->bounds.x + parent->scroll_x) + c->bounds.w + m_r;
        if (child_right > content_right) { content_right = child_right; }
        y_cursor += c->bounds.h + m_b + s->gap * scale;
        c = c->next_sibling;
    }

    //
    // Post-pass for absolute + fixed children. Both use the same
    // helper; the only difference is the containing block:
    // absolute -> this window's content rect, fixed -> the
    // viewport (the panel dimensions). For meek-ui's use today
    // <window> IS the viewport, so the rectangles are identical.
    // The split matters once we have nested positioned ancestors
    // that aren't <window>.
    //
    int64 vp_w = 0;
    int64 vp_h = 0;
    scene__viewport(&vp_w, &vp_h);
    for (gui_node* ac = parent->first_child; ac != NULL; ac = ac->next_sibling)
    {
        if (ac->resolved.display == GUI_DISPLAY_NONE) { continue; }
        if (ac->resolved.position == GUI_POSITION_ABSOLUTE)
        {
            scene__layout_absolute_child(parent, ac, cx, cy, cw, ch);
        }
        else if (ac->resolved.position == GUI_POSITION_FIXED)
        {
            scene__layout_absolute_child(parent, ac, 0.0f, 0.0f, (float)vp_w, (float)vp_h);
        }
    }

    parent->content_w = (content_right - cx) + s->pad_r * scale;
    if (parent->content_w < cw) { parent->content_w = cw; }
    parent->content_h = (content_bottom - content_top) + parent->scroll_y;
    if (parent->content_h < 0.0f) { parent->content_h = 0.0f; }
    parent->content_h += s->pad_b * scale;
}

static void window_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = avail_w;
    n->bounds.h = avail_h;

    //
    // Two-pass layout for the AUTO overflow case (mirrors widget_div).
    // Speculative pass measures content_h with full width; the real
    // pass narrows the content width when a scrollbar will be visible.
    //
    _widget_window_internal__layout_children(n, scale, avail_w);
    boole need_v = scroll__vbar_visible(n);
    boole need_h = scroll__hbar_visible(n);
    if (need_v || need_h)
    {
        float cw = avail_w;
        if (need_v)
        {
            cw -= scroll__bar_size(n, scale);
            if (cw < 0.0f) { cw = 0.0f; }
        }
        _widget_window_internal__layout_children(n, scale, cw);
    }
    scroll__clamp(n);
}

//
// Drawing: bg first (under children), scrollbar last (over them).
// Scissor is pushed here and popped in emit_draws_post so children's
// draws -- and ONLY children's draws -- get clipped to the window.
//
static void window_emit_draws(gui_node* n, float scale)
{
    if (_widget_window_internal__needs_clip(n))
    {
        renderer__push_scissor(n->bounds);
    }

    //
    // Shared bg helper: gradient + shadow + bg-image + blur + solid
    // bg + border. Previously this function only emitted solid bg +
    // border, so a <window> with a background-gradient painted
    // nothing. Same fix applied to widget_div.
    //
    scene__emit_default_bg(n, scale);
}

static void window_emit_draws_post(gui_node* n, float scale)
{
    if (scroll__vbar_visible(n))
    {
        scroll__draw_vbar(n, scale);
    }
    if (scroll__hbar_visible(n))
    {
        scroll__draw_hbar(n, scale);
    }
    if (_widget_window_internal__needs_clip(n))
    {
        renderer__pop_scissor();
    }
}

static void window_on_mouse_down(gui_node* n, int64 x, int64 y, int64 button)
{
    (void)button;
    scroll__on_mouse_down(n, x, y, scene__scale());
}

static void window_on_mouse_up(gui_node* n, int64 x, int64 y, int64 button)
{
    (void)x;
    (void)y;
    (void)button;
    scroll__on_mouse_up(n);
}

static boole window_on_mouse_drag(gui_node* n, int64 x, int64 y)
{
    int64 coord = (n->scroll_drag_axis == 2) ? x : y;
    return scroll__on_mouse_drag(n, coord, scene__scale());
}

static const widget_vtable g_window_vtable = {
    .type_name        = "window",
    .init_defaults    = NULL,
    .apply_attribute  = NULL,
    .layout           = window_layout,
    .emit_draws       = window_emit_draws,
    .emit_draws_post  = window_emit_draws_post,
    .on_mouse_down    = window_on_mouse_down,
    .on_mouse_up      = window_on_mouse_up,
    .on_mouse_drag    = window_on_mouse_drag,
    .consumes_click   = FALSE,
};

void widget_window__register(void)
{
    widget_registry__register(GUI_NODE_WINDOW, &g_window_vtable);
}
