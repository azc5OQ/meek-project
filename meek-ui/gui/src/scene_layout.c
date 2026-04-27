//
//scene_layout.c -- layout pass split out of scene.c. Owns the
//column / row / generic dispatch helpers that walk the tree and
//write node->bounds.
//
//All widget-specific sizing logic still lives in widget_<name>.c
//files; this TU is the coordinator. Accesses shared state via
//scene__root() + scene__scale() (public accessors) rather than
//reaching into scene.c's statics directly.
//

#include <stdlib.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"

//
// Last viewport size passed to scene__layout. Popups / overlays that
// need to center themselves on the window read this via
// scene__viewport. Updated every frame.
//
static int64 _scene_layout_internal__viewport_w = 0;
static int64 _scene_layout_internal__viewport_h = 0;

void scene__layout_node(gui_node* n, float x, float y, float avail_w, float avail_h)
{
    if (n == NULL) { return; }
    const widget_vtable* vt = widget_registry__get(n->type);
    if (vt == NULL || vt->layout == NULL)
    {
        n->bounds.x = x;
        n->bounds.y = y;
        n->bounds.w = 0.0f;
        n->bounds.h = 0.0f;
        return;
    }
    vt->layout(n, x, y, avail_w, avail_h, scene__scale());
}

float scene__layout_width(gui_node* n, float avail_w, float scale)
{
    if (n == NULL) { return avail_w; }
    gui_style* s = &n->resolved;
    if (s->width_pct > 0.0f) { return avail_w * (s->width_pct / 100.0f); }
    if (s->size_w    > 0.0f) { return s->size_w * scale; }
    return avail_w;
}

float scene__layout_height(gui_node* n, float avail_h, float scale)
{
    if (n == NULL) { return avail_h; }
    gui_style* s = &n->resolved;
    if (s->height_pct > 0.0f) { return avail_h * (s->height_pct / 100.0f); }
    if (s->size_h     > 0.0f) { return s->size_h * scale; }
    return avail_h;
}

void scene__shift_tree(gui_node* n, float dx, float dy)
{
    if (n == NULL) { return; }
    n->bounds.x += dx;
    n->bounds.y += dy;
    for (gui_node* c = n->first_child; c != NULL; c = c->next_sibling)
    {
        scene__shift_tree(c, dx, dy);
    }
}

static void _scene_layout_internal__shift_tree(gui_node* n, float dx, float dy)
{
    scene__shift_tree(n, dx, dy);
}

//
// Lay out one position:absolute child against its parent's content
// rect (post-pad, post-border). Inset semantics:
//   - left+right both set: child width = content_w - left - right;
//     style width is ignored.
//   - left only: child x = content_x + left; width from style or
//     parent (size_w / width_pct or fall through to content_w).
//   - right only: child x = content_right - width - right.
//   - neither: child x = content_x + 0; width as above.
// Same logic on the vertical axis.
//
// Percent insets resolve against content_w / content_h.
//
void scene__layout_absolute_child(gui_node* parent,
                                  gui_node* c,
                                  float content_x,
                                  float content_y,
                                  float content_w,
                                  float content_h)
{
    (void)parent;
    float scale = scene__scale();
    gui_style* cs = &c->resolved;

    //
    // Resolve insets to pixels in this parent's coord space. Percent
    // wins ONLY if the matching px slot is 0 (matches the
    // overlay_style precedence: px slot is normally written, percent
    // slot only when explicitly set with `%`).
    //
    float l = (cs->inset_l > 0.0f) ? cs->inset_l * scale : (cs->inset_l_pct > 0.0f ? content_w * (cs->inset_l_pct / 100.0f) : 0.0f);
    float r = (cs->inset_r > 0.0f) ? cs->inset_r * scale : (cs->inset_r_pct > 0.0f ? content_w * (cs->inset_r_pct / 100.0f) : 0.0f);
    float t = (cs->inset_t > 0.0f) ? cs->inset_t * scale : (cs->inset_t_pct > 0.0f ? content_h * (cs->inset_t_pct / 100.0f) : 0.0f);
    float b = (cs->inset_b > 0.0f) ? cs->inset_b * scale : (cs->inset_b_pct > 0.0f ? content_h * (cs->inset_b_pct / 100.0f) : 0.0f);

    boole have_l = (cs->inset_l > 0.0f) || (cs->inset_l_pct > 0.0f);
    boole have_r = (cs->inset_r > 0.0f) || (cs->inset_r_pct > 0.0f);
    boole have_t = (cs->inset_t > 0.0f) || (cs->inset_t_pct > 0.0f);
    boole have_b = (cs->inset_b > 0.0f) || (cs->inset_b_pct > 0.0f);

    float child_x;
    float child_y;
    float child_w;
    float child_h;

    if (have_l && have_r)
    {
        //
        // Stretch between two horizontal edges. Child width derived;
        // size_w / width_pct ignored on this axis.
        //
        child_x = content_x + l;
        child_w = content_w - l - r;
    }
    else
    {
        child_w = scene__layout_width(c, content_w, scale);
        if (have_r) { child_x = content_x + content_w - child_w - r; }
        else        { child_x = content_x + l; }
    }
    if (child_w < 0.0f) { child_w = 0.0f; }

    if (have_t && have_b)
    {
        child_y = content_y + t;
        child_h = content_h - t - b;
    }
    else
    {
        child_h = scene__layout_height(c, content_h, scale);
        if (have_b) { child_y = content_y + content_h - child_h - b; }
        else        { child_y = content_y + t; }
    }
    if (child_h < 0.0f) { child_h = 0.0f; }

    scene__layout_node(c, child_x, child_y, child_w, child_h);
}

