//
// widget_textarea.c - multi-line text input.
//
// Like <input> but text wraps to the widget's width and literal '\n'
// bytes break to a new line. Cursor lives at the end of the text;
// mid-text editing + arrow navigation are deferred (matches the
// current <input> widget's scope).
//
// Typing flow:
//   on_char        append printable ASCII/Latin-1 byte to text[].
//   on_key         VK_BACK (0x08) removes the last byte (handles \n too);
//                  VK_RETURN (0x0D) inserts a '\n'.
//   on_change      fires after every mutation with the new byte length
//                  in ev.change.scalar (same contract as <input>).
//
// Layout: the widget reserves its full styled height; text clips at
// the bottom. A scrollable variant would use overflow-y handling from
// widget_div; left for later.
//

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "font.h"

#define _WIDGET_TEXTAREA_INTERNAL__HPAD 8.0f
#define _WIDGET_TEXTAREA_INTERNAL__VPAD 6.0f

//
// Walk text from `start` forward and find the byte index at which a
// line break falls. Returns the number of bytes to draw on the
// current line (excluding the break). Break priorities, high to low:
//   1. an explicit '\n' (consumes it -- caller skips 1 byte past)
//   2. last whitespace before content would overflow wrap_w
//   3. a hard break at the character that would cause the overflow
// Third case yields character-level wrapping for tokens wider than
// the widget (long URLs, code, etc.), matching CSS
// `overflow-wrap: anywhere` behaviour.
//
static int64 _widget_textarea_internal__line_len(gui_font* f, char* text, int64 from, int64 total, float wrap_w, boole* out_break_is_newline)
{
    *out_break_is_newline = FALSE;
    if (f == NULL || text == NULL) { return total - from; }
    float x           = 0.0f;
    int64 last_break  = -1;
    for (int64 i = from; i < total; i++)
    {
        char c = text[i];
        if (c == '\n')
        {
            *out_break_is_newline = TRUE;
            return i - from;
        }
        if (c == ' ' || c == '\t')
        {
            last_break = i;
        }
        float advance = font__measure_n(f, &text[i], 1);
        x += advance;
        if (x > wrap_w && last_break >= from)
        {
            //
            // break at the last whitespace we saw on this line.
            // caller skips the whitespace byte via the "+1" advance
            // on soft-wrap returns (handled below).
            //
            return last_break - from;
        }
        if (x > wrap_w && i > from)
        {
            //
            // long run with no break. split at this character (hard
            // break) -- returning i - from, caller picks up at i.
            //
            return i - from;
        }
    }
    return total - from;
}

