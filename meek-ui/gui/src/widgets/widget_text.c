//
//widget_text.c - static label (<label> in .ui, formerly <Text>).
//
//auto-sizes to its text's measured width + the font's line height,
//unless style specifies explicit size_w / size_h. honors the
//avail_w passed in by the parent's layout and WORD-WRAPS when the
//text's natural width would exceed that bound.
//
//  <label text="Hello world" />
//
//layout pass:
//  1. pick the working font_size + family out of resolved style.
//  2. if size_w is pinned in style, use it as the wrap width AND as
//     the final bounds.w. otherwise wrap against avail_w (or don't
//     wrap at all when avail_w is 0 / the parent is happy with a
//     wide single-line child).
//  3. walk the text greedily: accumulate xadvance until a space
//     pushes us past the wrap width, then break at the last space.
//     count lines, track max line width.
//  4. bounds.w = wrap_width (or max line width if no wrap needed)
//     bounds.h = lines * line_height (or pinned size_h).
//
//draw pass:
//  repeat the same walk, calling font__draw_n per line with the
//  line's substring + the correct y baseline. pen_y advances by
//  line_height between lines.
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
//small guards used while walking the text. 256 matches the fixed
//capacity of gui_node.text[], so a well-formed node can't produce a
//line count bigger than this. we cap at a defensive value anyway in
//case callers stuff more bytes into text by bypassing the parser.
//
#define _WIDGET_TEXT_INTERNAL__MAX_LINES 64

//
//decide whether a byte is a word separator for wrap purposes. CSS
//treats runs of whitespace as a single break point and collapses
//them on render; we do the same by accepting the end-of-line offset
//and then skipping trailing spaces before the next line starts.
//also counts newlines so a literal '\n' in text forces a break
//regardless of wrap width -- useful if user uses &#10; in XML text
//or pastes a multi-line string in programmatically.
//
static boole _widget_text_internal__is_break(char c)
{
    return (boole)(c == ' ' || c == '\t' || c == '\n');
}

//
//scan forward from `start` and return the byte offset at which the
//CURRENT line should end. greedy: fits as many characters as we can
//before the cumulative xadvance would exceed wrap_w, then breaks at
//the last break-character seen. if no break-character fits before
//the overflow, we return the overflow point itself (the single long
//word is allowed to overhang its own line -- matches CSS default
//overflow-wrap: normal). a forced break character ('\n') snaps us
//early regardless of width.
//
static int64 _widget_text_internal__find_line_end(gui_font* f,
                                                   const char* text,
                                                   int64        text_len,
                                                   int64        start,
                                                   float        wrap_w)
{
    if (start >= text_len)
    {
        return start;
    }
    //
    //wrap_w <= 0 means "no wrap budget" -- caller wants a single line.
    //just scan for a literal '\n' or the end of the string.
    //
    if (wrap_w <= 0.0f)
    {
        int64 i = start;
        while (i < text_len && text[i] != '\n')
        {
            i++;
        }
        return i;
    }

    int64 last_break = -1;
    float x          = 0.0f;
    int64 i          = start;
    while (i < text_len)
    {
        char c = text[i];
        if (c == '\n')
        {
            //
            //forced newline -- break here immediately.
            //
            return i;
        }
        if (_widget_text_internal__is_break(c))
        {
            //
            //remember this as a candidate wrap point. we still advance
            //past the space so a subsequent "width exceeded" check
            //counts the space toward the line budget.
            //
            last_break = i;
        }

        unsigned char code = (unsigned char)c;
        float advance = font__measure_n(f, (char*)&text[i], 1);
        (void)code;
        x += advance;

        if (x > wrap_w && last_break >= 0)
        {
            //
            //overflow and we have somewhere to break. split there.
            //
            return last_break;
        }
        i++;
    }
    //
    //entire remaining tail fits. return the end-of-string offset.
    //
    return i;
}

//
//skip any trailing break characters after a line's end so the next
//line doesn't start with the space that was consumed by the break.
//
static int64 _widget_text_internal__skip_breaks(const char* text, int64 text_len, int64 i)
{
    while (i < text_len && _widget_text_internal__is_break(text[i]) && text[i] != '\n')
    {
        i++;
    }
    if (i < text_len && text[i] == '\n')
    {
        i++;
    }
    return i;
}