void scene__layout_children_as_column(gui_node* parent)
{
    gui_style* s     = &parent->resolved;
    float      scale = scene__scale();
    float      bw    = scene__border_width(parent, scale);
    float      cx    = parent->bounds.x + s->pad_l * scale + bw;
    float      cy    = parent->bounds.y + s->pad_t * scale + bw;
    float      cw    = parent->bounds.w - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    float      ch    = parent->bounds.h - (s->pad_t + s->pad_b) * scale - 2.0f * bw;
    if (cw < 0.0f) { cw = 0.0f; }
    if (ch < 0.0f) { ch = 0.0f; }

    float v_offset = 0.0f;
    if (s->valign != GUI_VALIGN_TOP)
    {
        float total_h = 0.0f;
        int64 visible_count = 0;
        for (gui_node* cc = parent->first_child; cc != NULL; cc = cc->next_sibling)
        {
            if (cc->resolved.display  == GUI_DISPLAY_NONE)        { continue; }
            if (cc->resolved.position == GUI_POSITION_ABSOLUTE
         || cc->resolved.position == GUI_POSITION_FIXED)        { continue; }
            scene__layout_node(cc, cx, 0.0f, cw, 0.0f);
            total_h += cc->bounds.h;
            visible_count++;
        }
        if (visible_count > 1)
        {
            total_h += s->gap * scale * (float)(visible_count - 1);
        }
        float slack = ch - total_h;
        if (slack < 0.0f) { slack = 0.0f; }
        if      (s->valign == GUI_VALIGN_CENTER) { v_offset = slack * 0.5f; }
        else if (s->valign == GUI_VALIGN_BOTTOM) { v_offset = slack;        }
    }

    float     y_cursor = cy + v_offset;
    gui_node* c        = parent->first_child;
    while (c != NULL)
    {
        if (c->resolved.display  == GUI_DISPLAY_NONE)      { c = c->next_sibling; continue; }
        if (c->resolved.position == GUI_POSITION_ABSOLUTE
         || c->resolved.position == GUI_POSITION_FIXED)
        {
            c = c->next_sibling;
            continue;
        }

        float m_t = c->resolved.margin_t * scale;
        float m_b = c->resolved.margin_b * scale;
        float m_l = c->resolved.margin_l * scale;
        float m_r = c->resolved.margin_r * scale;
        y_cursor += m_t;

        float child_cx = cx + m_l;
        float child_cw = cw - (m_l + m_r);
        if (child_cw < 0.0f) { child_cw = 0.0f; }

        scene__layout_node(c, child_cx, y_cursor, child_cw, ch - (y_cursor - cy));

        if (s->halign != GUI_HALIGN_LEFT)
        {
            float slack_w = child_cw - c->bounds.w;
            if (slack_w > 0.0f)
            {
                float dx = 0.0f;
                if      (s->halign == GUI_HALIGN_CENTER) { dx = slack_w * 0.5f; }
                else if (s->halign == GUI_HALIGN_RIGHT)  { dx = slack_w;        }
                _scene_layout_internal__shift_tree(c, dx, 0.0f);
            }
        }
        //
        // position: relative -- the child stays in flow (sibling
        // cursor advances by its full height as if unshifted), but
        // its rendered bounds get nudged by inset_l/t (positive)
        // and inset_r/b (negative). CSS-spec: top wins over bottom
        // and left wins over right; we follow that by computing
        // shift = inset_l - inset_r (and similarly for y), which
        // collapses to either when only one is set.
        //
        if (c->resolved.position == GUI_POSITION_RELATIVE)
        {
            float dx_rel = (c->resolved.inset_l - c->resolved.inset_r) * scale;
            float dy_rel = (c->resolved.inset_t - c->resolved.inset_b) * scale;
            if (dx_rel != 0.0f || dy_rel != 0.0f)
            {
                _scene_layout_internal__shift_tree(c, dx_rel, dy_rel);
            }
        }
        y_cursor += c->bounds.h + m_b + s->gap * scale;
        c = c->next_sibling;
    }

    //
    // Second pass: absolute children. They were skipped above, so the
    // sibling cursor + alignment numbers above were computed as if
    // they didn't exist. Now we lay each one out against the parent's
    // content rect using its insets / size. Same content rect the
    // flow loop used (cx/cy/cw/ch already include pad + border).
    //
    // Fixed children get post-passed at the window-root level by
    // window_layout (they anchor to the viewport, not the immediate
    // parent), so we skip them here.
    //
    for (gui_node* ac = parent->first_child; ac != NULL; ac = ac->next_sibling)
    {
        if (ac->resolved.display  == GUI_DISPLAY_NONE)        { continue; }
        if (ac->resolved.position != GUI_POSITION_ABSOLUTE)   { continue; }
        scene__layout_absolute_child(parent, ac, cx, cy, cw, ch);
    }
}

