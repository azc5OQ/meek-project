//
// ===========================================================================
// parser_style.c - CSS-shaped parser for the toolkit's .style files.
// ===========================================================================
//
// WHAT THIS PARSER IS
// -------------------
// A hand-written recursive-descent parser for the CSS-like syntax described
// in session/DESIGN.md. Input is a text file; output is a sequence of
// scene__register_style(selector, &style) calls -- one per rule found.
// scene.c's selector table then applies those rules to every matching
// node during scene__resolve_styles.
//
// GRAMMAR (the subset we currently accept):
//   stylesheet  := rule*
//   rule        := selector '{' block '}'
//   block       := ( decl | nested_rule )*
//   nested_rule := ':' ident '{' block '}'       // pseudo-state nesting
//   decl        := ident ':' value ';'
//   value       := color | number | size_2d | keyword
//   color       := '#' hex{6}
//   number      := [0-9]+ ('.' [0-9]+)?
//   size_2d     := number 'x' number             // e.g. 200x40
//   keyword     := ident                         // e.g. auto, scroll
//
// Example .style file:
//   Window       { bg: #1e1e1e; font_family: Roboto-Regular; font_size: 14; }
//   #main        { pad: 24; gap: 8; }
//   Button       { bg: #333; radius: 6; pad: 8; :hover { bg: #555; } }
//   Button.primary { bg: #0066cc; }
//
// WHY CSS-SHAPED (not real CSS)
// -----------------------------
// Developers already know "selector { property: value; }". Reusing the
// shape costs zero novelty budget. But real CSS drags in the cascade,
// specificity algebra, the unit zoo, media queries, keyframe syntax,
// and a dozen other features that we don't need for a retained-mode
// toolkit. We kept the surface syntax and cut everything heavy:
//   - NO `!important`, no `inherit`/`initial`/`unset` keywords.
//   - NO media queries, @keyframes, @font-face.
//   - NO cascade or specificity beyond flat precedence in scene.c
//     (id beats class beats type; within a tier, later rule wins).
//   - NO universal selector (`*`) or combinators (space/`>`/`+`).
//   - ONE unit: pixel. Size/padding/gap are plain numbers.
//
// PROPERTIES SUPPORTED (list lives here; add to match gui_style):
//   bg                <color>         background color
//   fg                <color>         foreground / accent (slider thumb etc.)
//   color             <color>         text color
//   font_family       <ident|string>  font name (matches .ttf filename)
//   font_size         <number>        point size
//   radius            <number>        corner radius
//   pad               <number>        padding (all four sides)
//   gap               <number>        spacing between children
//   size              <w>x<h>         explicit size (0 = auto on either axis)
//   overflow          <keyword>       visible|hidden|scroll|auto (both axes)
//   overflow_x        <keyword>       same, horizontal axis only
//   overflow_y        <keyword>       same, vertical axis only
//   scrollbar_size    <number>        scrollbar thickness in px
//   scrollbar_radius  <number>        scrollbar corner radius
//   scrollbar_track   <color>         scrollbar channel color
//   scrollbar_thumb   <color>         scrollbar draggable thumb color
//
// ERROR STRATEGY
// --------------
// Same as parser_xml.c: a latched `ok` flag on the lexer. First error
// logs "filename:line: message" and sets ok = FALSE; helpers short
// circuit. At end of parse, ok is the return value of load_styles().
// Unlike parser_xml, we DON'T blow away the partial style set on
// error -- any rules successfully registered before the error are
// already committed. This is fine for hot reload because the live app
// does a scene__clear_styles() before each reparse.
//
// I/O
// ---
// fs__read_entire_file slurps the file. parser_style never sees a
// FILE*. This means the same parser happily runs against memory-mapped
// files, embedded byte arrays, or test fixtures.
//

#include <stdio.h>  // snprintf for nested-pseudo selector composition.
#include <stdlib.h> // atof -- still libc; stdlib__atof not yet provided.
//
// NOTE: <string.h> deliberately NOT included. strcmp / memcpy / memset
// have all been migrated to the project-local `stdlib__` wrappers in
// clib/stdlib.h below. If you re-introduce a libc string call here,
// migrate it to stdlib__* in the same patch.
//

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "fs.h"
#include "parser_style.h"
#include "clib/memory_manager.h"
#include "clib/stdlib.h"
#include "third_party/log.h"

/**
 * Lexer state for one parse run.
 *
 * Mirrors parser_xml.c's lexer -- same fields, same conventions. One of
 * these is built on the stack at the top of parser_style__load_styles
 * and threaded through every helper.
 */
typedef struct _parser_style_internal__lexer
{
    char* src;      // input buffer (null-terminated copy of the file).
    int64 len;      // length in bytes (excluding the null terminator).
    int64 pos;      // current parse position (0..len).
    int64 line;     // current line number for error messages (1-based).
    char* filename; // path echoed in error messages; not owned.
    boole ok;       // latched FALSE once any error has been reported.
} _parser_style_internal__lexer;

//
// ===========================================================================
// Forward declarations.
// ===========================================================================
//
// Every helper is file-local. Public API is just parser_style__load_styles.
//
static void  _parser_style_internal__error(_parser_style_internal__lexer* lx, char* msg);
static char  _parser_style_internal__peek(_parser_style_internal__lexer* lx);
static char  _parser_style_internal__advance(_parser_style_internal__lexer* lx);
static boole _parser_style_internal__eof(_parser_style_internal__lexer* lx);
static void  _parser_style_internal__skip_ws_and_comments(_parser_style_internal__lexer* lx);
static boole _parser_style_internal__is_alpha(char c);
static boole _parser_style_internal__is_alnum(char c);
static boole _parser_style_internal__is_digit(char c);
static boole _parser_style_internal__parse_ident(_parser_style_internal__lexer* lx, char* out, int64 cap);
static boole _parser_style_internal__parse_selector(_parser_style_internal__lexer* lx, char* out, int64 cap);
static boole _parser_style_internal__parse_color(_parser_style_internal__lexer* lx, gui_color* out);
static boole _parser_style_internal__parse_number(_parser_style_internal__lexer* lx, float* out);
static boole _parser_style_internal__parse_size(_parser_style_internal__lexer* lx, float* out_w, float* out_h);
static boole _parser_style_internal__parse_easing(_parser_style_internal__lexer* lx, gui_easing* out_kind, float out_params[4]);
static boole _parser_style_internal__parse_decl(_parser_style_internal__lexer* lx, gui_style* style);
static void  _parser_style_internal__parse_block(_parser_style_internal__lexer* lx, char* base_selector);
static void  _parser_style_internal__parse_rule(_parser_style_internal__lexer* lx);

//
// ===========================================================================
// Public API.
// ===========================================================================
//

//
// The only entry point. Reads the file, parses every rule, and calls
// scene__register_style for each. Returns TRUE on a clean parse,
// FALSE on I/O failure or any parse error.
//
boole parser_style__load_styles(char* path)
{
    //
    // Step 1: read the file via the fs abstraction. Same interface
    // parser_xml uses; any file that opens+reads successfully comes
    // back as a heap buffer with a null terminator at buf[size].
    //
    int64 size = 0;
    char* buf  = fs__read_entire_file(path, &size);
    if (buf == NULL)
    {
        log_error("failed to read '%s'", path);
        return FALSE;
    }

    //
    // Step 2: initialize the lexer.
    //
    _parser_style_internal__lexer lx;
    lx.src      = buf;
    lx.len      = size;
    lx.pos      = 0;
    lx.line     = 1;
    lx.filename = path;
    lx.ok       = TRUE;

    //
    // Step 3: top-level loop. Skip leading whitespace/comments, then
    // parse one rule. Loop until EOF or an error latches ok=FALSE.
    //
    // Note: we don't bail on the first bad rule. parse_rule skips what
    // it can; sometimes an error in one declaration leaves us at a
    // recoverable position, and the rest of the file still registers.
    // But: the first error flips ok=FALSE, so downstream parse helpers
    // become no-ops. In practice one error = "stop parsing".
    //
    while (lx.ok)
    {
        _parser_style_internal__skip_ws_and_comments(&lx);
        if (_parser_style_internal__eof(&lx))
        {
            break;
        }
        _parser_style_internal__parse_rule(&lx);
    }

    GUI_FREE(buf);
    return lx.ok;
}

//
// ===========================================================================
// Lexer primitives -- character-at-a-time helpers.
// ===========================================================================
//

//
// Log "filename:line: message" and latch ok=FALSE. Every downstream
// helper bails when ok is FALSE (through a boolean return).
//
static void _parser_style_internal__error(_parser_style_internal__lexer* lx, char* msg)
{
    log_error("%s:%lld: %s", lx->filename, (long long)lx->line, msg);
    lx->ok = FALSE;
}

//
// Look at the current character without consuming. Returns 0 at EOF
// so callers can treat EOF and NUL uniformly.
//
static char _parser_style_internal__peek(_parser_style_internal__lexer* lx)
{
    if (lx->pos >= lx->len)
    {
        return 0;
    }
    return lx->src[lx->pos];
}

//
// Consume one character, return it, bump line counter on '\n'.
//
static char _parser_style_internal__advance(_parser_style_internal__lexer* lx)
{
    if (lx->pos >= lx->len)
    {
        return 0;
    }
    char c = lx->src[lx->pos++];
    if (c == '\n')
    {
        lx->line++;
    }
    return c;
}

//
// EOF test. Used as a loop guard in every scanning function.
//
static boole _parser_style_internal__eof(_parser_style_internal__lexer* lx)
{
    return (boole)(lx->pos >= lx->len);
}

