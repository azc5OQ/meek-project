//
// widget_select.c - HTML-style dropdown menu.
//
//   <select id="size" on_change="on_size_change">
//       <option text="Small"  />
//       <option text="Medium" />
//       <option text="Large"  />
//   </select>
//
// CLOSED STATE:
//   Looks like a button-ish rect with the currently selected option's
//   text left-aligned and a small chevron indicator on the right edge.
//   Click: opens the popup.
//
// OPEN STATE (`n->is_open == TRUE`):
//   A popup menu of options appears directly below the select, each
//   option a row with its text. Clicking a row picks that option and
//   closes. Clicking the select itself closes. Clicking anywhere else
//   is a limitation today -- it doesn't auto-close. (Follow-up:
//   extend scene__set_overlay with a "click-outside" callback.)
//
// HOW THE POPUP DRAWS ABOVE EVERYTHING ELSE:
//   Normal widget emit_draws fires at the widget's position in the
//   tree, so any sibling drawn later paints over it. We need the
//   opposite: the popup has to land ON TOP of subsequent siblings.
//   Scene's OVERLAY mechanism (see scene__set_overlay in scene.h)
//   solves that -- when the select opens, we register a (node, bounds,
//   draw_fn) triple that scene processes AFTER the normal walk.
//   Hit-testing is similarly redirected: clicks inside the popup's
//   bounds route to the select instead of to whatever is spatially
//   underneath.
//
// SCENE INTEGRATION PIECES USED:
//   scene__set_overlay(node, bounds, draw_fn) -- open/close the popup.
//   scene__overlay_node()                     -- "is my popup still
//                                                 the active overlay?"
//                                                 used during close to
//                                                 avoid clobbering
//                                                 another widget's
//                                                 overlay.
//   n->is_open  -- our own 0/1 state bit (on gui_node).
//   n->value    -- currently selected option index.
//   n->text     -- currently selected option's text (cached here so
//                  we don't have to walk children every frame during
//                  draw).
//
// STYLING HOOKS:
//   bg / has_bg            -- select button bg.
//   color / has_color      -- text color (option labels).
//   fg / has_fg            -- popup row highlight + chevron color.
//   radius                 -- select + popup row corner radius.
//   size / size_w / size_h -- normal sizing rules.
//   font_family / font_size -- picked up via inheritance.
//   :hover / :pressed / :appear -- all supported via the standard
//                                  animator / state resolver.
//

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "font.h"

//
// Forward decl for the overlay draw function (scene calls it after the
// main tree walk; declared here so select's on_mouse_up can pass its
// address to scene__set_overlay).
//
static void _widget_select_internal__draw_popup(gui_node* n, float scale);

//
// Popup row metrics. Constants keep the layout consistent; larger
// selects (lots of options) will run past the viewport bottom -- a
// future enhancement is to clamp or make the popup scrollable.
//
static const float _WIDGET_SELECT_INTERNAL__ROW_HEIGHT = 32.0f;
static const float _WIDGET_SELECT_INTERNAL__POPUP_PAD  = 4.0f;

//
// Return the number of <option> children this select has.
//
static int64 _widget_select_internal__option_count(gui_node* select)
{
    int64 count = 0;
    for (gui_node* c = select->first_child; c != NULL; c = c->next_sibling)
    {
        if (c->type == GUI_NODE_OPTION)
        {
            count++;
        }
    }
    return count;
}

//
// Return the Nth <option> child, or NULL if out of range.
//
static gui_node* _widget_select_internal__option_at(gui_node* select, int64 index)
{
    int64 i = 0;
    for (gui_node* c = select->first_child; c != NULL; c = c->next_sibling)
    {
        if (c->type == GUI_NODE_OPTION)
        {
            if (i == index) { return c; }
            i++;
        }
    }
    return NULL;
}

