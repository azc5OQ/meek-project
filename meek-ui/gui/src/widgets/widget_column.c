//
//widget_column.c - vertical container with auto-height.
//
//two-pass layout: first lay out each child speculatively at (0,0) to
//read its measured height, then sum + pad + gap to derive the column's
//own height, then the standard column placement helper does the real
//positioning. style.size_w/size_h overrides the auto sizing.
//

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"

static void column_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style* s = &n->resolved;

    float w = scene__layout_width(n, avail_w, scale);

    //
    // Border eats from BOTH sides of each axis. Subtract 2*bw from the
    // measure-pass child width so children probe into the inner area
    // (matches the real placement done by scene__layout_children_as_column,
    // which applies the same shrink). bw is 0 for unbordered columns.
    //
    float bw = scene__border_width(n, scale);

    //
    //speculative measure pass: lay each child out at (0,0) just to
    //read bounds.h. real positioning happens via
    //scene__layout_children_as_column below.
    //
    float child_total = 0.0f;
    int64 child_index = 0;
    {
        gui_node* c           = n->first_child;
        float     child_avail = w - (s->pad_l + s->pad_r) * scale - 2.0f * bw;
        if (child_avail < 0.0f) { child_avail = 0.0f; }
        while (c != NULL)
        {
            //
            // display: none children take no measured height and no
            // inter-child gap: they must be invisible to the
            // auto-sizing column.
            //
            if (c->resolved.display == GUI_DISPLAY_NONE)
            {
                c = c->next_sibling;
                continue;
            }
            scene__layout_node(c, 0.0f, 0.0f, child_avail, 0.0f);
            child_total += c->bounds.h;
            if (child_index > 0)
            {
                child_total += s->gap * scale;
            }
            child_index++;
            c = c->next_sibling;
        }
    }

    //
    // Final height priority: explicit pixel (size_h) > percent of
    // parent (height_pct) > auto-size to children. Without the
    // height_pct branch, a column that declares `height: 100%`
    // silently collapses to the sum of its children -- which
    // breaks "fullscreen overlay" patterns where the column should
    // fill the remaining space even if its children have auto /
    // percent heights of their own (infinite recursion would
    // otherwise loop through scene__layout_children_as_column).
    //
    // Auto-height adds 2*bw so the top/bottom border lines have
    // room outside the children.
    //
    float h;
    if (s->size_h > 0.0f)          { h = s->size_h * scale; }
    else if (s->height_pct > 0.0f) { h = avail_h * (s->height_pct / 100.0f); }
    else                           { h = child_total + (s->pad_t + s->pad_b) * scale + 2.0f * bw; }

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
    scene__layout_children_as_column(n);
}

static const widget_vtable g_column_vtable = {
    .type_name        = "column",
    .init_defaults    = NULL,
    .apply_attribute  = NULL,
    .layout           = column_layout,
    .emit_draws       = NULL,
    .on_mouse_down    = NULL,
    .on_mouse_up      = NULL,
    .on_mouse_drag    = NULL,
    .consumes_click   = FALSE,
};

void widget_column__register(void)
{
    widget_registry__register(GUI_NODE_COLUMN, &g_column_vtable);
}