static void text_layout(gui_node* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)avail_h;
    gui_style* s = &n->resolved;

    //
    //resolved font at scaled size. font_size 0 falls back to a 14 px
    //default (same logic as before the wrap rewrite).
    //
    float     size_px = (s->font_size > 0.0f ? s->font_size : 14.0f) * scale;
    gui_font* f       = font__at(s->font_family, size_px);

    //
    //if there's no font registered yet (e.g. Android: fonts stubbed
    //out), fall back to a pure sizing approximation so layout can
    //still make progress. uses the scaled font_size for height and
    //zero for width.
    //
    if (f == NULL)
    {
        n->bounds.x = x;
        n->bounds.y = y;
        n->bounds.w = (s->size_w > 0.0f) ? s->size_w * scale : 0.0f;
        n->bounds.h = (s->size_h > 0.0f) ? s->size_h * scale : size_px;
        return;
    }

    float line_h = font__line_height(f);

    //
    //figure out wrap width. pin beats avail_w beats "no wrap".
    //when neither is set (size_w = 0 AND avail_w = 0) we do a single
    //line and report the measured width -- matches pre-wrap behavior.
    //
    float wrap_w = 0.0f;
    if (s->size_w > 0.0f)
    {
        wrap_w = s->size_w * scale;
    }
    else if (avail_w > 0.0f)
    {
        wrap_w = avail_w;
    }

    //
    //walk the string once to compute line count + max line width.
    //identical walk repeats in text_emit_draws to render each line;
    //for the text sizes we handle (<= 256 bytes) the duplication is
    //trivial and beats carrying scratch state on gui_node.
    //
    int64 text_len = 0;
    while (n->text[text_len] != 0 && text_len < (int64)sizeof(n->text))
    {
        text_len++;
    }

    int64 lines      = 0;
    float max_line_w = 0.0f;
    int64 cursor     = 0;
    while (cursor < text_len && lines < _WIDGET_TEXT_INTERNAL__MAX_LINES)
    {
        int64 end = _widget_text_internal__find_line_end(f, n->text, text_len, cursor, wrap_w);
        if (end <= cursor)
        {
            //
            //find_line_end refused to advance (shouldn't happen with
            //valid inputs, but guard so a bad text blob doesn't loop
            //forever). force one character of progress.
            //
            end = cursor + 1;
        }
        float line_w = font__measure_n(f, &n->text[cursor], end - cursor);
        if (line_w > max_line_w)
        {
            max_line_w = line_w;
        }
        lines++;
        cursor = _widget_text_internal__skip_breaks(n->text, text_len, end);
    }
    if (lines == 0)
    {
        //
        //empty text still needs a baseline height so callers that put
        //a <label text="" /> in a column don't produce a 0-gap.
        //use size_h if pinned else the single-line height.
        //
        lines = 1;
    }

    //
    //final sizing. size_w pinned wins; else max_line_w bounded by
    //wrap_w (we never return a bounds.w larger than the parent told
    //us we could have, to keep scissor / scroll math honest).
    //
    float w;
    if (s->size_w > 0.0f)
    {
        w = s->size_w * scale;
    }
    else if (wrap_w > 0.0f && max_line_w > wrap_w)
    {
        w = wrap_w;
    }
    else
    {
        w = max_line_w;
    }

    float h = (s->size_h > 0.0f) ? s->size_h * scale : (float)lines * line_h;

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static void text_emit_draws(gui_node* n, float scale)
{
    //
    // bg + border via the shared helper so text labels can carry
    // gradient / shadow / bg-image / blur / border-gradient the
    // same way containers do. No effect when no bg style is set.
    //
    scene__emit_default_bg(n, scale);

    if (n->text[0] == 0)
    {
        return;
    }

    float     size_px = (n->resolved.font_size > 0.0f ? n->resolved.font_size : 14.0f) * scale;
    gui_font* f       = font__at(n->resolved.font_family, size_px);
    if (f == NULL)
    {
        return;
    }

    float ascent = font__ascent(f);
    float line_h = font__line_height(f);

    //
    //re-derive wrap width (must match what text_layout used).
    //
    float wrap_w = 0.0f;
    if (n->resolved.size_w > 0.0f)
    {
        wrap_w = n->resolved.size_w * scale;
    }
    else
    {
        wrap_w = n->bounds.w; // layout already bounded this
    }

    //
    //measure text_len the same way text_layout did (stops at the
    //first null or the 256-byte cap).
    //
    int64 text_len = 0;
    while (n->text[text_len] != 0 && text_len < (int64)sizeof(n->text))
    {
        text_len++;
    }

    gui_color color = n->resolved.has_font_color ? n->resolved.font_color : scene__rgb(1.0f, 1.0f, 1.0f);

    //
    //walk lines, same algorithm as layout. emit each line's substring
    //at (bounds.x, bounds.y + ascent + line_index * line_h). pen y is
    //the baseline -- font__draw_n uses y as baseline, so we add
    //ascent to bounds.y for the first line and line_h for each
    //successive one.
    //
    int64 cursor     = 0;
    int64 line_index = 0;
    while (cursor < text_len && line_index < _WIDGET_TEXT_INTERNAL__MAX_LINES)
    {
        int64 end = _widget_text_internal__find_line_end(f, n->text, text_len, cursor, wrap_w);
        if (end <= cursor)
        {
            end = cursor + 1;
        }
        float tx = n->bounds.x;
        float ty = n->bounds.y + ascent + (float)line_index * line_h;
        font__draw_n(f, tx, ty, color, &n->text[cursor], end - cursor);

        line_index++;
        cursor = _widget_text_internal__skip_breaks(n->text, text_len, end);
    }
}

static const widget_vtable g_text_vtable = {
    .type_name       = "label",
    .layout          = text_layout,
    .emit_draws      = text_emit_draws,
};

void widget_text__register(void)
{
    widget_registry__register(GUI_NODE_TEXT, &g_text_vtable);
}