//
// Compute the popup's bounding rect (the full vertical list of options
// sitting directly below the select's bounds). Used for hit-testing
// and for the scene overlay registration.
//
static gui_rect _widget_select_internal__popup_bounds(gui_node* n, float scale)
{
    int64 count = _widget_select_internal__option_count(n);
    float row_h = _WIDGET_SELECT_INTERNAL__ROW_HEIGHT * scale;
    float pad   = _WIDGET_SELECT_INTERNAL__POPUP_PAD  * scale;

    gui_rect r;
    r.x = n->bounds.x;
    r.y = n->bounds.y + n->bounds.h + pad;
    r.w = n->bounds.w;
    r.h = (float)count * row_h;
    return r;
}

//
// Sync n->text to the currently selected option's text. Called
// whenever n->value changes, so the select shows the right label on
// the next draw without having to walk children during emit_draws.
//
static void _widget_select_internal__sync_label(gui_node* n)
{
    gui_node* opt = _widget_select_internal__option_at(n, (int64)n->value);
    if (opt == NULL)
    {
        n->text[0]  = 0;
        n->text_len = 0;
        return;
    }
    int64 len = (int64)strlen(opt->text);
    if (len >= (int64)sizeof(n->text)) { len = (int64)sizeof(n->text) - 1; }
    memcpy(n->text, opt->text, (size_t)len);
    n->text[len]  = 0;
    n->text_len   = len;
}

static void select_init_defaults(gui_node* n)
{
    //
    // Default to the first option selected. sync_label runs AGAIN
    // after children are parsed (at first layout pass in the parser
    // aren't wired yet -- the attribute apply happens before children
    // are added), so init here is just "value = 0".
    //
    n->value = 0.0f;
}

static boole select_apply_attribute(gui_node* n, char* name, char* value)
{
    if (strcmp(name, "value") == 0)
    {
        //
        // Parse as a plain integer index. Negative or out-of-range is
        // clamped at first layout (where we know the child count).
        //
        int64 idx = 0;
        for (char* p = value; *p != 0; p++)
        {
            if (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); }
        }
        n->value = (float)idx;
        return TRUE;
    }
    return FALSE;
}

static void select_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_h;
    gui_style* s = &n->resolved;
    float w = scene__layout_width(n, avail_w, scale);
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 36.0f * scale;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;

    //
    // Lay out children at zero size (they're data carriers, not
    // visible nodes). Also takes this opportunity to clamp value
    // into [0, count-1] now that we know the child count, and to
    // refresh the cached label text.
    //
    for (gui_node* c = n->first_child; c != NULL; c = c->next_sibling)
    {
        c->bounds.x = x;
        c->bounds.y = y;
        c->bounds.w = 0.0f;
        c->bounds.h = 0.0f;
    }

    int64 count = _widget_select_internal__option_count(n);
    if (count > 0)
    {
        if (n->value < 0.0f)               { n->value = 0.0f; }
        if ((int64)n->value >= count)      { n->value = (float)(count - 1); }
    }
    _widget_select_internal__sync_label(n);
}

