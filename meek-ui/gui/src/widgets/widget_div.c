//
// widget_div.c - generic styled block container with optional scrollbars.
//
// Like <Column> in that it lays out children vertically using the
// standard column placement helper, but with two differences:
//   1. <div> defaults to filling the available space in BOTH dimensions
//      (width AND height) rather than collapsing to children-height.
//   2. <div> optionally shows scrollbars driven by the overflow_x /
//      overflow_y style properties. Scrollable divs let the user drag
//      the thumb to scroll content that exceeds the div's bounds, AND
//      clip overflowing content via renderer__push_scissor.
//
// Use it as a styled panel / section / card / "page region" -- any
// time you want a sized block whose visuals (bg, radius, padding,
// scrollbars) you control, and whose children flow inside.
//
// Tag name is intentionally lowercase ("div") to match HTML convention.
//
// SCROLLING
// ---------
// All scrollbar mechanics (visibility decision, track + thumb rects,
// clamp, drag handling) live in scroll.c so widget_window.c can reuse
// the same code. This file just orchestrates: layout → emit_draws →
// scene recurses children → emit_draws_post.
//
// Overflow modes (gui_style.overflow_y):
//     VISIBLE  no scrollbar, no clip, no scroll. default.
//     HIDDEN   no scrollbar, but scissor still clips children to bounds.
//     SCROLL   always show a scrollbar even if content fits; clip.
//     AUTO     show a scrollbar only if content exceeds bounds; clip.
//
// Layout flow for a scrollable div:
//     1. Compute the div's own bounds from avail_w / avail_h / style.
//     2. Speculative pass: lay out children at the full width to
//        measure content_h.
//     3. If a scrollbar will be shown (SCROLL mode, or AUTO with
//        overflow), redo the layout with content_w narrowed by
//        scrollbar_size so children don't paint into the bar slot.
//     4. scroll__clamp() snaps scroll_y back into the legal range.
//
// Draw flow:
//     emit_draws       (called BEFORE scene recurses into children)
//         push_scissor(div bounds) if clipping
//         submit bg rect
//     ...scene walks into children, who submit their own rects/text
//        ...all clipped by the scissor pushed above...
//     emit_draws_post  (called AFTER children)
//         draw scrollbar (track + thumb)
//         pop_scissor
//
// Drag interaction lives entirely in scroll.{h,c}; we just delegate
// the on_mouse_* hooks.
//

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "scroll.h"

//
// Decide whether this div needs clipping at all. Clipping is on
// whenever the div's overflow_y is HIDDEN, SCROLL, or AUTO-with-
// overflowing-content (the same condition that matters for the bar).
// VISIBLE divs skip the scissor push to keep the renderer's flush
// count low for the common case.
//
static boole _widget_div_internal__needs_clip(gui_node* n)
{
    gui_overflow ox = n->resolved.overflow_x;
    gui_overflow oy = n->resolved.overflow_y;
    if (ox == GUI_OVERFLOW_HIDDEN || ox == GUI_OVERFLOW_SCROLL) { return TRUE; }
    if (oy == GUI_OVERFLOW_HIDDEN || oy == GUI_OVERFLOW_SCROLL) { return TRUE; }
    if (ox == GUI_OVERFLOW_AUTO || oy == GUI_OVERFLOW_AUTO)
    {
        //
        // AUTO mode only clips when there's actual overflow. We check
        // both axes so either direction of overflow triggers the clip.
        //
        return (boole)(scroll__vbar_visible(n) || scroll__hbar_visible(n));
    }
    return FALSE;
}