//
// Skip ASCII whitespace and C-style /* ... */ comments. CSS uses
// the same /* */ comment syntax, so the same scanner works here.
// We do NOT accept // line comments -- real CSS doesn't either.
//
// The outer for(;;) loop handles the case where a comment is
// immediately followed by more whitespace, which could be followed
// by another comment, etc. We keep consuming until we hit a non-
// whitespace, non-comment character.
//
static void _parser_style_internal__skip_ws_and_comments(_parser_style_internal__lexer* lx)
{
    for (;;)
    {
        //
        // Inner loop: consume plain whitespace.
        //
        while (!_parser_style_internal__eof(lx))
        {
            char c = _parser_style_internal__peek(lx);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                _parser_style_internal__advance(lx);
            }
            else
            {
                break;
            }
        }

        //
        // Is there a "/*" here? If so, consume through "*/" and loop
        // back to the whitespace scanner. If not, we're done.
        //
        if (lx->pos + 1 < lx->len && lx->src[lx->pos] == '/' && lx->src[lx->pos + 1] == '*')
        {
            lx->pos += 2;
            boole closed = FALSE;
            while (lx->pos + 1 < lx->len)
            {
                if (lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/')
                {
                    lx->pos += 2;
                    closed = TRUE;
                    break;
                }
                _parser_style_internal__advance(lx);
            }
            if (!closed)
            {
                //
                // Hit EOF without finding `*/`. Without this branch
                // the .style file would silently load with everything
                // after the unclosed comment dropped on the floor and
                // no warning to the dev.
                //
                _parser_style_internal__error(lx, "unterminated /* */ comment");
                return;
            }
            continue;
        }
        break;
    }
}

//
// Character classes. We don't pull in <ctype.h> because:
//   - <ctype.h> is locale-aware, which slows things down and makes
//     the build non-portable between musl/glibc/MSVCRT.
//   - We only need ASCII anyway (selectors and property names are
//     chosen by the engine, not by users).
//
static boole _parser_style_internal__is_alpha(char c)
{
    return (boole)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_');
}
static boole _parser_style_internal__is_alnum(char c)
{
    //
    // Note '-' is included here -- CSS identifiers allow hyphens
    // (e.g. `font-family`). Our property names actually use underscores
    // (`font_family`), but we accept hyphens too so that a future
    // conversion to CSS-canonical names doesn't break existing files.
    //
    return (boole)(_parser_style_internal__is_alpha(c) || (c >= '0' && c <= '9') || c == '-');
}
static boole _parser_style_internal__is_digit(char c)
{
    return (boole)(c >= '0' && c <= '9');
}

//
// Read a plain identifier (first char alpha/'_', rest alnum/'_'/'-').
// Used for property names, keyword values (auto/scroll/visible/...),
// and pseudo-state names (hover/pressed/disabled/default).
//
// Returns FALSE on empty input or overflow, logs the error, latches ok.
//
static boole _parser_style_internal__parse_ident(_parser_style_internal__lexer* lx, char* out, int64 cap)
{
    int64 n = 0;
    boole first = TRUE;
    while (!_parser_style_internal__eof(lx))
    {
        char c = _parser_style_internal__peek(lx);
        if (first)
        {
            if (!_parser_style_internal__is_alpha(c))
            {
                break;
            }
            first = FALSE;
        }
        else if (!_parser_style_internal__is_alnum(c))
        {
            break;
        }
        if (n + 1 >= cap)
        {
            _parser_style_internal__error(lx, "identifier too long");
            return FALSE;
        }
        //
        //normalize `_` to `-` when building the ident. property-name
        //dispatch below uses the CSS-canonical kebab-case spellings
        //(font-family, scroll-smooth, ease-in-out, ...). existing
        //.style files written with snake_case (font_family, ...)
        //still work because the normalization happens before strcmp.
        //affects only IDENTIFIERS read via this function -- property
        //names and keyword values (easing names, visibility / display
        //keywords). font-family VALUES (Roboto-Regular, etc.) are
        //read via a separate bespoke value parser and are unaffected.
        //
        char stored = (c == '_') ? '-' : c;
        out[n++] = stored;
        _parser_style_internal__advance(lx);
    }
    if (n == 0)
    {
        _parser_style_internal__error(lx, "expected identifier");
        return FALSE;
    }
    out[n] = 0;
    return TRUE;
}

//
// Read a selector token: everything up to the opening '{' or a
// selector-boundary character. Letters, digits, '.', '#', '-', '_',
// ':' are all treated as part of the selector, so "Button.primary",
// "#main", "Button:hover" each come back as one string. scene.c's
// _scene_internal__parse_selector then splits the pieces apart and
// decides how to match.
//
// We DON'T accept spaces inside a selector (no descendant combinator)
// or commas (no selector lists). Those would complicate specificity
// in ways we've deliberately opted out of.
//
static boole _parser_style_internal__parse_selector(_parser_style_internal__lexer* lx, char* out, int64 cap)
{
    int64 n = 0;
    while (!_parser_style_internal__eof(lx))
    {
        char c = _parser_style_internal__peek(lx);
        if (c == '{' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == 0)
        {
            break;
        }
        if (n + 1 >= cap)
        {
            _parser_style_internal__error(lx, "selector too long");
            return FALSE;
        }
        out[n++] = c;
        _parser_style_internal__advance(lx);
    }
    if (n == 0)
    {
        _parser_style_internal__error(lx, "expected selector");
        return FALSE;
    }
    out[n] = 0;
    return TRUE;
}

//
// Parse "#rrggbb" or "#rrggbbaa" into a gui_color. Six digits gives an
// opaque color (alpha = 1.0). Eight digits adds an explicit alpha
// channel in the trailing two hex digits (00 = fully transparent,
// ff = opaque). Useful for animation starting values like
// `bg: #6366f100;` to mean "this color but invisible to begin with".
//
// Three-digit shortcut (#rgb) is not accepted -- it's ambiguous when
// the next char is itself a hex digit and we haven't seen any real
// .style file want it.
//
// Accepts both cases: #ff00aa and #FF00AA both work.
//
static boole _parser_style_internal__parse_color(_parser_style_internal__lexer* lx, gui_color* out)
{
    //
    // Accept the `transparent` keyword as a synonym for #00000000.
    // Every other CSS-like named color is intentionally not
    // supported (hex-only palette by design), but `transparent` is
    // so load-bearing in practice -- status bars, overlays, nav
    // buttons with hover-only backgrounds -- that requiring users
    // to remember `#00000000` is gratuitous. Keep this as the ONLY
    // named-color alias.
    //
    if (_parser_style_internal__peek(lx) == 't')
    {
        char ident[16];
        int64 saved_pos = lx->pos;
        int64 saved_ln  = lx->line;
        if (_parser_style_internal__parse_ident(lx, ident, sizeof(ident)) &&
            stdlib__strcmp(ident, "transparent") == 0)
        {
            out->r = 0; out->g = 0; out->b = 0; out->a = 0;
            return TRUE;
        }
        // Not `transparent`: rewind and fall through to the hex path
        // so the normal error message fires if this wasn't a color
        // keyword after all.
        lx->pos  = saved_pos;
        lx->line = saved_ln;
    }

    if (_parser_style_internal__peek(lx) != '#')
    {
        _parser_style_internal__error(lx, "expected '#' for color (or the 'transparent' keyword)");
        return FALSE;
    }
    _parser_style_internal__advance(lx);

    //
    // Accumulate up to 8 nibbles. Stop at the first non-hex character,
    // then verify the count is either 6 (rrggbb) or 8 (rrggbbaa).
    //
    unsigned int v = 0;
    int64 n = 0;
    while (n < 8)
    {
        char c = _parser_style_internal__peek(lx);
        int  d;
        if      (c >= '0' && c <= '9') { d = c - '0';      }
        else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
        else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
        else                            { break;            }
        v = (v << 4) | (unsigned int)d;
        _parser_style_internal__advance(lx);
        n++;
    }
    if (n != 6 && n != 8)
    {
        _parser_style_internal__error(lx, "expected hex color (#rrggbb or #rrggbbaa)");
        return FALSE;
    }
    if (n == 6)
    {
        //
        // scene__hex unpacks 0xrrggbb into four 0..1 floats with alpha = 1.
        //
        *out = scene__hex((uint)v);
    }
    else
    {
        //
        // Eight digits: the trailing pair is alpha. Mask + scale.
        //
        unsigned int rgb   = (v >> 8) & 0xFFFFFFu;
        unsigned int alpha = v & 0xFFu;
        gui_color    c     = scene__hex((uint)rgb);
        c.a = (float)alpha / 255.0f;
        *out = c;
    }
    return TRUE;
}