static void select_emit_draws(gui_node* n, float scale)
{
    gui_style* s = &n->resolved;

    //
    // Button-like rect. When the style sets any bg property (solid,
    // gradient, or bg-image), delegate to the shared helper so the
    // full bundle (plus shadow/blur/border-gradient) composes. When
    // the style sets nothing we emit the fallback dark-panel colour
    // so a <select> without any styling still reads as a control.
    //
    if (s->has_background_color || s->has_bg_gradient || s->has_shadow || s->blur_px > 0.0f)
    {
        scene__emit_default_bg(n, scale);
    }
    else
    {
        gui_color bg = scene__rgb(0.12f, 0.13f, 0.16f);
        renderer__submit_rect(n->bounds, bg, s->radius * scale);
        scene__emit_border(n, n->bounds, scale);
    }

    //
    // Label text: left-aligned inside the select's bounds with a
    // 12 px left pad. Right side reserved for the chevron.
    //
    if (n->text[0] != 0)
    {
        float     size_px = (s->font_size > 0.0f ? s->font_size : 14.0f) * scale;
        gui_font* f       = font__at(s->font_family, size_px);
        if (f != NULL)
        {
            float     ascent = font__ascent(f);
            float     pad    = 12.0f * scale;
            float     line   = font__line_height(f);
            float     ty     = n->bounds.y + (n->bounds.h - line) * 0.5f + ascent;
            gui_color col    = s->has_font_color ? s->font_color : scene__rgb(0.96f, 0.96f, 0.98f);
            font__draw(f, n->bounds.x + pad, ty, col, n->text);
        }
    }

    //
    // Chevron: two thin diagonal bars forming a down-caret on the
    // right edge. When the popup is open, flip it to an up-caret by
    // drawing the bars mirrored vertically. Drawn with fg (or a
    // light gray default).
    //
    gui_color chev_col = s->has_accent_color ? s->accent_color : scene__rgb(0.72f, 0.74f, 0.80f);
    float     side     = 10.0f * scale;
    float     bar_w    = 2.0f  * scale;
    float     cx       = n->bounds.x + n->bounds.w - 16.0f * scale;
    float     cy       = n->bounds.y + n->bounds.h * 0.5f;

    //
    // Two tiny rects approximating a chevron. Not a perfect diagonal
    // but reads as "dropdown" at typical sizes. Upgraded to a proper
    // glyph / icon atlas later.
    //
    gui_rect l, r;
    if (n->is_open)
    {
        //
        // Up-caret: bars meeting at the top.
        //
        l.x = cx - side * 0.5f; l.y = cy;                   l.w = side * 0.55f; l.h = bar_w;
        r.x = cx;               r.y = cy;                   r.w = side * 0.55f; r.h = bar_w;
    }
    else
    {
        //
        // Down-caret: bars meeting at the bottom (default).
        //
        l.x = cx - side * 0.5f; l.y = cy - bar_w * 0.5f;    l.w = side * 0.55f; l.h = bar_w;
        r.x = cx;               r.y = cy - bar_w * 0.5f;    r.w = side * 0.55f; r.h = bar_w;
    }
    renderer__submit_rect(l, chev_col, 0.0f);
    renderer__submit_rect(r, chev_col, 0.0f);
}

