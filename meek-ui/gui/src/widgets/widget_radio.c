//
// widget_radio.c - radio button (one-of-many selector).
//
//   <radio class="size" id="sz-small"  on_change="on_size" />
//   <radio class="size" id="sz-medium" on_change="on_size" value="1" />
//   <radio class="size" id="sz-large"  on_change="on_size" />
//
// VISUAL:
//   - Outer CIRCLE (high-radius rounded square, radius = min(w,h)/2).
//   - When selected (n->value != 0) draws an inner filled circle
//     centered inside with ~40% of the outer's diameter.
//
// GROUPING (the one-of-N semantics):
//   - Radios with the SAME CLASS form a group. Selecting one
//     deselects every other radio in the same class -- matches HTML's
//     radios-with-same-name-attribute semantics but repurposes the
//     class attribute since we already have that.
//   - Group enforcement walks the scene tree from root on every click
//     to find siblings. O(N_radios) per click which is fine for the
//     sizes of radio groups real apps have (handful).
//   - Radios with NO class are treated as independent toggles (their
//     own one-button group). Rarely useful but well-defined.
//
// INPUT:
//   - On mouse_up INSIDE the widget: walk the tree, set this radio's
//     value = 1 and every other same-class radio's value = 0. Then
//     dispatch on_change on THIS radio with the new value.
//   - Clicking an already-selected radio is a no-op (consistent with
//     HTML + native behavior).
//
// STYLING HOOKS:
//   bg / has_bg   -- outer ring bg.
//   fg / has_fg   -- inner filled dot when selected.
//   :hover / :pressed / :appear -- same semantics as any other widget.
//

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"

static boole radio_apply_attribute(gui_node* n, char* name, char* value)
{
    //
    // `value=` (legacy slot) and `checked=` (CSS-muscle-memory
    // alias) both mean "start selected if truthy". Note: if the
    // author passes a non-boolean `value=` (e.g. value="dark" as
    // an option identifier), we'll dutifully set n->value = 0 --
    // that matches the HTML idiom where `value="dark"` alone does
    // NOT select a radio; the selection requires `checked`.
    //
    if (strcmp(name, "value") == 0 || strcmp(name, "checked") == 0)
    {
        if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "selected") == 0 || strcmp(value, "checked") == 0)
        {
            n->value = 1.0f;
        }
        else
        {
            n->value = 0.0f;
        }
        return TRUE;
    }
    //
    // `group=` is sugar: radios with the same group act as mutually
    // exclusive siblings. We implement grouping via class (see the
    // file-header comment), so `group="theme"` just becomes
    // `class="theme"`. Doesn't overwrite an existing class if one
    // was already set -- last-write-wins per file order matches
    // the parser's general attr semantics.
    //
    if (strcmp(name, "group") == 0)
    {
        size_t m = strlen(value);
        if (m >= sizeof(n->klass))
        {
            m = sizeof(n->klass) - 1;
        }
        memcpy(n->klass, value, m);
        n->klass[m] = 0;
        return TRUE;
    }
    return FALSE;
}

static void radio_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_w;
    (void)avail_h;
    gui_style* s = &n->resolved;
    float w = (s->size_w > 0.0f) ? s->size_w * scale : 20.0f * scale;
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 20.0f * scale;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static void radio_emit_draws(gui_node* n, float scale)
{
    (void)scale;
    gui_style* s = &n->resolved;

    //
    // Outer circle = rounded square with radius = half the shorter
    // side. The renderer's SDF path clamps radius to that max anyway,
    // so this just makes the intent explicit.
    //
    float outer_r = (n->bounds.w < n->bounds.h ? n->bounds.w : n->bounds.h) * 0.5f;
    gui_color outer = s->has_background_color ? s->background_color : scene__rgb(0.22f, 0.23f, 0.28f);
    renderer__submit_rect(n->bounds, outer, outer_r);

    //
    // Inner dot when selected. ~40% of the outer diameter, centered.
    //
    if (n->value != 0.0f)
    {
        float inner_side = n->bounds.w * 0.40f;
        if (inner_side > n->bounds.h * 0.40f) { inner_side = n->bounds.h * 0.40f; }

        gui_rect inner;
        inner.x = n->bounds.x + (n->bounds.w - inner_side) * 0.5f;
        inner.y = n->bounds.y + (n->bounds.h - inner_side) * 0.5f;
        inner.w = inner_side;
        inner.h = inner_side;

        gui_color fill = s->has_accent_color ? s->accent_color : scene__rgb(0.39f, 0.40f, 0.95f);
        renderer__submit_rect(inner, fill, inner_side * 0.5f);
    }
}

//
// Recursive helper: walk subtree and deselect every radio whose class
// matches `group`. Skips `self` so we don't immediately unselect the
// button the user clicked.
//
static void _widget_radio_internal__deselect_group(gui_node* n, char* group, gui_node* self)
{
    if (n == NULL) { return; }
    if (n != self && n->type == GUI_NODE_RADIO)
    {
        if (strcmp(n->klass, group) == 0)
        {
            n->value = 0.0f;
        }
    }
    gui_node* c = n->first_child;
    while (c != NULL)
    {
        _widget_radio_internal__deselect_group(c, group, self);
        c = c->next_sibling;
    }
}

static void radio_on_mouse_up(gui_node* n, int64 x, int64 y, int64 button)
{
    (void)button;
    //
    // Only act when the release lands inside our bounds.
    //
    float fx = (float)x, fy = (float)y;
    if (fx < n->bounds.x || fx >= n->bounds.x + n->bounds.w ||
        fy < n->bounds.y || fy >= n->bounds.y + n->bounds.h)
    {
        return;
    }
    //
    // Already selected -> no-op + no event (HTML behavior).
    //
    if (n->value != 0.0f)
    {
        return;
    }

    //
    // Deselect siblings in the same class-group, then select self.
    //
    gui_node* root = scene__root();
    if (root != NULL && n->klass[0] != 0)
    {
        _widget_radio_internal__deselect_group(root, n->klass, n);
    }
    n->value = 1.0f;
    scene__dispatch_change(n, n->value);
}

static const widget_vtable g_radio_vtable = {
    .type_name       = "radio",
    .apply_attribute = radio_apply_attribute,
    .layout          = radio_layout,
    .emit_draws      = radio_emit_draws,
    .on_mouse_up     = radio_on_mouse_up,
    .consumes_click  = TRUE,
};

void widget_radio__register(void)
{
    widget_registry__register(GUI_NODE_RADIO, &g_radio_vtable);
}
