//
// widget_checkbox.c - a two-state toggle (unchecked / checked).
//
//   <checkbox id="accept-tos" on_change="on_tos_changed" />
//   <checkbox id="remember"   on_change="on_remember_changed" value="1" />
//
// VISUAL:
//   - Outer rounded square drawn at `bounds` with the resolved bg.
//   - When checked (n->value != 0), an inner rounded square is drawn
//     inside the outer one with the resolved fg color. The inner rect
//     is inset by ~25% on all sides so the outer border acts as a
//     visible frame.
//
// STATE:
//   - n->value holds 0 (unchecked) or 1 (checked). Reuses the slider's
//     scalar `value` field; checkboxes ignore value_min / value_max.
//   - Click (mouse_down + mouse_up on the same widget) toggles the
//     value and dispatches on_change with the new scalar (0.0 or 1.0).
//
// INPUT:
//   - Uses scene's default click semantics: on mouse_up, if release
//     landed on the same widget as mouse_down, scene dispatches the
//     on_click handler. We hijack on_mouse_up instead of on_click so
//     the toggle is visible even without an explicit on_click="..."
//     attribute, and dispatch on_change ourselves for the typical
//     "boolean-setting changed" handler pattern.
//
// STYLING HOOKS:
//   bg         -- outer square bg (container).
//   fg         -- inner filled square color when checked.
//   radius     -- corner radius of the outer square (inner uses a
//                 proportional smaller radius).
//   :hover     -- swap bg/fg for a highlight on mouseover.
//   :pressed   -- swap bg for a darker shade during the press.
//   :appear    -- supported like every other widget; the inner fill
//                 also respects the animator's alpha fade.
//

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"

//
// Per-type defaults. Checkboxes default to 20x20 so they're tappable on
// touch devices without being huge. Overrideable via size: in .style.
//
static void checkbox_init_defaults(gui_node* n)
{
    (void)n;
    // nothing to initialize beyond the zero-initialized state --
    // value = 0 (unchecked) is the sensible default.
}

static boole checkbox_apply_attribute(gui_node* n, char* name, char* value)
{
    //
    // Accept both `value=` (our original slot) and `checked=`
    // (HTML/CSS muscle memory). Truthy strings set the node's
    // value to 1; anything else leaves it 0.
    //
    if (strcmp(name, "value") == 0 || strcmp(name, "checked") == 0)
    {
        if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "checked") == 0)
        {
            n->value = 1.0f;
        }
        else
        {
            n->value = 0.0f;
        }
        return TRUE;
    }
    return FALSE;
}

static void checkbox_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
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

static void checkbox_emit_draws(gui_node* n, float scale)
{
    gui_style* s = &n->resolved;

    //
    // Outer square. Uses bg + radius like any other widget. Fallback
    // to a neutral gray if the style didn't set a bg -- easier to
    // spot than an invisible rect while iterating on .style.
    //
    gui_color outer = s->has_background_color ? s->background_color : scene__rgb(0.22f, 0.23f, 0.28f);
    renderer__submit_rect(n->bounds, outer, s->radius * scale);

    //
    // Inner filled square (only when checked). Inset by 25% on each
    // side so the outer forms a visible border. Inner radius is
    // scaled proportionally so the visual rhythm matches the outer.
    //
    if (n->value != 0.0f)
    {
        float inset = n->bounds.w * 0.25f;
        if (inset > n->bounds.h * 0.25f) { inset = n->bounds.h * 0.25f; }

        gui_rect inner;
        inner.x = n->bounds.x + inset;
        inner.y = n->bounds.y + inset;
        inner.w = n->bounds.w - inset * 2.0f;
        inner.h = n->bounds.h - inset * 2.0f;

        gui_color fill = s->has_accent_color ? s->accent_color : scene__rgb(0.39f, 0.40f, 0.95f); // default indigo tick.
        renderer__submit_rect(inner, fill, s->radius * scale * 0.5f);
    }
}

//
// Toggle on release-over-self. We use on_mouse_up rather than on_click
// so the toggle happens deterministically even when the host didn't
// supply an on_click="" attribute.
//
static void checkbox_on_mouse_up(gui_node* n, int64 x, int64 y, int64 button)
{
    (void)button;
    //
    // Make sure the release landed on us. scene tracks the "pressed"
    // node and calls our on_mouse_up with the original pressed node,
    // but scene also passes the release coordinates so we can confirm
    // the cursor is still inside our bounds -- prevents "drag off to
    // cancel" style toggles, matching native platform behavior.
    //
    float fx = (float)x, fy = (float)y;
    if (fx < n->bounds.x || fx >= n->bounds.x + n->bounds.w ||
        fy < n->bounds.y || fy >= n->bounds.y + n->bounds.h)
    {
        return;
    }

    //
    // Flip and fire on_change. scene__dispatch_change is a no-op when
    // the node has no on_change handler wired.
    //
    n->value = (n->value != 0.0f) ? 0.0f : 1.0f;
    scene__dispatch_change(n, n->value);
}

static const widget_vtable g_checkbox_vtable = {
    .type_name       = "checkbox",
    .init_defaults   = checkbox_init_defaults,
    .apply_attribute = checkbox_apply_attribute,
    .layout          = checkbox_layout,
    .emit_draws      = checkbox_emit_draws,
    .on_mouse_up     = checkbox_on_mouse_up,
    //
    // consumes_click = TRUE so scene doesn't ALSO fire our on_click
    // handler (we handle the whole interaction in on_mouse_up).
    //
    .consumes_click  = TRUE,
};

void widget_checkbox__register(void)
{
    widget_registry__register(GUI_NODE_CHECKBOX, &g_checkbox_vtable);
}