//
// Parse an unsigned decimal number into a float. Accepts "42",
// "3.14", "0.5". No leading '+' or '-', no exponent form (no "1e3").
// We could add those, but zero .style file needs them and keeping the
// grammar narrow shrinks the surface for bugs.
//
// Pipes through atof() because the digit parser here is just a copy
// loop -- we don't need to reimplement IEEE-754 from scratch for a
// style-file load.
//
static boole _parser_style_internal__parse_number(_parser_style_internal__lexer* lx, float* out)
{
    char  buf[32];
    int64 n = 0;
    boole seen_dot = FALSE;
    //
    // Optional leading '-'. Useful for box-shadow offsets (`box-shadow:
    // -3 -3 8 #000`) and any future property that meaningfully
    // accepts a negative value. A bare '-' with no digits after it
    // gets caught by the n == 0 check below.
    //
    if (_parser_style_internal__peek(lx) == '-')
    {
        buf[n++] = '-';
        _parser_style_internal__advance(lx);
    }
    while (!_parser_style_internal__eof(lx))
    {
        char c = _parser_style_internal__peek(lx);
        if (c == '.')
        {
            //
            // Reject a second '.' rather than silently letting atof
            // truncate at the first one (which would parse "3.14.15"
            // as 3.14 with no warning to the dev).
            //
            if (seen_dot)
            {
                _parser_style_internal__error(lx, "number has multiple decimal points");
                return FALSE;
            }
            seen_dot = TRUE;
        }
        else if (!_parser_style_internal__is_digit(c))
        {
            break;
        }
        if (n + 1 >= (int64)sizeof(buf))
        {
            _parser_style_internal__error(lx, "number too long");
            return FALSE;
        }
        buf[n++] = c;
        _parser_style_internal__advance(lx);
    }
    if (n == 0)
    {
        _parser_style_internal__error(lx, "expected number");
        return FALSE;
    }
    //
    // A lone "-" passes the n == 0 guard but is not a number.
    // Catch it so atof("-") doesn't silently produce 0.0.
    //
    if (n == 1 && buf[0] == '-')
    {
        _parser_style_internal__error(lx, "'-' with no digits is not a number");
        return FALSE;
    }
    buf[n] = 0;
    *out = (float)atof(buf);
    //
    // Optional trailing `px` suffix. CSS muscle memory writes
    // `font-size: 14px;` / `radius: 18px;` / `gap: 12px;` even
    // though px is the only unit our layout speaks. Accepting +
    // discarding the suffix keeps both forms parsing equivalently
    // so a single .style file can mix bare numbers and `Npx`
    // without surprise. Note this lives BELOW atof so the digits
    // are already in `buf`; the px just gets consumed off the
    // input stream.
    //
    if (_parser_style_internal__peek(lx) == 'p')
    {
        _parser_style_internal__advance(lx);
        if (_parser_style_internal__peek(lx) == 'x')
        {
            _parser_style_internal__advance(lx);
        }
        else
        {
            //
            // Lone 'p' that isn't 'px' is malformed -- the spec
            // doesn't define any other p-prefixed unit. Roll back
            // is annoying without a lexer mark, but failing loud
            // here means the dev sees the typo right away rather
            // than getting silently truncated to the bare number.
            //
            _parser_style_internal__error(lx, "expected 'px' suffix or no unit after number");
            return FALSE;
        }
    }
    return TRUE;
}

//
// Parse "<w>x<h>": e.g. "200x40", "0x120". Used by `size` and any
// future two-axis sized property. We deliberately don't accept
// whitespace around the 'x' -- "200 x 40" is a parse error. This
// keeps the lexer simple and makes the syntax read like coordinates
// ("100x200 pixels").
//
// Either axis may be 0, meaning "auto" in the layout engine.
//
static boole _parser_style_internal__parse_size(_parser_style_internal__lexer* lx, float* out_w, float* out_h)
{
    if (!_parser_style_internal__parse_number(lx, out_w))
    {
        return FALSE;
    }
    char c = _parser_style_internal__peek(lx);
    if (c != 'x' && c != 'X')
    {
        _parser_style_internal__error(lx, "expected 'x' between width and height");
        return FALSE;
    }
    _parser_style_internal__advance(lx);
    if (!_parser_style_internal__parse_number(lx, out_h))
    {
        return FALSE;
    }
    return TRUE;
}

//
// Parse an easing specifier. Writes the enum kind to *out_kind and (for
// parameterised kinds) populates out_params. out_params must point to an
// array of 4 floats; untouched slots are zeroed for deterministic state.
//
// Accepted forms:
//   linear | ease-in | ease-out | ease-in-out
//   ease | ease-in-back | ease-out-back | ease-in-out-back | ease-out-bounce
//   cubic-bezier(x1, y1, x2, y2)
//   spring(stiffness, damping)
//
// The curve keyword is the same one CSS uses. `cubic-bezier` + `spring`
// require a parenthesised argument list; commas or whitespace separate
// the numbers, and the closing `)` is mandatory. Malformed args emit an
// error via the lexer's error mechanism and return FALSE.
//
static boole _parser_style_internal__parse_easing(_parser_style_internal__lexer* lx, gui_easing* out_kind, float out_params[4])
{
    out_params[0] = 0.0f;
    out_params[1] = 0.0f;
    out_params[2] = 0.0f;
    out_params[3] = 0.0f;

    char eas[24];
    if (!_parser_style_internal__parse_ident(lx, eas, (int64)sizeof(eas))) { return FALSE; }

    if      (stdlib__strcmp(eas, "linear")           == 0) { *out_kind = GUI_EASING_LINEAR;           return TRUE; }
    else if (stdlib__strcmp(eas, "ease-in")          == 0) { *out_kind = GUI_EASING_EASE_IN;          return TRUE; }
    else if (stdlib__strcmp(eas, "ease-out")         == 0) { *out_kind = GUI_EASING_EASE_OUT;         return TRUE; }
    else if (stdlib__strcmp(eas, "ease-in-out")      == 0) { *out_kind = GUI_EASING_EASE_IN_OUT;      return TRUE; }
    else if (stdlib__strcmp(eas, "ease")             == 0) { *out_kind = GUI_EASING_EASE;             return TRUE; }
    else if (stdlib__strcmp(eas, "ease-in-back")     == 0) { *out_kind = GUI_EASING_EASE_IN_BACK;     return TRUE; }
    else if (stdlib__strcmp(eas, "ease-out-back")    == 0) { *out_kind = GUI_EASING_EASE_OUT_BACK;    return TRUE; }
    else if (stdlib__strcmp(eas, "ease-in-out-back") == 0) { *out_kind = GUI_EASING_EASE_IN_OUT_BACK; return TRUE; }
    else if (stdlib__strcmp(eas, "ease-out-bounce")  == 0) { *out_kind = GUI_EASING_EASE_OUT_BOUNCE;  return TRUE; }
    else if (stdlib__strcmp(eas, "cubic-bezier")     == 0 || stdlib__strcmp(eas, "spring") == 0)
    {
        //
        // Parenthesised argument list. cubic-bezier takes 4 numbers,
        // spring takes 2. Numbers are comma- or whitespace-separated;
        // the parser eats either.
        //
        boole is_bezier = (stdlib__strcmp(eas, "cubic-bezier") == 0);
        int   want      = is_bezier ? 4 : 2;

        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__peek(lx) != '(')
        {
            _parser_style_internal__error(lx, is_bezier ? "cubic-bezier expects '(x1, y1, x2, y2)'" : "spring expects '(stiffness, damping)'");
            return FALSE;
        }
        _parser_style_internal__advance(lx);

        for (int i = 0; i < want; i++)
        {
            _parser_style_internal__skip_ws_and_comments(lx);
            if (!_parser_style_internal__parse_number(lx, &out_params[i])) { return FALSE; }
            _parser_style_internal__skip_ws_and_comments(lx);
            if (i < want - 1)
            {
                //
                // Optional comma between args. Accepting ws-only
                // keeps `cubic-bezier(0 0 1 1)` valid too, matching
                // the loose style of the rest of this parser.
                //
                if (_parser_style_internal__peek(lx) == ',') { _parser_style_internal__advance(lx); }
            }
        }
        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__peek(lx) != ')')
        {
            _parser_style_internal__error(lx, "easing: expected ')'");
            return FALSE;
        }
        _parser_style_internal__advance(lx);

        *out_kind = is_bezier ? GUI_EASING_CUBIC_BEZIER : GUI_EASING_SPRING;
        return TRUE;
    }

    _parser_style_internal__error(lx, "easing must be one of: linear, ease-in, ease-out, ease-in-out, ease, ease-in-back, ease-out-back, ease-in-out-back, ease-out-bounce, cubic-bezier(...), spring(...)");
    return FALSE;
}