static void textarea_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style* s = &n->resolved;
    float w = (s->size_w > 0.0f) ? s->size_w * scale : (avail_w > 0.0f ? avail_w : 240.0f * scale);
    float h = (s->size_h > 0.0f) ? s->size_h * scale : 120.0f * scale;
    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static void textarea_emit_draws(gui_node* n, float scale)
{
    scene__emit_default_bg(n, scale);

    float     size_px = (n->resolved.font_size > 0.0f ? n->resolved.font_size : 14.0f) * scale;
    gui_font* f       = font__at(n->resolved.font_family, size_px);
    if (f == NULL) { return; }

    float line_h  = font__line_height(f);
    float ascent  = font__ascent(f);
    float wrap_w  = n->bounds.w - 2.0f * _WIDGET_TEXTAREA_INTERNAL__HPAD * scale;
    if (wrap_w < 1.0f) { wrap_w = 1.0f; }

    float text_x0 = n->bounds.x + _WIDGET_TEXTAREA_INTERNAL__HPAD * scale;
    float text_y  = n->bounds.y + _WIDGET_TEXTAREA_INTERNAL__VPAD * scale + ascent;
    float bottom  = n->bounds.y + n->bounds.h - _WIDGET_TEXTAREA_INTERNAL__VPAD * scale;

    gui_color color = n->resolved.has_font_color ? n->resolved.font_color : scene__rgb(1.0f, 1.0f, 1.0f);
    color.a *= n->effective_opacity;

    //
    // Walk the text one wrapped line at a time. line_len returns the
    // number of bytes on the current line (excluding any break char).
    // If the break was a '\n' we step 1 byte past it; if it was a soft
    // whitespace wrap we step 1 byte past the space to avoid leading
    // it on the next line. A hard break (no whitespace in the line)
    // resumes at the same position with no skip.
    //
    int64 i           = 0;
    int64 total       = n->text_len;
    float cursor_x    = text_x0;
    float cursor_y    = text_y;
    while (i < total)
    {
        boole was_newline = FALSE;
        int64 len = _widget_textarea_internal__line_len(f, n->text, i, total, wrap_w, &was_newline);
        if (len > 0)
        {
            font__draw_n(f, text_x0, text_y, color, &n->text[i], len);
        }
        //
        // Cursor lives at end of text: track pen position for the
        // LAST line so we can draw the caret after the walk. Note
        // that `text_y` is the BASELINE for this line.
        //
        cursor_x = text_x0 + ((len > 0) ? font__measure_n(f, &n->text[i], len) : 0.0f);
        cursor_y = text_y;

        if (was_newline)
        {
            //
            // Explicit newline -- skip the '\n' byte itself.
            //
            i += len + 1;
            //
            // A trailing newline means the caret is at column 0 of a
            // fresh line; reflect that.
            //
            if (i >= total)
            {
                cursor_x = text_x0;
                cursor_y = text_y + line_h;
            }
        }
        else if (i + len < total)
        {
            //
            // Soft wrap. If we broke at a whitespace, skip it on the
            // next line so the new line doesn't begin with a dangling
            // space. For a hard break (broken at a non-whitespace),
            // don't skip -- the character that caused the overflow
            // was beyond `len`, so we resume at i + len regardless.
            //
            char break_ch = n->text[i + len];
            if (break_ch == ' ' || break_ch == '\t')
            {
                i += len + 1;
            }
            else
            {
                i += len;
            }
        }
        else
        {
            i += len;
        }
        text_y += line_h;
        if (text_y - ascent > bottom) { break; }
    }

    if (scene__focus() == n)
    {
        gui_rect cur;
        cur.x = cursor_x;
        cur.y = cursor_y - ascent;
        cur.w = 2.0f * scale;
        cur.h = line_h;
        renderer__submit_rect(cur, color, 0.0f);
    }
}

static void textarea_on_char(gui_node* n, uint codepoint)
{
    //
    // Accept printable ASCII + Latin-1. Control bytes come through
    // on_key instead (backspace, return).
    //
    if (codepoint < 32 || codepoint > 255)
    {
        return;
    }
    //
    // Capacity check is intentionally `text_len + 1 >= sizeof`, not
    // `text_len + 1 > sizeof`. After the append we'll write to
    // text[text_len] (the new char) AND text[text_len + 1] (the
    // null), so we need (text_len + 1) < sizeof, i.e. abort when
    // (text_len + 1) >= sizeof. Looks like an off-by-one but is
    // correct -- mirror of the same pattern in widget_input.c.
    //
    if (n->text_len + 1 >= (int64)sizeof(n->text))
    {
        return;
    }
    n->text[n->text_len++] = (char)codepoint;
    n->text[n->text_len]   = 0;
    scene__dispatch_change(n, (float)n->text_len);
}

static void textarea_on_key(gui_node* n, int64 vk, boole down)
{
    if (!down) { return; }
    if (vk == 0x08) // VK_BACK
    {
        if (n->text_len > 0)
        {
            n->text_len--;
            n->text[n->text_len] = 0;
            scene__dispatch_change(n, (float)n->text_len);
        }
    }
    else if (vk == 0x0D) // VK_RETURN
    {
        if (n->text_len + 1 < (int64)sizeof(n->text))
        {
            n->text[n->text_len++] = '\n';
            n->text[n->text_len]   = 0;
            scene__dispatch_change(n, (float)n->text_len);
        }
    }
}

//
// Same `placeholder=` alias as widget_input. Keeps the two text-
// input widgets grammar-compatible so authors can switch
// between <input> and <textarea> without renaming attributes.
//
static boole textarea_apply_attribute(gui_node* n, char* name, char* value)
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

static const widget_vtable g_textarea_vtable = {
    .type_name       = "textarea",
    .apply_attribute = textarea_apply_attribute,
    .layout          = textarea_layout,
    .emit_draws      = textarea_emit_draws,
    .on_char         = textarea_on_char,
    .on_key          = textarea_on_key,
    .takes_focus     = TRUE,
};

void widget_textarea__register(void)
{
    widget_registry__register(GUI_NODE_TEXTAREA, &g_textarea_vtable);
}