//
// Scroll-aware column layout. Mirrors scene__layout_children_as_column
// but: shifts children up by scroll_y, measures total content extent
// into parent->content_h. Returns nothing; mutates child bounds.
//
static void _widget_div_internal__layout_children(gui_node* parent, float scale, float content_w)
{
    gui_style* s  = &parent->resolved;
    //
    // Border eats from BOTH sides of each axis -- inner area shrinks
    // by 2*bw and the child origin shifts by bw so children sit
    // inside the line, not under it. Same gating as scene__emit_border
    // via scene__border_width: bw is 0 for unbordered divs, so this
    // collapses to the original math in the common case.
    //
    float      bw = scene__border_width(parent, scale);
    float      cx = parent->bounds.x + s->pad_l * scale + bw;
    float      cy = parent->bounds.y + s->pad_t * scale + bw;
    float      ch = parent->bounds.h - (s->pad_t + s->pad_b) * scale - 2.0f * bw;
    float      cw = content_w        - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    if (cw < 0.0f) { cw = 0.0f; }
    if (ch < 0.0f) { ch = 0.0f; }

    //
    // Vertical group alignment. Only makes sense when the div's
    // content actually fits in its height; a scrollable div (content
    // overflows) naturally wants TOP anchoring so the "first item" is
    // stable across scroll. Speculatively measure total child height;
    // if it's less than ch, offset the whole stack by the slack. If
    // it's greater (would scroll), skip the offset.
    //
    float v_offset = 0.0f;
    if (s->valign != GUI_VALIGN_TOP)
    {
        float total_h = 0.0f;
        int64 visible_count = 0;
        for (gui_node* cc = parent->first_child; cc != NULL; cc = cc->next_sibling)
        {
            if (cc->resolved.display == GUI_DISPLAY_NONE) { continue; }
            scene__layout_node(cc, cx, 0.0f, cw, 0.0f);
            total_h += cc->bounds.h;
            visible_count++;
        }
        if (visible_count > 1)
        {
            total_h += s->gap * scale * (float)(visible_count - 1);
        }
        float slack = ch - total_h;
        if (slack > 0.0f)
        {
            if      (s->valign == GUI_VALIGN_CENTER) { v_offset = slack * 0.5f; }
            else if (s->valign == GUI_VALIGN_BOTTOM) { v_offset = slack;        }
        }
    }

    //
    // Children start at (top-left of content area) minus the current
    // scroll offsets so increasing scroll_y moves content upward and
    // scroll_x moves content leftward (classic scrollbar semantics).
    //
    float x_origin       = cx - parent->scroll_x;
    float y_cursor       = cy + v_offset - parent->scroll_y;
    float content_top    = cy;
    float content_bottom = cy;
    float content_right  = cx; // widest child's right edge (un-scrolled).

    gui_node* c = parent->first_child;
    while (c != NULL)
    {
        //
        // display: none children collapse out of the layout: no
        // measurement, no gap consumed, no contribution to
        // content_bottom. Matches the scene helpers' behaviour.
        //
        if (c->resolved.display == GUI_DISPLAY_NONE)
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

        float child_x = x_origin + m_l;
        float child_cw = cw - (m_l + m_r);
        if (child_cw < 0.0f) { child_cw = 0.0f; }

        //
        // For the x-overflow case we need to measure each child at its
        // natural width, not capped to cw. Pass cw still (the column
        // width contract is kept so children that lay themselves out
        // relative to the parent's content-width keep the same
        // behaviour), but then track the actual right edge so
        // content_w reflects "the widest child", not "cw". Children
        // that elect to be wider than cw (e.g. a nested wide image)
        // will trigger horizontal overflow, which the hbar picks up.
        //
        scene__layout_node(c, child_x, y_cursor, child_cw, ch - (y_cursor - cy));
        //
        // Per-child horizontal alignment. Same approach as the scene
        // column helper: if the child took less than cw, shift by the
        // slack based on halign. LEFT = no-op.
        //
        if (s->halign != GUI_HALIGN_LEFT)
        {
            float slack_w = child_cw - c->bounds.w;
            if (slack_w > 0.0f)
            {
                float dx = 0.0f;
                if      (s->halign == GUI_HALIGN_CENTER) { dx = slack_w * 0.5f; }
                else if (s->halign == GUI_HALIGN_RIGHT)  { dx = slack_w;        }
                scene__shift_tree(c, dx, 0.0f);
            }
        }
        float child_bottom = c->bounds.y + c->bounds.h + m_b;
        if (child_bottom > content_bottom) { content_bottom = child_bottom; }
        //
        // Un-scroll the child's right edge (we subtracted scroll_x from
        // the origin) so content_right is comparable to cw directly.
        //
        float child_right = (c->bounds.x + parent->scroll_x) + c->bounds.w + m_r;
        if (child_right > content_right) { content_right = child_right; }
        y_cursor += c->bounds.h + m_b + s->gap * scale;
        c = c->next_sibling;
    }

    //
    // content_h / content_w are both measured in the un-scrolled
    // coordinate system: the distance from the top-left of the padding
    // zone to the bottom-right of the most-extending child, AS IF
    // scroll were zero. We add scroll_y back because content_bottom
    // was computed with it already subtracted. Then add bottom padding
    // so a div with pad: 24 actually shows 24 px of padding under its
    // last child. For content_w, content_right was computed AFTER
    // un-scrolling each child, so we just diff against cx and add the
    // right padding.
    //
    parent->content_w = (content_right - cx) + s->pad_r * scale;
    if (parent->content_w < cw) { parent->content_w = cw; }
    parent->content_h = (content_bottom - content_top) + parent->scroll_y;
    if (parent->content_h < 0.0f) { parent->content_h = 0.0f; }
    parent->content_h += s->pad_b * scale;
}