//
// ===========================================================================
// Declaration parser -- the big property-name dispatch.
// ===========================================================================
//
// Parse exactly one "property: value;" declaration. The mapping from
// property NAMES to gui_style FIELDS is a chain of strcmp/else-if.
// A table-driven approach (static array of {name, parser_fn, offset})
// would be cleaner, but this form:
//   - Keeps each property's value-type visible at the call site,
//   - Makes adding a property a single-location edit, and
//   - Costs nothing meaningful at runtime (handful of strcmps per decl;
//     parse runs once per .style file on load + on hot reload).
//
// Unknown properties are WARNED about but not fatal: we skip to the
// next ';' and keep going. That way a typo doesn't wreck the whole
// file; the known-good properties in the rest of the block still apply.
//
static boole _parser_style_internal__parse_decl(_parser_style_internal__lexer* lx, gui_style* style)
{
    //
    // ----- property name ---------------------------------------------------
    //
    char prop[32];
    if (!_parser_style_internal__parse_ident(lx, prop, (int64)sizeof(prop)))
    {
        return FALSE;
    }

    //
    // ----- mandatory ':' ---------------------------------------------------
    // CSS requires ':' between name and value. We enforce it strictly.
    //
    _parser_style_internal__skip_ws_and_comments(lx);
    if (_parser_style_internal__peek(lx) != ':')
    {
        _parser_style_internal__error(lx, "expected ':' after property name");
        return FALSE;
    }
    _parser_style_internal__advance(lx);
    _parser_style_internal__skip_ws_and_comments(lx);

    //
    // ----- value (dispatch on property name) ------------------------------
    //
    // Each branch:
    //   1. Parses a value of the right shape (color, number, size, ident).
    //   2. Writes it into the matching gui_style field, setting any
    //      has_* flag so the scene's overlay logic can tell "set" from
    //      "default".
    //
    boole known = TRUE;
    //
    // Three color properties, one struct field each. Old short names
    // ("bg" / "fg" / "color") are accepted as aliases for the new
    // CSS-shaped names ("background-color" / "accent-color" /
    // "font-color") so existing .style files don't break -- but new
    // code should use the long names. The accent-color name comes
    // from CSS 4 where it controls slider thumbs / checkbox ticks /
    // etc., which is exactly what fg meant in this codebase.
    //
    if (stdlib__strcmp(prop, "background-color") == 0 || stdlib__strcmp(prop, "bg") == 0)
    {
        //
        // Background color. has_background_color differentiates "set
        // to black" from "not set" -- without the flag the resolver
        // couldn't tell whether to overlay.
        //
        gui_color c;
        if (!_parser_style_internal__parse_color(lx, &c))
        {
            return FALSE;
        }
        style->background_color     = c;
        style->has_background_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "accent-color") == 0 || stdlib__strcmp(prop, "fg") == 0)
    {
        //
        // Active widget part: slider thumb, checkbox tick, radio dot,
        // select chevron + selected-row highlight, image tint. Matches
        // CSS 4's `accent-color` property, which controls the same
        // set of native form-control highlights.
        //
        gui_color c;
        if (!_parser_style_internal__parse_color(lx, &c))
        {
            return FALSE;
        }
        style->accent_color     = c;
        style->has_accent_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "font-color") == 0 || stdlib__strcmp(prop, "color") == 0)
    {
        //
        // Text color. CSS calls this just `color`; we accept that as
        // an alias but prefer `font-color` because it's
        // unambiguously the text color (as opposed to bg, accent, or
        // any other "color" of a widget).
        //
        gui_color c;
        if (!_parser_style_internal__parse_color(lx, &c))
        {
            return FALSE;
        }
        style->font_color     = c;
        style->has_font_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "font-family") == 0)
    {
        //
        // Font family name. Accepts EITHER a quoted string
        // (font_family: "Inter";) OR a bare identifier
        // (font_family: Inter;). CSS normally requires quotes for
        // multi-word family names; we don't have multi-word names
        // right now (TTF filenames are the family names), but the
        // quote support is there for future-proofing.
        //
        // The value is everything up to ';' (with optional quotes
        // stripped), copied into style->font_family. Overflow is
        // silently truncated.
        //
        char buf[64];
        int64 n = 0;
        _parser_style_internal__skip_ws_and_comments(lx);

        //
        // Detect and consume an opening quote. We remember which
        // quote char it was so only the matching close ends the
        // value -- this lets a double-quoted value contain ' and
        // vice versa.
        //
        char quote = 0;
        if (_parser_style_internal__peek(lx) == '"' || _parser_style_internal__peek(lx) == '\'')
        {
            quote = _parser_style_internal__peek(lx);
            _parser_style_internal__advance(lx);
        }

        //
        // Read until end-of-value. For quoted values that's the
        // matching quote; for bare values that's ';' or whitespace.
        //
        while (!_parser_style_internal__eof(lx))
        {
            char c = _parser_style_internal__peek(lx);
            if (quote != 0 && c == quote)
            {
                _parser_style_internal__advance(lx); // consume closing quote.
                break;
            }
            if (quote == 0 && (c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n'))
            {
                break;
            }
            if (c == 0)
            {
                break;
            }
            if (n + 1 >= (int64)sizeof(buf))
            {
                break;
            }
            buf[n++] = c;
            _parser_style_internal__advance(lx);
        }
        buf[n] = 0;

        //
        // Copy into the node style field, truncating if necessary
        // (style->font_family is a fixed-size char array). Truncation
        // is logged so the dev knows their long font name didn't
        // round-trip cleanly -- silent truncation would just produce
        // a font lookup miss + fallback to the first registered
        // family with no obvious cause.
        //
        size_t copy_n = (size_t)n;
        if (copy_n >= sizeof(style->font_family))
        {
            log_warn("font-family '%s' truncated to %zu bytes (max name length)", buf, sizeof(style->font_family) - 1);
            copy_n = sizeof(style->font_family) - 1;
        }
        stdlib__memcpy(style->font_family, buf, copy_n);
        style->font_family[copy_n] = 0;
    }
    else if (stdlib__strcmp(prop, "font-size") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->font_size))
        {
            return FALSE;
        }
        style->font_size_explicit = TRUE;
    }
    else if (stdlib__strcmp(prop, "radius") == 0 || stdlib__strcmp(prop, "border-radius") == 0)
    {
        //
        // `radius` is our canonical name; `border-radius` is an
        // alias for CSS muscle memory. Both set the same field.
        // Alias added after the meek-shell D2 test run produced a
        // cascade of typos from contributors trained on CSS; see
        // session/memory/reference_style_property_names.md.
        //
        if (!_parser_style_internal__parse_number(lx, &style->radius))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "padding") == 0 || stdlib__strcmp(prop, "pad") == 0)
    {
        //
        // Shorthand: `padding: 12;` sets all four sides. Old name
        // `pad:` kept as alias for .style files predating the rename.
        // Per-side versions (padding-top etc.) are handled just
        // below; they OVERRIDE whatever this shorthand produced if
        // they appear later in the same rule block.
        //
        float v;
        if (!_parser_style_internal__parse_number(lx, &v))
        {
            return FALSE;
        }
        style->pad_t = v;
        style->pad_r = v;
        style->pad_b = v;
        style->pad_l = v;
    }
    else if (stdlib__strcmp(prop, "padding-top")    == 0 || stdlib__strcmp(prop, "pad-t") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->pad_t)) { return FALSE; }
    }
    else if (stdlib__strcmp(prop, "padding-right")  == 0 || stdlib__strcmp(prop, "pad-r") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->pad_r)) { return FALSE; }
    }
    else if (stdlib__strcmp(prop, "padding-bottom") == 0 || stdlib__strcmp(prop, "pad-b") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->pad_b)) { return FALSE; }
    }
    else if (stdlib__strcmp(prop, "padding-left")   == 0 || stdlib__strcmp(prop, "pad-l") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->pad_l)) { return FALSE; }
    }
    else if (stdlib__strcmp(prop, "margin") == 0)
    {
        //
        // CSS-aligned margin: space AROUND this element. Different
        // from gap, which is space BETWEEN siblings inside a flex/
        // grid container. Shorthand: single value sets all four
        // sides; per-side versions below override.
        //
        float v;
        if (!_parser_style_internal__parse_number(lx, &v))
        {
            return FALSE;
        }
        style->margin_t = v;
        style->margin_r = v;
        style->margin_b = v;
        style->margin_l = v;
    }
    else if (stdlib__strcmp(prop, "margin-top")    == 0) { if (!_parser_style_internal__parse_number(lx, &style->margin_t)) { return FALSE; } }
    else if (stdlib__strcmp(prop, "margin-right")  == 0) { if (!_parser_style_internal__parse_number(lx, &style->margin_r)) { return FALSE; } }
    else if (stdlib__strcmp(prop, "margin-bottom") == 0) { if (!_parser_style_internal__parse_number(lx, &style->margin_b)) { return FALSE; } }
    else if (stdlib__strcmp(prop, "margin-left")   == 0) { if (!_parser_style_internal__parse_number(lx, &style->margin_l)) { return FALSE; } }
    else if (stdlib__strcmp(prop, "gap") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->gap))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "size") == 0)
    {
        if (!_parser_style_internal__parse_size(lx, &style->size_w, &style->size_h))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "overflow") == 0 || stdlib__strcmp(prop, "overflow-x") == 0 || stdlib__strcmp(prop, "overflow-y") == 0)
    {
        //
        // `overflow` takes one of four keyword values:
        //   visible  default, no clip, no scrollbar
        //   hidden   clip to bounds (clipping deferred), no scrollbar
        //   scroll   always show scrollbar
        //   auto     show scrollbar only if content exceeds bounds
        //
        // The shorthand `overflow: <kw>;` writes BOTH axes; the
        // axis-specific forms write just one. This mirrors CSS.
        //
        // Widget-level: currently only <div> respects these fields.
        // See widget_div.c.
        //
        char kw[16];
        if (!_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)))
        {
            return FALSE;
        }
        gui_overflow mode;
        if      (stdlib__strcmp(kw, "visible") == 0) { mode = GUI_OVERFLOW_VISIBLE; }
        else if (stdlib__strcmp(kw, "hidden")  == 0) { mode = GUI_OVERFLOW_HIDDEN;  }
        else if (stdlib__strcmp(kw, "scroll")  == 0) { mode = GUI_OVERFLOW_SCROLL;  }
        else if (stdlib__strcmp(kw, "auto")    == 0) { mode = GUI_OVERFLOW_AUTO;    }
        else
        {
            _parser_style_internal__error(lx, "overflow expects one of: visible, hidden, scroll, auto");
            return FALSE;
        }
        if (stdlib__strcmp(prop, "overflow-x") == 0)
        {
            style->overflow_x = mode;
        }
        else if (stdlib__strcmp(prop, "overflow-y") == 0)
        {
            style->overflow_y = mode;
        }
        else
        {
            //
            // Shorthand: applies to both axes.
            //
            style->overflow_x = mode;
            style->overflow_y = mode;
        }
    }
    else if (stdlib__strcmp(prop, "scrollbar-size") == 0)
    {
        //
        // Thickness of the scrollbar in logical pixels. A vertical
        // scrollbar is scrollbar_size px wide; a horizontal one is
        // scrollbar_size px tall. 0 (the default) resolves to
        // widget_div's built-in 12px.
        //
        if (!_parser_style_internal__parse_number(lx, &style->scrollbar_size))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "scrollbar-radius") == 0)
    {
        //
        // Shared corner radius for track + thumb. 0 inherits the
        // widget's "reasonable default" (roughly half the bar size,
        // so the thumb reads as a pill).
        //
        if (!_parser_style_internal__parse_number(lx, &style->scrollbar_radius))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "scrollbar-track") == 0)
    {
        //
        // Track (channel) color. Typically a dim version of the
        // containing div's own bg.
        //
        gui_color c;
        if (!_parser_style_internal__parse_color(lx, &c))
        {
            return FALSE;
        }
        style->scrollbar_track     = c;
        style->has_scrollbar_track = TRUE;
    }
    else if (stdlib__strcmp(prop, "scrollbar-thumb") == 0)
    {
        //
        // Thumb color. Typically brighter than the track so the
        // draggable element reads clearly.
        //
        gui_color c;
        if (!_parser_style_internal__parse_color(lx, &c))
        {
            return FALSE;
        }
        style->scrollbar_thumb     = c;
        style->has_scrollbar_thumb = TRUE;
    }
    else if (stdlib__strcmp(prop, "appear") == 0)
    {
        //
        // appear: <duration_ms> [easing]
        //
        // Sets up the "fade in on first show" animation. Duration is a
        // plain number interpreted as milliseconds (CSS uses `ms` /
        // `s` suffixes; we don't bother -- everything is ms here).
        //
        // Easing is an optional second token: linear / ease_in /
        // ease_out / ease_in_out. Defaults to ease_out (the "pop"
        // curve). For separately specifying just the easing without
        // the duration, use the appear_easing property below.
        //
        // Examples:
        //   appear: 600;             -> 600 ms, ease_out
        //   appear: 250 ease_in;     -> 250 ms, ease_in
        //   appear: 800 linear;      -> 800 ms, linear
        //
        if (!_parser_style_internal__parse_number(lx, &style->appear_ms))
        {
            return FALSE;
        }
        //
        // Default easing is ease_out unless an explicit easing follows.
        // (Setting it here also means a subsequent `appear_easing` decl
        // can override.)
        //
        style->appear_easing = GUI_EASING_EASE_OUT;

        //
        // Look for an optional easing identifier after the number,
        // separated by whitespace. If the next non-whitespace char is
        // ';' there's no easing -- skip the parse.
        //
        _parser_style_internal__skip_ws_and_comments(lx);
        char nxt = _parser_style_internal__peek(lx);
        if (nxt != ';')
        {
            if (!_parser_style_internal__parse_easing(lx, &style->appear_easing, style->appear_easing_params))
            {
                return FALSE;
            }
        }
    }
    else if (stdlib__strcmp(prop, "appear-easing") == 0)
    {
        //
        // Standalone easing setter. Useful when the duration is
        // inherited from a parent rule but you want a different
        // easing on this node. No-op if appear_ms hasn't been set
        // anywhere up the chain (overlay only propagates easing
        // alongside a non-zero appear_ms).
        //
        if (!_parser_style_internal__parse_easing(lx, &style->appear_easing, style->appear_easing_params))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "animation") == 0)
    {
        //
        // animation: <duration_ms> [easing]
        //
        // Used INSIDE a per-state pseudo block (typically :appear, also
        // :disappear once that lands) to control how the animator moves
        // from this state's per-property values to the default-state
        // values. The pseudo-state's own :appear / :disappear name
        // already says WHEN the animation runs; this declaration only
        // controls duration + curve.
        //
        // Optional inside :appear -- if omitted, animator falls back to
        // sensible defaults (300 ms ease_out). So the minimum useful
        // appear block is just `:appear { bg: ...; }` with no other
        // metadata.
        //
        // Examples (all valid inside :appear { ... }):
        //   animation: 600;             -> 600 ms, ease_out
        //   animation: 250 ease_in;     -> 250 ms, ease_in
        //   animation: 800 linear;      -> 800 ms, linear
        //
        if (!_parser_style_internal__parse_number(lx, &style->animation_duration_ms))
        {
            return FALSE;
        }
        style->animation_easing = GUI_EASING_EASE_OUT; // default if no easing follows.

        _parser_style_internal__skip_ws_and_comments(lx);
        char nxt = _parser_style_internal__peek(lx);
        if (nxt != ';')
        {
            if (!_parser_style_internal__parse_easing(lx, &style->animation_easing, style->animation_easing_params))
            {
                return FALSE;
            }
        }
    }
    else if (stdlib__strcmp(prop, "transition") == 0)
    {
        //
        // transition: <prop> <duration_ms> [easing]
        //
        // Generic state-change interpolation. Unlike :appear / :disappear
        // which play a one-shot entrance/exit, `transition` smooths every
        // subsequent change to the named property so hover <-> default,
        // focus <-> default, etc. all animate. Distinct data on the style
        // struct (transition_duration_ms / transition_easing) so it
        // doesn't collide with the :appear declaration.
        //
        // <prop> is currently `all` only. Named-property filtering is a
        // future refinement; for now any animated property lerps when
        // duration > 0.
        //
        // Examples:
        //   transition: all 200;                      -> 200 ms ease-out
        //   transition: all 300 ease-in-out;
        //   transition: all 600 spring(18, 4);
        //
        char pname[16];
        if (!_parser_style_internal__parse_ident(lx, pname, (int64)sizeof(pname)))
        {
            return FALSE;
        }
        if (stdlib__strcmp(pname, "all") != 0)
        {
            //
            // Warn but accept -- we store the duration on the style
            // regardless so the author still gets motion, just on all
            // props instead of just the named one. Named filtering will
            // turn the warning into "only this prop animates".
            //
            _parser_style_internal__error(lx, "transition: named properties not yet supported; using 'all'");
            // Not a hard error; fall through and keep parsing.
        }

        _parser_style_internal__skip_ws_and_comments(lx);
        if (!_parser_style_internal__parse_number(lx, &style->transition_duration_ms))
        {
            return FALSE;
        }
        style->transition_easing = GUI_EASING_EASE_OUT; // default if no easing follows.

        _parser_style_internal__skip_ws_and_comments(lx);
        char nxt = _parser_style_internal__peek(lx);
        if (nxt != ';')
        {
            if (!_parser_style_internal__parse_easing(lx, &style->transition_easing, style->transition_easing_params))
            {
                return FALSE;
            }
        }
    }
    else if (stdlib__strcmp(prop, "visibility") == 0)
    {
        //
        // visibility: visible | hidden
        //
        // CSS-style per-node visibility. Hidden nodes still occupy
        // layout space (siblings don't reflow) but emit_draws skips
        // them. See gui_visibility in gui.h for the full behaviour
        // spec. Not inherited in this PoC: a hidden parent is the
        // only node that's hidden; children decide for themselves.
        // To hide a subtree, use display: none on the parent.
        //
        char kw[16];
        if (!_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)))
        {
            return FALSE;
        }
        if      (stdlib__strcmp(kw, "visible") == 0) { style->visibility = GUI_VISIBILITY_VISIBLE; }
        else if (stdlib__strcmp(kw, "hidden")  == 0) { style->visibility = GUI_VISIBILITY_HIDDEN;  }
        else
        {
            _parser_style_internal__error(lx, "visibility must be one of: visible, hidden");
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "display") == 0)
    {
        //
        // display: block | none
        //
        // CSS-style per-node display. `none` removes the node AND its
        // subtree from BOTH layout and rendering -- siblings collapse
        // into the space it would have taken. `block` is the default
        // "node participates normally" value. HTML's many other display
        // values (inline, flex, grid, ...) collapse into `block` in
        // this PoC because we only have column + row layout.
        //
        char kw[16];
        if (!_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)))
        {
            return FALSE;
        }
        if      (stdlib__strcmp(kw, "block") == 0) { style->display = GUI_DISPLAY_BLOCK; }
        else if (stdlib__strcmp(kw, "none")  == 0) { style->display = GUI_DISPLAY_NONE;  }
        else
        {
            _parser_style_internal__error(lx, "display must be one of: block, none");
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "position") == 0)
    {
        //
        // position: static | absolute
        //
        // Static (default) keeps the child in the parent's normal
        // column/row flow. Absolute lifts it out and anchors against
        // the parent's content rect via top/right/bottom/left.
        // CSS's `relative`, `fixed`, `sticky` are not implemented --
        // the use case driving this is "overlay one column over its
        // sibling" and absolute is sufficient.
        //
        char kw[16];
        if (!_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)))
        {
            return FALSE;
        }
        if      (stdlib__strcmp(kw, "static")   == 0) { style->position = GUI_POSITION_STATIC;   }
        else if (stdlib__strcmp(kw, "relative") == 0) { style->position = GUI_POSITION_RELATIVE; }
        else if (stdlib__strcmp(kw, "absolute") == 0) { style->position = GUI_POSITION_ABSOLUTE; }
        else if (stdlib__strcmp(kw, "fixed")    == 0) { style->position = GUI_POSITION_FIXED;    }
        else
        {
            _parser_style_internal__error(lx, "position must be one of: static, relative, absolute, fixed");
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "top")    == 0 ||
             stdlib__strcmp(prop, "right")  == 0 ||
             stdlib__strcmp(prop, "bottom") == 0 ||
             stdlib__strcmp(prop, "left")   == 0)
    {
        //
        // CSS insets for positioned children. Bare number = px;
        // `<n>%` = percent of parent's content w (left/right) or
        // h (top/bottom). Same px / pct dual-slot pattern as
        // width / height -- whichever slot is set wins; setting
        // px clears pct and vice versa.
        //
        // Ignored entirely when position != absolute; parsing the
        // value into the slot is fine in that case (no observable
        // effect at layout time).
        //
        float v = 0.0f;
        if (!_parser_style_internal__parse_number(lx, &v)) { return FALSE; }
        _parser_style_internal__skip_ws_and_comments(lx);

        char c = _parser_style_internal__peek(lx);
        boole is_percent = FALSE;
        if (c == '%')
        {
            is_percent = TRUE;
            _parser_style_internal__advance(lx);
        }
        else if (c == 'p')
        {
            _parser_style_internal__advance(lx);
            if (_parser_style_internal__peek(lx) == 'x')
            {
                _parser_style_internal__advance(lx);
            }
        }

        if      (stdlib__strcmp(prop, "top")    == 0)
        {
            if (is_percent) { style->inset_t_pct = v; style->inset_t = 0.0f; }
            else            { style->inset_t     = v; style->inset_t_pct = 0.0f; }
        }
        else if (stdlib__strcmp(prop, "right")  == 0)
        {
            if (is_percent) { style->inset_r_pct = v; style->inset_r = 0.0f; }
            else            { style->inset_r     = v; style->inset_r_pct = 0.0f; }
        }
        else if (stdlib__strcmp(prop, "bottom") == 0)
        {
            if (is_percent) { style->inset_b_pct = v; style->inset_b = 0.0f; }
            else            { style->inset_b     = v; style->inset_b_pct = 0.0f; }
        }
        else
        {
            if (is_percent) { style->inset_l_pct = v; style->inset_l = 0.0f; }
            else            { style->inset_l     = v; style->inset_l_pct = 0.0f; }
        }
    }
    else if (stdlib__strcmp(prop, "scroll-smooth") == 0)
    {
        //
        // scroll_smooth: <duration_ms>
        //
        // Opt-in smooth scrolling for any scrollable container. Wheel
        // input and thumb drag both set the node's scroll_y_target
        // (desired position); the animator then lerps scroll_y toward
        // target over roughly this many milliseconds (exponential
        // decay, so the tail asymptotes). 0 = instant (default).
        //
        if (!_parser_style_internal__parse_number(lx, &style->scroll_smooth_ms))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "scroll-fade") == 0)
    {
        //
        //scroll_fade: <pixels>
        //
        //soft edge-fade region inside a scrollable container. on the
        //container rule; applies to descendants. animator multiplies
        //each descendant's alpha by a 0..1 ramp based on how far its
        //bounds sit from the container's top + bottom edges. 0 (the
        //default) disables the effect entirely; ~30-60 px usually
        //reads well for body-text scrollers.
        //
        if (!_parser_style_internal__parse_number(lx, &style->scroll_fade_px))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "box-shadow") == 0)
    {
        //
        // box-shadow: <dx> <dy> <blur> <color>
        // The four tokens in that order, separated by whitespace.
        //
        float dx = 0.0f;
        float dy = 0.0f;
        float bl = 0.0f;
        gui_color sc;
        if (!_parser_style_internal__parse_number(lx, &dx)) { return FALSE; }
        _parser_style_internal__skip_ws_and_comments(lx);
        if (!_parser_style_internal__parse_number(lx, &dy)) { return FALSE; }
        _parser_style_internal__skip_ws_and_comments(lx);
        if (!_parser_style_internal__parse_number(lx, &bl)) { return FALSE; }
        _parser_style_internal__skip_ws_and_comments(lx);
        if (!_parser_style_internal__parse_color(lx, &sc))  { return FALSE; }
        style->shadow_dx    = dx;
        style->shadow_dy    = dy;
        style->shadow_blur  = bl;
        style->shadow_color = sc;
        style->has_shadow   = TRUE;
    }
    else if (stdlib__strcmp(prop, "opacity") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->opacity))
        {
            return FALSE;
        }
        style->has_opacity = TRUE;
    }
    else if (stdlib__strcmp(prop, "z-index") == 0)
    {
        float v = 0.0f;
        if (!_parser_style_internal__parse_number(lx, &v))
        {
            return FALSE;
        }
        style->z_index = (int)v;
    }
    else if (stdlib__strcmp(prop, "blur") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->blur_px))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "background-gradient") == 0 || stdlib__strcmp(prop, "bg-gradient") == 0)
    {
        //
        // background-gradient: <from-color> <to-color> [direction]
        // (Old name `bg-gradient` accepted as alias for backward
        // compat with .style files written before the rename.)
        // direction is an optional trailing ident: vertical (default),
        // horizontal, diagonal-tl, diagonal-tr.
        //
        gui_color ca;
        gui_color cb;
        if (!_parser_style_internal__parse_color(lx, &ca)) { return FALSE; }
        _parser_style_internal__skip_ws_and_comments(lx);
        if (!_parser_style_internal__parse_color(lx, &cb)) { return FALSE; }
        _parser_style_internal__skip_ws_and_comments(lx);
        gui_gradient_dir dir = GUI_GRADIENT_VERTICAL;
        //
        // Peek at the next char: if it's an alpha it's a direction ident;
        // otherwise we leave the default and let the `;` terminate.
        //
        char peeked = _parser_style_internal__peek(lx);
        if (_parser_style_internal__is_alpha(peeked))
        {
            char ident[32];
            if (!_parser_style_internal__parse_ident(lx, ident, sizeof(ident)))
            {
                return FALSE;
            }
            if      (stdlib__strcmp(ident, "vertical")     == 0) { dir = GUI_GRADIENT_VERTICAL;    }
            else if (stdlib__strcmp(ident, "horizontal")   == 0) { dir = GUI_GRADIENT_HORIZONTAL;  }
            else if (stdlib__strcmp(ident, "diagonal-tl")  == 0) { dir = GUI_GRADIENT_DIAGONAL_TL; }
            else if (stdlib__strcmp(ident, "diagonal-tr")  == 0) { dir = GUI_GRADIENT_DIAGONAL_TR; }
            else
            {
                log_warn("%s:%lld: unknown bg-gradient direction '%s', using vertical", lx->filename, (long long)lx->line, ident);
            }
        }
        style->bg_gradient_from = ca;
        style->bg_gradient_to   = cb;
        style->bg_gradient_dir  = dir;
        style->has_bg_gradient  = TRUE;
    }
    else if (stdlib__strcmp(prop, "columns") == 0)
    {
        float v = 0.0f;
        if (!_parser_style_internal__parse_number(lx, &v)) { return FALSE; }
        style->collection_columns = (int)v;
    }
    else if (stdlib__strcmp(prop, "item-width") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->item_width)) { return FALSE; }
    }
    else if (stdlib__strcmp(prop, "item-height") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->item_height)) { return FALSE; }
    }
    else if (stdlib__strcmp(prop, "layout") == 0)
    {
        char ident[16];
        if (!_parser_style_internal__parse_ident(lx, ident, sizeof(ident)))
        {
            return FALSE;
        }
        if      (stdlib__strcmp(ident, "grid") == 0) { style->collection_layout = GUI_COLLECTION_GRID; }
        else if (stdlib__strcmp(ident, "list") == 0) { style->collection_layout = GUI_COLLECTION_LIST; }
        else if (stdlib__strcmp(ident, "flow") == 0) { style->collection_layout = GUI_COLLECTION_FLOW; }
        else
        {
            log_warn("%s:%lld: unknown layout '%s' (grid|list|flow)", lx->filename, (long long)lx->line, ident);
        }
    }
    else if (stdlib__strcmp(prop, "width") == 0 || stdlib__strcmp(prop, "height") == 0)
    {
        //
        // CSS-ish dimension. Accepts:
        //   width: 200;    (bare number -> absolute pixels, like size_w)
        //   width: 200px;  (same, explicit px suffix)
        //   width: 100%;   (percent of parent's inner content area)
        //
        float v = 0.0f;
        if (!_parser_style_internal__parse_number(lx, &v)) { return FALSE; }
        _parser_style_internal__skip_ws_and_comments(lx);
        //
        // Peek the suffix. `%` sets the percent slot; `px` (or no
        // suffix) sets the pixel slot. Unknown suffixes fall through
        // to pixels with a warning so typos don't break the rest of
        // the block.
        //
        char c = _parser_style_internal__peek(lx);
        boole is_percent = FALSE;
        if (c == '%')
        {
            is_percent = TRUE;
            _parser_style_internal__advance(lx);
        }
        else if (c == 'p')
        {
            //
            // Consume "px" if present; otherwise leave the char for
            // the next token (probably ';').
            //
            _parser_style_internal__advance(lx);
            if (_parser_style_internal__peek(lx) == 'x')
            {
                _parser_style_internal__advance(lx);
            }
        }
        if (stdlib__strcmp(prop, "width") == 0)
        {
            if (is_percent) { style->width_pct = v; style->size_w = 0.0f; style->size_w_explicit = FALSE; }
            else            { style->size_w    = v; style->width_pct = 0.0f; style->size_w_explicit = TRUE; }
        }
        else
        {
            if (is_percent) { style->height_pct = v; style->size_h = 0.0f; style->size_h_explicit = FALSE; }
            else            { style->size_h     = v; style->height_pct = 0.0f; style->size_h_explicit = TRUE; }
        }
    }
    else if (stdlib__strcmp(prop, "background-image") == 0)
    {
        //
        // background-image: <path-or-string>
        // Same flexible bare-ident / quoted-string parsing the
        // font-family branch uses; path ends at ';' or whitespace
        // for bare values, or the matching quote for quoted values.
        //
        char buf[128];
        int64 n = 0;
        _parser_style_internal__skip_ws_and_comments(lx);
        char quote = 0;
        if (_parser_style_internal__peek(lx) == '"' || _parser_style_internal__peek(lx) == '\'')
        {
            quote = _parser_style_internal__peek(lx);
            _parser_style_internal__advance(lx);
        }
        while (!_parser_style_internal__eof(lx))
        {
            char c = _parser_style_internal__peek(lx);
            if (quote != 0 && c == quote)
            {
                _parser_style_internal__advance(lx);
                break;
            }
            if (quote == 0 && (c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n'))
            {
                break;
            }
            if (c == 0) { break; }
            if (n + 1 >= (int64)sizeof(buf)) { break; }
            buf[n++] = c;
            _parser_style_internal__advance(lx);
        }
        buf[n] = 0;
        size_t copy_n = (size_t)n;
        if (copy_n >= sizeof(style->background_image))
        {
            copy_n = sizeof(style->background_image) - 1;
        }
        stdlib__memcpy(style->background_image, buf, copy_n);
        style->background_image[copy_n] = 0;
        style->has_background_image = TRUE;
    }
    else if (stdlib__strcmp(prop, "background-size") == 0 || stdlib__strcmp(prop, "object-fit") == 0)
    {
        //
        // Shared fit keyword set. `stretch` is accepted as an alias
        // for `fill` because users trained on HTML/CSS tend to say
        // "stretch" colloquially even though CSS spells it `fill`.
        //
        char ident[16];
        if (!_parser_style_internal__parse_ident(lx, ident, sizeof(ident)))
        {
            return FALSE;
        }
        gui_image_fit fit = GUI_FIT_FILL;
        if      (stdlib__strcmp(ident, "fill")    == 0) { fit = GUI_FIT_FILL;    }
        else if (stdlib__strcmp(ident, "stretch") == 0) { fit = GUI_FIT_FILL;    }
        else if (stdlib__strcmp(ident, "contain") == 0) { fit = GUI_FIT_CONTAIN; }
        else if (stdlib__strcmp(ident, "cover")   == 0) { fit = GUI_FIT_COVER;   }
        else if (stdlib__strcmp(ident, "none")    == 0) { fit = GUI_FIT_NONE;    }
        else
        {
            log_warn("%s:%lld: unknown %s value '%s' (fill|stretch|contain|cover|none)", lx->filename, (long long)lx->line, prop, ident);
        }
        if (stdlib__strcmp(prop, "object-fit") == 0)
        {
            style->object_fit = fit;
        }
        else
        {
            style->background_size = fit;
        }
    }
    else if (stdlib__strcmp(prop, "horizontal-alignment") == 0 || stdlib__strcmp(prop, "halign") == 0)
    {
        //
        // `halign` is a short alias for `horizontal-alignment`.
        // Added for ergonomics -- long property name was tripping
        // meek-shell contributors who'd seen `style->halign` in the
        // C code and reached for the obvious short form.
        //
        char ident[16];
        if (!_parser_style_internal__parse_ident(lx, ident, sizeof(ident)))
        {
            return FALSE;
        }
        if      (stdlib__strcmp(ident, "left")   == 0) { style->halign = GUI_HALIGN_LEFT;   }
        else if (stdlib__strcmp(ident, "center") == 0) { style->halign = GUI_HALIGN_CENTER; }
        else if (stdlib__strcmp(ident, "right")  == 0) { style->halign = GUI_HALIGN_RIGHT;  }
        else
        {
            log_warn("%s:%lld: unknown horizontal-alignment '%s' (left|center|right)", lx->filename, (long long)lx->line, ident);
        }
    }
    else if (stdlib__strcmp(prop, "vertical-alignment") == 0 || stdlib__strcmp(prop, "valign") == 0)
    {
        char ident[16];
        if (!_parser_style_internal__parse_ident(lx, ident, sizeof(ident)))
        {
            return FALSE;
        }
        if      (stdlib__strcmp(ident, "top")    == 0) { style->valign = GUI_VALIGN_TOP;    }
        else if (stdlib__strcmp(ident, "center") == 0) { style->valign = GUI_VALIGN_CENTER; }
        else if (stdlib__strcmp(ident, "bottom") == 0) { style->valign = GUI_VALIGN_BOTTOM; }
        else
        {
            log_warn("%s:%lld: unknown vertical-alignment '%s' (top|center|bottom)", lx->filename, (long long)lx->line, ident);
        }
    }
    else if (stdlib__strcmp(prop, "border-width") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->border_width))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "border-color") == 0)
    {
        //
        // Two accepted shapes:
        //   border-color: #rrggbb;                                     (solid, existing)
        //   border-color: gradient(#from, #to [, direction]);          (gradient, new)
        //   border-color: linear-gradient(#from, #to [, direction]);   (CSS-ish alias)
        //
        // The keyword form is dispatched by peeking: '#' -> solid,
        // alpha -> gradient keyword. Anything else is a parse error.
        // For the gradient case we populate the dedicated
        // border_gradient_* slots AND mirror the `from` colour into
        // border_color + has_border_color so style consumers that
        // don't speak gradient yet still get a sensible solid line.
        //
        _parser_style_internal__skip_ws_and_comments(lx);
        char peeked = _parser_style_internal__peek(lx);
        if (peeked == '#')
        {
            gui_color c;
            if (!_parser_style_internal__parse_color(lx, &c))
            {
                return FALSE;
            }
            style->border_color     = c;
            style->has_border_color = TRUE;
        }
        else if (_parser_style_internal__is_alpha(peeked))
        {
            char ident[20];
            if (!_parser_style_internal__parse_ident(lx, ident, (int64)sizeof(ident)))
            {
                return FALSE;
            }
            if (stdlib__strcmp(ident, "gradient") != 0 &&
                stdlib__strcmp(ident, "linear-gradient") != 0)
            {
                _parser_style_internal__error(lx, "border-color: expected '#rrggbb' or 'gradient(...)'");
                return FALSE;
            }
            _parser_style_internal__skip_ws_and_comments(lx);
            if (_parser_style_internal__peek(lx) != '(')
            {
                _parser_style_internal__error(lx, "border-color gradient: expected '('");
                return FALSE;
            }
            _parser_style_internal__advance(lx);
            _parser_style_internal__skip_ws_and_comments(lx);

            gui_color ca;
            if (!_parser_style_internal__parse_color(lx, &ca)) { return FALSE; }
            _parser_style_internal__skip_ws_and_comments(lx);
            //
            // Comma is optional between args (matching the loose style
            // elsewhere); if present, consume it.
            //
            if (_parser_style_internal__peek(lx) == ',')
            {
                _parser_style_internal__advance(lx);
                _parser_style_internal__skip_ws_and_comments(lx);
            }

            gui_color cb;
            if (!_parser_style_internal__parse_color(lx, &cb)) { return FALSE; }
            _parser_style_internal__skip_ws_and_comments(lx);

            gui_gradient_dir dir = GUI_GRADIENT_VERTICAL;
            if (_parser_style_internal__peek(lx) == ',')
            {
                _parser_style_internal__advance(lx);
                _parser_style_internal__skip_ws_and_comments(lx);
            }
            if (_parser_style_internal__is_alpha(_parser_style_internal__peek(lx)))
            {
                char dir_ident[24];
                if (!_parser_style_internal__parse_ident(lx, dir_ident, (int64)sizeof(dir_ident)))
                {
                    return FALSE;
                }
                if      (stdlib__strcmp(dir_ident, "vertical")     == 0) { dir = GUI_GRADIENT_VERTICAL;    }
                else if (stdlib__strcmp(dir_ident, "horizontal")   == 0) { dir = GUI_GRADIENT_HORIZONTAL;  }
                else if (stdlib__strcmp(dir_ident, "diagonal-tl")  == 0) { dir = GUI_GRADIENT_DIAGONAL_TL; }
                else if (stdlib__strcmp(dir_ident, "diagonal-tr")  == 0) { dir = GUI_GRADIENT_DIAGONAL_TR; }
                else
                {
                    log_warn("%s:%lld: unknown border-color gradient direction '%s', using vertical", lx->filename, (long long)lx->line, dir_ident);
                }
                _parser_style_internal__skip_ws_and_comments(lx);
            }
            if (_parser_style_internal__peek(lx) != ')')
            {
                _parser_style_internal__error(lx, "border-color gradient: expected ')'");
                return FALSE;
            }
            _parser_style_internal__advance(lx);

            style->border_gradient_from = ca;
            style->border_gradient_to   = cb;
            style->border_gradient_dir  = dir;
            style->has_border_gradient  = TRUE;
            //
            // Mirror the start colour to the solid slot so the border
            // still draws if a future code path checks has_border_color
            // before has_border_gradient.
            //
            style->border_color         = ca;
            style->has_border_color     = TRUE;
        }
        else
        {
            _parser_style_internal__error(lx, "border-color: expected '#rrggbb' or 'gradient(...)'");
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "border-style") == 0)
    {
        char ident[16];
        if (!_parser_style_internal__parse_ident(lx, ident, sizeof(ident)))
        {
            return FALSE;
        }
        if      (stdlib__strcmp(ident, "none")   == 0) { style->border_style = GUI_BORDER_NONE;   }
        else if (stdlib__strcmp(ident, "solid")  == 0) { style->border_style = GUI_BORDER_SOLID;  }
        else if (stdlib__strcmp(ident, "dashed") == 0) { style->border_style = GUI_BORDER_DASHED; }
        else if (stdlib__strcmp(ident, "dotted") == 0) { style->border_style = GUI_BORDER_DOTTED; }
        else
        {
            log_warn("%s:%lld: unknown border-style '%s' (none|solid|dashed|dotted)", lx->filename, (long long)lx->line, ident);
        }
    }
    else if (stdlib__strcmp(prop, "border") == 0)
    {
        //
        // CSS shorthand: `border: 2px solid #fff;` / `border: solid
        // 1 #ff0;` / etc. Tokens are space-separated; we classify
        // each by its leading character:
        //   '#'         -> color
        //   digit / '.' -> width (with optional 'px' suffix)
        //   letter      -> style keyword (solid|dashed|dotted|none)
        //
        // Order is irrelevant. Missing components leave the matching
        // field at its current value (no implicit zeroing). The
        // typical usage stays canonical "<width> <style> <color>".
        //
        for (int slot = 0; slot < 3; slot++)
        {
            _parser_style_internal__skip_ws_and_comments(lx);
            char c = _parser_style_internal__peek(lx);
            if (c == ';' || c == '}' || c == 0) { break; }

            if (c == '#')
            {
                gui_color col;
                if (!_parser_style_internal__parse_color(lx, &col))
                {
                    return FALSE;
                }
                style->border_color     = col;
                style->has_border_color = TRUE;
            }
            else if ((c >= '0' && c <= '9') || c == '.')
            {
                if (!_parser_style_internal__parse_number(lx, &style->border_width))
                {
                    return FALSE;
                }
                //
                // Accept and discard a trailing "px" so `2px` parses
                // the same as `2`. CSS-natural; matches existing
                // width / height parsing in this file.
                //
                if (_parser_style_internal__peek(lx) == 'p')
                {
                    _parser_style_internal__advance(lx);
                    if (_parser_style_internal__peek(lx) == 'x')
                    {
                        _parser_style_internal__advance(lx);
                    }
                }
            }
            else
            {
                char ident[16];
                if (!_parser_style_internal__parse_ident(lx, ident, sizeof(ident)))
                {
                    return FALSE;
                }
                if      (stdlib__strcmp(ident, "none")   == 0) { style->border_style = GUI_BORDER_NONE;   }
                else if (stdlib__strcmp(ident, "solid")  == 0) { style->border_style = GUI_BORDER_SOLID;  }
                else if (stdlib__strcmp(ident, "dashed") == 0) { style->border_style = GUI_BORDER_DASHED; }
                else if (stdlib__strcmp(ident, "dotted") == 0) { style->border_style = GUI_BORDER_DOTTED; }
                else
                {
                    log_warn("%s:%lld: unknown border style keyword '%s' in shorthand (none|solid|dashed|dotted)", lx->filename, (long long)lx->line, ident);
                }
            }
        }
    }
    else
    {
        //
        // Unknown property. Warn, then skip tokens until the next ';'
        // or '}' so the rest of the block still parses. This is the
        // forgiving path -- a typo like `radiuz: 6;` triggers one
        // warning but doesn't invalidate the whole file.
        //
        known = FALSE;
        log_warn("%s:%lld: unknown property '%s' (skipping)", lx->filename, (long long)lx->line, prop);
        while (!_parser_style_internal__eof(lx))
        {
            char c = _parser_style_internal__peek(lx);
            if (c == ';' || c == '}')
            {
                break;
            }
            _parser_style_internal__advance(lx);
        }
    }

    //
    // ----- mandatory ';' ---------------------------------------------------
    // Every declaration MUST end with ';' -- no CSS-style "last
    // declaration can omit the semicolon". This keeps the parser
    // deterministic (we can always require ';' before going back to
    // the block loop).
    //
    _parser_style_internal__skip_ws_and_comments(lx);
    if (_parser_style_internal__peek(lx) != ';')
    {
        _parser_style_internal__error(lx, "expected ';' at end of declaration");
        return FALSE;
    }
    _parser_style_internal__advance(lx);
    //
    // `known` is (void)'d because it's only used for its side effect
    // (logging the warning); nothing consumes its value. Keeping the
    // variable makes the flow clearer, and gives future code a hook
    // if we ever want to stop processing the block after an unknown
    // property.
    //
    (void)known;
    return TRUE;
}

