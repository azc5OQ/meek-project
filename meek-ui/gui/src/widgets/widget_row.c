//
// widget_row.c - horizontal container with auto-size.
//
// Two-pass layout mirroring widget_column.c's shape, but along the
// other axis: first lay each child out speculatively at (0,0) to read
// its measured bounds.w / bounds.h, then derive the row's own width +
// height from the sum / max of those, then call the standard row
// placement helper on scene to do the real positioning.
//
// Before this file was properly wired, <Row> set its own bounds.h to
// 0 and never dispatched layout to its children -- so every Text,
// checkbox, and radio sitting inside a Row retained the (0,0,0,0)
// bounds left from scene__node_new's calloc, and all of them rendered
// on top of each other at the top-left corner of the window. Visible
// as strings like "System notifications" + "Share anonymous usage
// data" overlapping into a garbled "Sylstem notificaussage data" in
// the screenshot. Fixed by giving Row the same two-pass layout as
// Column, just along the opposite axis.
//
//   <Row>
//     <checkbox .../>
//     <Text text="Send me notifications" />
//   </Row>
//
// Style hooks:
//   size_w / size_h   explicit dimensions; 0 = auto-size.
//   pad_l..pad_b      inner padding honored by the placement helper.
//   gap               horizontal spacing between children.
//

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"

static void row_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_h;
    gui_style* s = &n->resolved;

    //
    // Speculative measure pass. Each child lays out at (0,0) with
    // the full inner-width so that auto-sized children (Text widgets,
    // checkbox defaults) report their natural size in bounds.w/.h.
    // Sum widths across siblings + gaps for the row's own width;
    // max height for the row's own height.
    //
    //
    // Border eats from BOTH sides of each axis. Subtract 2*bw from the
    // measure-pass child width so children probe into the inner area
    // (matches the real placement done by scene__layout_children_as_row,
    // which applies the same shrink). bw is 0 for unbordered rows.
    //
    float bw          = scene__border_width(n, scale);
    float candidate_w = scene__layout_width(n, avail_w, scale);
    float inner_w     = candidate_w - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
    if (inner_w < 0.0f)
    {
        inner_w = 0.0f;
    }
    //
    // For the speculative pass we don't yet know the row's height, so
    // pass the parent-supplied avail_w (minus padding) as height hint
    // too -- most children ignore avail_h anyway (Text auto-sizes to
    // line_height, checkbox to 20x20). Children that DO want to fill
    // height get a reasonable upper bound.
    //
    float child_total_w = 0.0f;
    float child_max_h   = 0.0f;
    int64 child_index   = 0;
    {
        gui_node* c = n->first_child;
        while (c != NULL)
        {
            //
            // display: none children contribute 0 to both width and
            // height (including the gap that would follow them), so
            // the row collapses around the gap exactly as CSS does.
            //
            if (c->resolved.display == GUI_DISPLAY_NONE)
            {
                c = c->next_sibling;
                continue;
            }
            scene__layout_node(c, 0.0f, 0.0f, inner_w, 0.0f);
            child_total_w += c->bounds.w;
            if (child_index > 0)
            {
                child_total_w += s->gap * scale;
            }
            if (c->bounds.h > child_max_h)
            {
                child_max_h = c->bounds.h;
            }
            child_index++;
            c = c->next_sibling;
        }
    }

    //
    // Final sizing. size_w / size_h pins override the measured
    // values; otherwise we take the summed width + padding (bounded
    // by avail_w so we don't overflow our parent) and max-child-height
    // + padding for height.
    //
    //
    // Auto-sizing adds 2*bw on each axis so the border lines have
    // room outside the children, matching the inner-area shrink the
    // placement helper applies.
    //
    //
    // Final width priority: explicit pixel (size_w) > percent of parent
    // (width_pct) > auto-size to children. Percent matters for layouts
    // that want a row to span the parent (e.g. a full-width topbar)
    // rather than hugging its children. Without the width_pct branch
    // `width: 100%` was silently ignored and the row collapsed to the
    // natural width of its contents.
    //
    float w;
    if (s->size_w > 0.0f)         { w = s->size_w * scale; }
    else if (s->width_pct > 0.0f) { w = avail_w * (s->width_pct / 100.0f); }
    else
    {
        w = child_total_w + (s->pad_l + s->pad_r) * scale + 2.0f * bw;
        if (w > avail_w) { w = avail_w; }
    }

    //
    // Final height: pixel > percent-of-avail > auto.
    //
    float h;
    if (s->size_h > 0.0f)          { h = s->size_h * scale; }
    else if (s->height_pct > 0.0f) { h = avail_h * (s->height_pct / 100.0f); }
    else                           { h = child_max_h + (s->pad_t + s->pad_b) * scale + 2.0f * bw; }

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;

    //
    // Real placement pass. The shared helper walks children and lays
    // them out at (cursor_x, cy) with cursor_x advancing by child.w +
    // gap. Same math pattern as scene__layout_children_as_column,
    // just transposed.
    //
    scene__layout_children_as_row(n);
}

static const widget_vtable g_row_vtable = {
    .type_name        = "row",
    .init_defaults    = NULL,
    .apply_attribute  = NULL,
    .layout           = row_layout,
    .emit_draws       = NULL,
    .on_mouse_down    = NULL,
    .on_mouse_up      = NULL,
    .on_mouse_drag    = NULL,
    .consumes_click   = FALSE,
};

void widget_row__register(void)
{
    widget_registry__register(GUI_NODE_ROW, &g_row_vtable);
}
