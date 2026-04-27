//
//widget_input.c - single-line text input.
//
//  mouse-down   -> takes focus (scene tracks the focused node globally).
//  on_char      -> appends the typed codepoint to node->text.
//  on_key       -> BACKSPACE removes the last byte of node->text.
//                  (arrow keys, delete, enter, ctrl+ shortcuts are all
//                  future work.)
//  on_change    -> fired after every text mutation with the new length
//                  in ev.change.scalar (so host apps can mirror state).
//
//rendered like a button but:
//  - text is left-aligned with a small horizontal padding.
//  - a thin vertical cursor bar is drawn at the end of the text when
//    the node is focused.
//
//limitations (v1):
//  - ASCII + Latin-1 only (matches the font atlas range).
//  - no text selection, no caret navigation.
//  - no clipping -- if the text gets wider than the bounds, it
//    renders past the right edge. fine for a poc.
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
//inner text padding in pixels, relative to bounds.
//
#define _WIDGET_INPUT_INTERNAL__HPAD 8.0f

static void input_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_w;
    (void)avail_h;
    gui_style* s = &n->resolved;
    float w = (s->size_w > 0.0f) ? s->size_w * scale : 240.0f * scale;
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 32.0f * scale;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static void input_emit_draws(gui_node* n, float scale)
{
    //
    // bg + border via the shared helper so inputs pick up gradient,
    // shadow, bg-image, blur, and border-gradient automatically.
    // Text / cursor / selection are submitted separately below.
    //
    scene__emit_default_bg(n, scale);

    float     size_px = (n->resolved.font_size > 0.0f ? n->resolved.font_size : 14.0f) * scale;
    gui_font* f       = font__at(n->resolved.font_family, size_px);

    //
    //text, left-aligned with horizontal padding.
    //
    float text_x = n->bounds.x + _WIDGET_INPUT_INTERNAL__HPAD * scale;
    float ascent = f != NULL ? font__ascent(f) : 0.0f;
    float line_h = f != NULL ? font__line_height(f) : size_px;
    float text_y = n->bounds.y + (n->bounds.h - line_h) * 0.5f + ascent;

    gui_color color = n->resolved.has_font_color ? n->resolved.font_color : scene__rgb(1.0f, 1.0f, 1.0f);

    if (f != NULL && n->text[0] != 0)
    {
        font__draw(f, text_x, text_y, color, n->text);
    }

    //
    //cursor at end of text, only when focused. thin vertical bar
    //(2*scale pixels wide, centered on the pen position after the
    //last character's advance). uses the resolved text color.
    //
    if (scene__focus() == n)
    {
        float cursor_x = text_x + (f != NULL ? font__measure(f, n->text) : 0.0f);
        float cursor_w = 2.0f * scale;
        float cursor_h = line_h;
        float cursor_y = n->bounds.y + (n->bounds.h - cursor_h) * 0.5f;

        gui_rect r;
        r.x = cursor_x;
        r.y = cursor_y;
        r.w = cursor_w;
        r.h = cursor_h;
        renderer__submit_rect(r, color, 0.0f);
    }
}

static void input_on_char(gui_node* n, uint codepoint)
{
    //
    //only accept printable ASCII+Latin-1 for now (matches font atlas
    //range). control characters (including the one WM_CHAR emits for
    //Backspace = 0x08) fall out here -- backspace comes through
    //on_key instead.
    //
    if (codepoint < 32 || codepoint > 255)
    {
        return;
    }
    //
    // Capacity check is intentionally `text_len + 1 >= sizeof`, not
    // `text_len + 1 > sizeof`. After the append we'll write to
    // text[text_len] (the new char) AND text[text_len + 1] (the
    // null), so we need text[text_len + 1] to be a valid byte:
    // (text_len + 1) < sizeof, i.e. abort when (text_len + 1) >=
    // sizeof. Looks like an off-by-one but is correct -- if you
    // catch yourself "fixing" this to `>`, trace through with
    // sizeof=8 first.
    //
    if (n->text_len + 1 >= (int64)sizeof(n->text))
    {
        return;
    }
    n->text[n->text_len++] = (char)codepoint;
    n->text[n->text_len]   = 0;
    scene__dispatch_change(n, (float)n->text_len);
}

static void input_on_key(gui_node* n, int64 vk, boole down)
{
    if (!down)
    {
        return;
    }
    //
    //VK_BACK = 0x08. we compare by value rather than #include'ing
    //<windows.h> here (keeps scene input-dispatcher-neutral; the
    //vk code is whatever the platform layer feeds us).
    //
    if (vk == 0x08) // VK_BACK
    {
        if (n->text_len > 0)
        {
            n->text_len--;
            n->text[n->text_len] = 0;
            scene__dispatch_change(n, (float)n->text_len);
        }
    }
}

//
// `placeholder=` is an HTML-muscle-memory alias for `text=` on
// empty inputs. Copies into node->text the same way the generic
// `text=` attribute would, so stylesheet/parser code downstream
// doesn't need to know there's a difference. Authors can still
// write `text=` if they prefer.
//
static boole input_apply_attribute(gui_node* n, char* name, char* value)
{
    if (strcmp(name, "placeholder") == 0)
    {
        size_t m = strlen(value);
        if (m >= sizeof(n->text))
        {
            m = sizeof(n->text) - 1;
        }
        memcpy(n->text, value, m);
        n->text[m]  = 0;
        n->text_len = (int64)m;
        return TRUE;
    }
    return FALSE;
}

static const widget_vtable g_input_vtable = {
    .type_name       = "input",
    .apply_attribute = input_apply_attribute,
    .layout          = input_layout,
    .emit_draws      = input_emit_draws,
    .on_char         = input_on_char,
    .on_key          = input_on_key,
    .takes_focus     = TRUE,
};

void widget_input__register(void)
{
    widget_registry__register(GUI_NODE_INPUT, &g_input_vtable);
}