//
// ===========================================================================
// Block parser -- declarations + nested pseudo-state rules.
// ===========================================================================
//

//
// Parse the body of a `{ ... }` block. A block is a mix of:
//   - Declarations (name: value;) that accumulate into one gui_style
//     which gets registered at block-end under `base_selector`.
//   - Nested pseudo-state rules (`:hover { ... }`) that each build
//     their own "base:pseudo" selector and register a separate style.
//
// Nested pseudo-state rules recurse back through this same function
// with the composed selector. This mirrors the CSS "&:hover" nesting
// syntax but without needing the '&' prefix -- ':hover' on its own
// is unambiguously a pseudo-state when it appears inside a block.
//
// A block that contains ONLY nested rules (no own declarations) does
// NOT register a style for `base_selector` itself -- the has_decls
// flag guards that. This is why you can write:
//
//   Button {
//       :hover { bg: #555; }
//       :pressed { bg: #222; }
//   }
//
// ...without producing a "Button { }" rule that would write zeros
// over nothing and waste a table slot.
//
static void _parser_style_internal__parse_block(_parser_style_internal__lexer* lx, char* base_selector)
{
    //
    // Require an opening '{'.
    //
    if (_parser_style_internal__peek(lx) != '{')
    {
        _parser_style_internal__error(lx, "expected '{'");
        return;
    }
    _parser_style_internal__advance(lx);

    //
    // Fresh style object for this block's declarations to fill in.
    // zero-initialized so every has_* flag starts FALSE.
    //
    gui_style style;
    stdlib__memset(&style, 0, sizeof(style));
    boole has_decls = FALSE;

    //
    // Main block loop. Each iteration decides what kind of element
    // we're looking at based on the first non-whitespace character.
    //
    while (lx->ok)
    {
        _parser_style_internal__skip_ws_and_comments(lx);
        char c = _parser_style_internal__peek(lx);

        //
        // End of block -- consume '}' and exit.
        //
        if (c == '}')
        {
            _parser_style_internal__advance(lx);
            break;
        }
        //
        // Premature EOF. Log and return; the top-level parse loop
        // will observe lx->ok == FALSE and stop.
        //
        if (c == 0)
        {
            _parser_style_internal__error(lx, "unexpected end of file inside block");
            return;
        }

        //
        // ----- Nested pseudo-state rule ----------------------------
        //
        // `:hover { ... }` (or `:pressed`, `:disabled`, `:default`)
        // inside a block means "style the parent selector's nodes
        // when they're in this state". We compose the full selector
        // as "parent:pseudo" and recurse -- parse_block will register
        // a new rule with that selector + its own gui_style.
        //
        if (c == ':')
        {
            _parser_style_internal__advance(lx);
            char pseudo[32];
            if (!_parser_style_internal__parse_ident(lx, pseudo, (int64)sizeof(pseudo)))
            {
                return;
            }

            //
            // Build "base:pseudo" into a 160-byte buffer. If we
            // overflow (very long id + long pseudo -- unlikely but
            // checked), bail with an error.
            //
            char nested[160];
            int  written = snprintf(nested, sizeof(nested), "%s:%s", base_selector, pseudo);
            if (written < 0 || written >= (int)sizeof(nested))
            {
                _parser_style_internal__error(lx, "nested selector too long");
                return;
            }

            _parser_style_internal__skip_ws_and_comments(lx);
            _parser_style_internal__parse_block(lx, nested); // recurse.
            continue;
        }

        //
        // ----- Ordinary declaration: name: value; ------------------
        //
        if (!_parser_style_internal__parse_decl(lx, &style))
        {
            return;
        }
        has_decls = TRUE;
    }

    //
    // Register the accumulated style under `base_selector`, but only
    // if the block contained at least one declaration. Blocks that
    // are only-nested-rules don't create a zero rule for their parent.
    //
    // Empty base_selector is defensive: shouldn't happen (parse_rule
    // fails if the selector is empty), but if it did we'd register
    // under "" which would match nothing.
    //
    if (has_decls && base_selector[0] != 0)
    {
        //
        // Border partial-declaration check. The renderer's three-way
        // gate in scene__emit_border requires width > 0 AND style
        // != none AND has_border_color -- partial declarations
        // produce no visible border, which surfaces as "I set
        // border-width: 2px and nothing happened". Logging here
        // catches the common case at parse time so the dev sees
        // the warning instead of debugging silent no-paint.
        //
        boole has_w = (style.border_width  > 0.0f);
        boole has_s = (style.border_style != GUI_BORDER_NONE);
        boole has_c = style.has_border_color;
        if ((has_w || has_s || has_c) && !(has_w && has_s && has_c))
        {
            log_warn("%s:%lld: '%s' has partial border declaration (width=%s style=%s color=%s); border won't paint until all three are set", lx->filename, (long long)lx->line, base_selector, has_w ? "yes" : "NO", has_s ? "yes" : "NO", has_c ? "yes" : "NO");
        }
        scene__register_style(base_selector, &style);
    }
}