void scene__layout_children_as_row(gui_node* parent)
{
    gui_style* s     = &parent->resolved;
    float      scale = scene__scale();
    float      bw    = scene__border_width(parent, scale);
    float      cx    = parent->bounds.x + s->pad_l * scale + bw;
    float      cy    = parent->bounds.y + s->pad_t * scale + bw;
    float      cw    = parent->bounds.w - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    float      ch    = parent->bounds.h - (s->pad_t + s->pad_b) * scale - 2.0f * bw;
    if (cw < 0.0f) { cw = 0.0f; }
    if (ch < 0.0f) { ch = 0.0f; }

    float h_offset = 0.0f;
    if (s->halign != GUI_HALIGN_LEFT)
    {
        float total_w = 0.0f;
        int64 visible_count = 0;
        for (gui_node* cc = parent->first_child; cc != NULL; cc = cc->next_sibling)
        {
            if (cc->resolved.display  == GUI_DISPLAY_NONE)      { continue; }
            if (cc->resolved.position == GUI_POSITION_ABSOLUTE
             || cc->resolved.position == GUI_POSITION_FIXED)    { continue; }
            scene__layout_node(cc, 0.0f, cy, cw, ch);
            total_w += cc->bounds.w;
            visible_count++;
        }
        if (visible_count > 1)
        {
            total_w += s->gap * scale * (float)(visible_count - 1);
        }
        float slack = cw - total_w;
        if (slack < 0.0f) { slack = 0.0f; }
        if      (s->halign == GUI_HALIGN_CENTER) { h_offset = slack * 0.5f; }
        else if (s->halign == GUI_HALIGN_RIGHT)  { h_offset = slack;        }
    }

    float     x_cursor = cx + h_offset;
    gui_node* c        = parent->first_child;
    while (c != NULL)
    {
        if (c->resolved.display  == GUI_DISPLAY_NONE)      { c = c->next_sibling; continue; }
        if (c->resolved.position == GUI_POSITION_ABSOLUTE
         || c->resolved.position == GUI_POSITION_FIXED)
        {
            c = c->next_sibling;
            continue;
        }

        float m_t = c->resolved.margin_t * scale;
        float m_b = c->resolved.margin_b * scale;
        float m_l = c->resolved.margin_l * scale;
        float m_r = c->resolved.margin_r * scale;
        x_cursor += m_l;

        float remaining_w = cw - (x_cursor - cx);
        if (remaining_w < 0.0f) { remaining_w = 0.0f; }
        float child_cy = cy + m_t;
        float child_ch = ch - (m_t + m_b);
        if (child_ch < 0.0f) { child_ch = 0.0f; }

        scene__layout_node(c, x_cursor, child_cy, remaining_w, child_ch);

        if (s->valign != GUI_VALIGN_TOP)
        {
            float slack_h = child_ch - c->bounds.h;
            if (slack_h > 0.0f)
            {
                float dy = 0.0f;
                if      (s->valign == GUI_VALIGN_CENTER) { dy = slack_h * 0.5f; }
                else if (s->valign == GUI_VALIGN_BOTTOM) { dy = slack_h;        }
                _scene_layout_internal__shift_tree(c, 0.0f, dy);
            }
        }
        //
        // position: relative -- same nudge as the column variant.
        //
        if (c->resolved.position == GUI_POSITION_RELATIVE)
        {
            float dx_rel = (c->resolved.inset_l - c->resolved.inset_r) * scale;
            float dy_rel = (c->resolved.inset_t - c->resolved.inset_b) * scale;
            if (dx_rel != 0.0f || dy_rel != 0.0f)
            {
                _scene_layout_internal__shift_tree(c, dx_rel, dy_rel);
            }
        }
        x_cursor += c->bounds.w + m_r + s->gap * scale;
        c = c->next_sibling;
    }

    //
    // Second pass: absolute children. See column variant for the
    // rationale -- siblings are flowed assuming these don't exist,
    // then each absolute child is placed against the content rect.
    //
    for (gui_node* ac = parent->first_child; ac != NULL; ac = ac->next_sibling)
    {
        if (ac->resolved.display  == GUI_DISPLAY_NONE)        { continue; }
        if (ac->resolved.position != GUI_POSITION_ABSOLUTE)   { continue; }
        scene__layout_absolute_child(parent, ac, cx, cy, cw, ch);
    }
}

void scene__layout(int64 viewport_w, int64 viewport_h)
{
    _scene_layout_internal__viewport_w = viewport_w;
    _scene_layout_internal__viewport_h = viewport_h;
    scene__layout_node(scene__root(), 0.0f, 0.0f, (float)viewport_w, (float)viewport_h);
}

void scene__viewport(int64* out_w, int64* out_h)
{
    if (out_w != NULL) { *out_w = _scene_layout_internal__viewport_w; }
    if (out_h != NULL) { *out_h = _scene_layout_internal__viewport_h; }
}