//
// Click handler for BOTH the closed-state select rect AND the popup
// area when open. scene routes the click to us in either case (the
// normal hit test handles the select rect; the overlay hit test
// handles the popup area).
//
static void select_on_mouse_up(gui_node* n, int64 x, int64 y, int64 button)
{
    (void)button;
    float fx = (float)x, fy = (float)y;
    float scale = scene__scale();

    //
    // Click on the select rect itself -> toggle open/closed.
    //
    if (fx >= n->bounds.x && fx < n->bounds.x + n->bounds.w &&
        fy >= n->bounds.y && fy < n->bounds.y + n->bounds.h)
    {
        if (n->is_open)
        {
            //
            // Close: clear the overlay (only if WE are the active
            // overlay -- prevents stomping another widget's overlay
            // that happened to be opened in the meantime, unlikely
            // but defensive).
            //
            n->is_open = FALSE;
            if (scene__overlay_node() == n)
            {
                gui_rect empty = { 0.0f, 0.0f, 0.0f, 0.0f };
                scene__set_overlay(NULL, empty, NULL);
            }
        }
        else
        {
            //
            // Open: register an overlay spanning the popup's projected
            // rect. The rect includes the select itself so clicks on
            // the button while open also route through us.
            //
            // If another <select> already owns the overlay, close it
            // first so its is_open flag doesn't go stale (the chevron
            // would stay flipped but the dropdown wouldn't paint --
            // matches OS behaviour where opening one combo closes any
            // other open combo). We can't safely notify non-select
            // overlays (e.g. popup) without a generic close callback;
            // popups are modal and cover the viewport, so the user
            // can't physically click a select while one is up.
            //
            gui_node* prev = scene__overlay_node();
            if (prev != NULL && prev != n && prev->type == GUI_NODE_SELECT)
            {
                prev->is_open = FALSE;
            }
            n->is_open = TRUE;
            gui_rect popup = _widget_select_internal__popup_bounds(n, scale);
            gui_rect combined;
            combined.x = n->bounds.x;
            combined.y = n->bounds.y;
            combined.w = n->bounds.w;
            combined.h = (popup.y + popup.h) - n->bounds.y;
            scene__set_overlay(n, combined, _widget_select_internal__draw_popup);
        }
        return;
    }

    //
    // Click outside the select rect. If the popup is open, check if
    // we landed on one of the rows.
    //
    if (!n->is_open) { return; }

    gui_rect popup = _widget_select_internal__popup_bounds(n, scale);
    if (fx < popup.x || fx >= popup.x + popup.w ||
        fy < popup.y || fy >= popup.y + popup.h)
    {
        //
        // Inside the combined overlay rect but neither in the select
        // button nor in the popup (the little padding strip between
        // them). Treat as "cancel" -- close without changing value.
        //
        n->is_open = FALSE;
        if (scene__overlay_node() == n)
        {
            gui_rect empty = { 0.0f, 0.0f, 0.0f, 0.0f };
            scene__set_overlay(NULL, empty, NULL);
        }
        return;
    }

    //
    // Row = floor((y - popup.y) / row_h).
    //
    float  row_h = _WIDGET_SELECT_INTERNAL__ROW_HEIGHT * scale;
    int64  row   = (int64)((fy - popup.y) / row_h);
    int64  count = _widget_select_internal__option_count(n);
    if (row < 0 || row >= count) { return; }

    //
    // Commit + close + fire on_change. Only dispatch when the value
    // actually changed -- matches HTML / native behavior.
    //
    float new_value = (float)row;
    boole changed   = (n->value != new_value);
    n->value  = new_value;
    n->is_open = FALSE;
    _widget_select_internal__sync_label(n);
    if (scene__overlay_node() == n)
    {
        gui_rect empty = { 0.0f, 0.0f, 0.0f, 0.0f };
        scene__set_overlay(NULL, empty, NULL);
    }
    if (changed)
    {
        scene__dispatch_change(n, new_value);
    }
}