//
// Parse a top-level rule: `selector [, selector2, ...] { block }`.
// Called from the stylesheet loop in parser_style__load_styles.
//
// Comma-separated selectors are supported: the block is applied to
// every listed selector, equivalent to copy-pasting the block under
// each one. Implementation: we collect the selector list first, then
// snapshot the lexer position at the opening of the block and
// re-parse the same block once per selector (rewinding in between).
// Re-parsing costs a few microseconds per extra selector and keeps
// parse_block's internals (declaration dispatch, nested pseudo-state
// rules, error handling) unchanged.
//
// No-space-in-selector rule still applies per token -- `.a b` is two
// tokens separated by whitespace, which we don't support. Commas ARE
// separators though; they and the whitespace around them are fine.
//
#define _PARSER_STYLE_INTERNAL_MAX_SELECTOR_LIST 16

static void _parser_style_internal__parse_rule(_parser_style_internal__lexer* lx)
{
    char selectors[_PARSER_STYLE_INTERNAL_MAX_SELECTOR_LIST][160];
    int64 n_selectors = 0;

    //
    // Read first selector (mandatory).
    //
    if (!_parser_style_internal__parse_selector(lx, selectors[0], (int64)sizeof(selectors[0])))
    {
        return;
    }
    n_selectors = 1;

    //
    // Optionally read `, selector2, selector3, ...`. Each iteration
    // consumes the comma + surrounding whitespace, then one more
    // selector. Stops when the next non-ws char isn't ','.
    //
    _parser_style_internal__skip_ws_and_comments(lx);
    while (_parser_style_internal__peek(lx) == ',')
    {
        _parser_style_internal__advance(lx);          //consume ','
        _parser_style_internal__skip_ws_and_comments(lx);

        if (n_selectors >= _PARSER_STYLE_INTERNAL_MAX_SELECTOR_LIST)
        {
            _parser_style_internal__error(lx, "too many comma-separated selectors");
            return;
        }
        if (!_parser_style_internal__parse_selector(lx,
                selectors[n_selectors], (int64)sizeof(selectors[n_selectors])))
        {
            return;
        }
        n_selectors++;
        _parser_style_internal__skip_ws_and_comments(lx);
    }

    //
    // At this point the lexer is positioned at the opening '{' of
    // the block (or some garbage that parse_block will reject).
    // Snapshot pos + line so we can rewind per selector. `ok` is NOT
    // saved -- if the block fails for one selector, bailing out is
    // the right behavior; don't mask errors by retrying.
    //
    int64 block_pos  = lx->pos;
    int64 block_line = lx->line;

    for (int64 i = 0; i < n_selectors; ++i)
    {
        //
        // Rewind for every iteration (including the first, even
        // though it's a no-op there, for symmetry/clarity).
        //
        lx->pos  = block_pos;
        lx->line = block_line;
        _parser_style_internal__parse_block(lx, selectors[i]);
        if (!lx->ok)
        {
            return;
        }
    }

    //
    // Lexer is now past the block's closing '}' (from the last
    // iteration). Exactly where the single-selector path would have
    // left it -- the stylesheet loop picks up from here.
    //
}
