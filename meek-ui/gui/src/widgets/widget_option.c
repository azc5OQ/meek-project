//
// widget_option.c - a single entry inside a <select> dropdown.
//
//   <select id="size">
//       <option text="Small" />
//       <option text="Medium" value="M" />
//       <option text="Large" />
//   </select>
//
// The option is intentionally degenerate as a widget -- it carries a
// `text` attribute (which the parser already puts into node->text via
// the generic attribute-handling path in parser_xml) and nothing else.
// The PARENT <select> walks its children at render time to enumerate
// option labels; the option itself never draws and takes no layout
// space (bounds = 0,0,0,0).
//
// This mirrors how HTML's <option> works inside <select>: the option
// is meaningful only as a child of its parent, which decides when and
// where (if ever) to render it.
//
// The `value` attribute is accepted for API parity with HTML but the
// MVP select identifies options by their INDEX within the select's
// child list, not by their value string. Wiring value-based lookup
// is a deferred cleanup.
//

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"

static void option_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_w;
    (void)avail_h;
    (void)scale;
    //
    // Zero-size bounds. The option never draws in normal layout and
    // never responds to input -- it's a data carrier for the parent
    // <select>'s own popup rendering + hit-testing.
    //
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = 0.0f;
    n->bounds.h = 0.0f;
}

static const widget_vtable g_option_vtable = {
    .type_name  = "option",
    .layout     = option_layout,
    // No emit_draws -- default "submit bg rect if has_bg" is harmless
    // with a zero-size rect, so we don't even need a NULL-override.
    // No input hooks -- options can't be clicked directly.
};

void widget_option__register(void)
{
    widget_registry__register(GUI_NODE_OPTION, &g_option_vtable);
}