static void div_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    //
    // Width / height come from the layout helpers which already
    // honor `width: N%`, `width: Npx`, `size: WxH`, and parent-
    // available fallbacks. div_layout doesn't read any other
    // style field directly, so there's no `gui_style* s` here.
    //
    float w = scene__layout_width(n, avail_w, scale);
    float h = scene__layout_height(n, avail_h, scale);
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;

    //
    // Two-pass layout for AUTO overflow:
    //   pass 1 measures content_w and content_h with the full width;
    //   pass 2 (only if either bar will be visible) re-lays out with
    //          dimensions narrowed by the bar(s) they'd occupy. A
    //          visible vbar eats from the right edge, an hbar eats
    //          from the bottom. The layout helper doesn't care about
    //          the height narrowing directly -- the scroll_y clamp at
    //          the end handles that -- but the width narrowing matters
    //          because children that auto-fit to cw must not bleed
    //          under the vbar.
    //
    _widget_div_internal__layout_children(n, scale, w);
    boole need_v = scroll__vbar_visible(n);
    boole need_h = scroll__hbar_visible(n);
    if (need_v || need_h)
    {
        float narrow_w = w;
        if (need_v)
        {
            narrow_w -= scroll__bar_size(n, scale);
            if (narrow_w < 0.0f) { narrow_w = 0.0f; }
        }
        _widget_div_internal__layout_children(n, scale, narrow_w);
    }

    scroll__clamp(n);
}

//
// emit_draws is called BEFORE scene recurses into children. Anything
// submitted here renders UNDER children. So this is the right spot
// for: scissor push (bracketing children's draws), and the bg rect.
//
static void div_emit_draws(gui_node* n, float scale)
{
    //
    // Push the scissor first so the bg itself, while drawn at the
    // same rect as the scissor (so a no-op for the bg), STAYS pushed
    // until emit_draws_post pops it. Children then inherit the clip.
    //
    if (_widget_div_internal__needs_clip(n))
    {
        renderer__push_scissor(n->bounds);
    }

    //
    // Delegate bg painting to the shared helper so we pick up
    // gradient + shadow + bg-image + blur in addition to plain
    // background-color. Earlier this function only handled
    // has_background_color, which meant a <div> styled with ONLY a
    // background-gradient drew nothing -- the gradient test scene
    // caught it. scene__emit_default_bg also calls scene__emit_border
    // internally, so that's covered too.
    //
    scene__emit_default_bg(n, scale);
}

//
// emit_draws_post runs AFTER scene has recursed through children.
// Submissions here paint over children -- ideal for scrollbars,
// focus rings, etc. Also where the scissor pushed in emit_draws
// gets popped, so subsequent siblings/uncles draw unclipped.
//
static void div_emit_draws_post(gui_node* n, float scale)
{
    if (scroll__vbar_visible(n))
    {
        scroll__draw_vbar(n, scale);
    }
    if (scroll__hbar_visible(n))
    {
        scroll__draw_hbar(n, scale);
    }
    if (_widget_div_internal__needs_clip(n))
    {
        renderer__pop_scissor();
    }
}

static void div_on_mouse_down(gui_node* n, int64 x, int64 y, int64 button)
{
    (void)button;
    scroll__on_mouse_down(n, x, y, scene__scale());
}

static void div_on_mouse_up(gui_node* n, int64 x, int64 y, int64 button)
{
    (void)x;
    (void)y;
    (void)button;
    scroll__on_mouse_up(n);
}

static boole div_on_mouse_drag(gui_node* n, int64 x, int64 y)
{
    //
    // Route the coordinate the scroll helper expects based on which
    // axis is currently being dragged. scroll_drag_axis was set by
    // scroll__on_mouse_down; 1 = y-thumb drag, 2 = x-thumb drag.
    //
    int64 coord = (n->scroll_drag_axis == 2) ? x : y;
    return scroll__on_mouse_drag(n, coord, scene__scale());
}

static const widget_vtable g_div_vtable = {
    .type_name        = "div",
    .init_defaults    = NULL,
    .apply_attribute  = NULL,
    .layout           = div_layout,
    .emit_draws       = div_emit_draws,
    .emit_draws_post  = div_emit_draws_post,
    .on_mouse_down    = div_on_mouse_down,
    .on_mouse_up      = div_on_mouse_up,
    .on_mouse_drag    = div_on_mouse_drag,
    .consumes_click   = FALSE,
};

void widget_div__register(void)
{
    widget_registry__register(GUI_NODE_DIV, &g_div_vtable);
}