//
// Overlay draw callback. Called by scene__emit_draws AFTER the normal
// tree walk finishes, so every rect and glyph submitted here lands ON
// TOP of everything the main walk produced. Draws:
//   - A panel rect below the select, with the resolved bg + radius.
//   - Each option as a row with its text. The currently selected row
//     gets a highlight tint (resolved fg with ~20% alpha).
//
static void _widget_select_internal__draw_popup(gui_node* n, float scale)
{
    gui_style* s = &n->resolved;

    gui_rect popup = _widget_select_internal__popup_bounds(n, scale);

    //
    // Force a flush of EVERYTHING queued so far (every rect + text
    // from the main scene walk) before we start drawing the popup.
    // Renderers draw rects THEN text per batch, so without this the
    // popup's panel_bg would queue into the same rect batch as all
    // the labels in the tree, then every tree label's text glyphs
    // would draw ON TOP of the popup panel -- the text-bleeding-
    // through-popup bug. push_scissor forces flush_batches, which
    // drains the current queues in the correct order.
    //
    // We use the full viewport as the scissor; nothing outside the
    // popup bounds would legally paint anyway since we only draw
    // inside popup here. Pop at the end to restore the previous
    // scissor state.
    //
    int64 vw = 0, vh = 0;
    scene__viewport(&vw, &vh);
    gui_rect full_vp;
    full_vp.x = 0.0f; full_vp.y = 0.0f;
    full_vp.w = (float)vw; full_vp.h = (float)vh;
    renderer__push_scissor(full_vp);

    //
    // Panel background. Slightly darker than the select button so the
    // popup reads as a distinct "layer" above the page.
    //
    // FORCE alpha = 1.0 regardless of what the node's :appear /
    // effective_opacity animation has written into
    // resolved.background_color.a. The select's own fade-in is a
    // scene-walk effect; the popup is a modal overlay that should
    // be fully opaque whenever open, otherwise tree text bleeds
    // through the panel and the options read against the wrong
    // backdrop. This shows up on Android more readily because
    // GL-context-loss resets appear_age_ms on re-foreground,
    // leaving the node mid-fade any time the popup opens after
    // backgrounding.
    //
    gui_color panel_bg;
    if (s->has_background_color)
    {
        panel_bg = s->background_color;
        panel_bg.r *= 0.85f; panel_bg.g *= 0.85f; panel_bg.b *= 0.85f;
    }
    else
    {
        panel_bg = scene__rgb(0.09f, 0.10f, 0.13f);
    }
    panel_bg.a = 1.0f;
    renderer__submit_rect(popup, panel_bg, s->radius * scale);

    //
    // Rows.
    //
    float row_h = _WIDGET_SELECT_INTERNAL__ROW_HEIGHT * scale;
    int64 count = _widget_select_internal__option_count(n);

    float     size_px = (s->font_size > 0.0f ? s->font_size : 14.0f) * scale;
    gui_font* f       = font__at(s->font_family, size_px);
    float     line    = f != NULL ? font__line_height(f) : size_px;
    float     ascent  = f != NULL ? font__ascent(f)      : size_px;
    gui_color text_c  = s->has_font_color ? s->font_color : scene__rgb(0.96f, 0.96f, 0.98f);
    //
    // Same opacity-pin rationale as the panel. If an inherited appear
    // animation faded the text alpha, the option labels would look
    // ghosted against the panel.
    //
    text_c.a = 1.0f;

    //
    // Selected-row highlight color: accent-color with transparent
    // alpha so the row reads as "tinted" rather than fully opaque.
    //
    gui_color sel_c;
    if (s->has_accent_color)
    {
        sel_c   = s->accent_color;
        sel_c.a = 0.25f;
    }
    else
    {
        sel_c = scene__rgba(0.39f, 0.40f, 0.95f, 0.25f);
    }

    float pad = 12.0f * scale;
    for (int64 i = 0; i < count; i++)
    {
        gui_rect row;
        row.x = popup.x;
        row.y = popup.y + (float)i * row_h;
        row.w = popup.w;
        row.h = row_h;

        //
        // Highlight tint for the currently selected row.
        //
        if (i == (int64)n->value)
        {
            renderer__submit_rect(row, sel_c, s->radius * scale);
        }

        //
        // Option text.
        //
        if (f != NULL)
        {
            gui_node* opt = _widget_select_internal__option_at(n, i);
            if (opt != NULL && opt->text[0] != 0)
            {
                float ty = row.y + (row.h - line) * 0.5f + ascent;
                font__draw(f, row.x + pad, ty, text_c, opt->text);
            }
        }
    }

    //
    // Pop the scissor we pushed at the top. This flushes the popup's
    // own rect + text batches in the correct order (rects first, then
    // text), so the popup's option text renders ON TOP of its panel
    // background -- the entire goal of the scissor bracketing here.
    //
    renderer__pop_scissor();
}

//
// Clear the scene overlay slot if it currently points at this
// node -- otherwise scene__emit_draws's overlay walk on the next
// frame would dereference a freed pointer (crash on Android,
// undefined behaviour on Windows). Hot reload + any other code
// path that frees a select node mid-open must hit this so we
// don't leak the dangling pointer into the scene.
//
static void select_on_destroy(gui_node* n)
{
    if (scene__overlay_node() == n)
    {
        gui_rect empty = { 0.0f, 0.0f, 0.0f, 0.0f };
        scene__set_overlay(NULL, empty, NULL);
    }
}

static const widget_vtable g_select_vtable = {
    .type_name       = "select",
    .init_defaults   = select_init_defaults,
    .apply_attribute = select_apply_attribute,
    .layout          = select_layout,
    .emit_draws      = select_emit_draws,
    .on_mouse_up     = select_on_mouse_up,
    .on_destroy      = select_on_destroy,
    .consumes_click  = TRUE,
};

void widget_select__register(void)
{
    widget_registry__register(GUI_NODE_SELECT, &g_select_vtable);
}
