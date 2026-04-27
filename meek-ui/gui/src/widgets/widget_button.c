//
//widget_button.c - clickable rectangle with optional centered text.
//
//size comes from style (size: WxH;) with a small default if
//unspecified. default click semantics: scene's input state machine
//routes mouse-up-on-same-node-as-mouse-down to the on_click handler.
//
//if node->text is non-empty, the button also draws the text centered
//both horizontally and vertically inside its bounds using the font
//specified in the resolved style (font_family + font_size).
//

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "font.h"

static void button_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_w;
    (void)avail_h;
    gui_style* s = &n->resolved;
    float w = (s->size_w > 0.0f) ? s->size_w * scale : 120.0f * scale;
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 32.0f * scale;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

//
//centered text: measure width, position so left = bounds.x + (w-tw)/2;
//y position uses ascent so the text's visual baseline sits near the
//vertical center. font_size is multiplied by scale so text grows with
//the scale slider.
//
static void button_emit_draws(gui_node* n, float scale)
{
    //
    // Route bg + border through the shared helper so buttons get
    // background-gradient, box-shadow, bg-image, blur, and border-
    // gradient for free (same bundle the container widgets use).
    // Previously this function hand-rolled `if (has_bg_color)
    // submit_rect` which silently ignored every other bg property.
    //
    scene__emit_default_bg(n, scale);

    if (n->text_len <= 0 && n->text[0] == 0)
    {
        return;
    }

    //
    //request the font at (family, scaled size). font_size of 0 falls
    //back to a default inside font__at.
    //
    float     size_px = n->resolved.font_size > 0.0f ? n->resolved.font_size : 14.0f;
    size_px *= scale;
    gui_font* f = font__at(n->resolved.font_family, size_px);
    if (f == NULL)
    {
        return; // no font registered.
    }

    float text_w = font__measure(f, n->text);
    float ascent = font__ascent(f);
    float line_h = font__line_height(f);

    //
    //center horizontally; place baseline such that the line's visual
    //center (ascent/2 ish) lands at the button's vertical center.
    //close enough; a real text engine would account for cap height.
    //
    float tx = n->bounds.x + (n->bounds.w - text_w) * 0.5f;
    float ty = n->bounds.y + (n->bounds.h - line_h) * 0.5f + ascent;

    gui_color color = n->resolved.has_font_color ? n->resolved.font_color : scene__rgb(1.0f, 1.0f, 1.0f);
    font__draw(f, tx, ty, color, n->text);
}

static const widget_vtable g_button_vtable = {
    .type_name        = "button",
    .init_defaults    = NULL,
    .apply_attribute  = NULL,
    .layout           = button_layout,
    .emit_draws       = button_emit_draws,
    .on_mouse_down    = NULL,
    .on_mouse_up      = NULL,
    .on_mouse_drag    = NULL,
    .on_char          = NULL,
    .on_key           = NULL,
    .consumes_click   = FALSE,
    .takes_focus      = FALSE,
};

void widget_button__register(void)
{
    widget_registry__register(GUI_NODE_BUTTON, &g_button_vtable);
}
