#ifndef GUI_H
#define GUI_H

#include "types.h"

//
//gui.h - common types shared by every module of the toolkit.
//declarations only. per-module .h files (scene.h, renderer.h,
//platform_win32.h, parser_xml.h) declare module-specific apis.
//

/**
 *rgba colour, components in 0..1.
 */
typedef struct gui_color
{
    float r;
    float g;
    float b;
    float a;
} gui_color;

/**
 *2d float vector, used for points and sizes.
 */
typedef struct gui_vec2
{
    float x;
    float y;
} gui_vec2;

/**
 *axis-aligned rectangle, top-left origin, in pixels.
 */
typedef struct gui_rect
{
    float x;
    float y;
    float w;
    float h;
} gui_rect;

/**
 *scene graph node kinds. parser_xml maps tag names to these.
 *more kinds (text, image, stack, grid, input) arrive in later
 *iterations.
 */
typedef enum gui_node_type
{
    GUI_NODE_WINDOW,
    GUI_NODE_COLUMN,
    GUI_NODE_ROW,
    GUI_NODE_BUTTON,
    GUI_NODE_SLIDER,
    GUI_NODE_DIV,
    GUI_NODE_TEXT,
    GUI_NODE_INPUT,
    GUI_NODE_CHECKBOX,
    GUI_NODE_RADIO,
    GUI_NODE_SELECT,
    GUI_NODE_OPTION,    // child of <select>; carries one option's text + value.
    GUI_NODE_IMAGE,     // <image src="..."/> -- PNG/JPEG loaded via stb_image.
    GUI_NODE_COLLECTION,   // <collection> -- grid / list / flow of child nodes.
    GUI_NODE_COLOR_PICKER, // <color-picker mode="slider|square"> -- hue strip or HSV square.
    GUI_NODE_POPUP,        // <popup> -- modal overlay with body + OK / yes-no buttons.
    GUI_NODE_TEXTAREA,     // <textarea> -- multi-line text input with word-wrap.
    GUI_NODE_CANVAS,       // <canvas> -- RGBA8 drawing surface (paint-app style).
    GUI_NODE_KEYBOARD,     // <keyboard> -- on-screen ASCII keyboard for touch displays.
    GUI_NODE_PROCESS_WINDOW, // <process-window> -- live texture of another process's rendered buffer (compositor forwards via meek_shell_v1).
    GUI_NODE_TYPE_COUNT // sentinel; sizes the widget vtable table. add new node kinds before this line.
} gui_node_type;

/**
 *per-state style slots. the resolver picks the slot matching n->state
 *(default/hover/pressed/disabled) each frame and copies it into
 *`resolved`. the APPEAR and DISAPPEAR slots are NOT regular states --
 *the resolver never selects them based on n->state. instead they hold
 *the STARTING values for an :appear animation (where the node fades
 *in from those values to the default-state values) and the FINAL
 *values for a :disappear animation (where the node fades to those
 *values before being removed). the animator reads them, not the
 *resolver.
 */
typedef enum gui_node_state
{
    GUI_STATE_DEFAULT   = 0,
    GUI_STATE_HOVER     = 1,
    GUI_STATE_PRESSED   = 2,
    GUI_STATE_DISABLED  = 3,
    GUI_STATE_APPEAR    = 4, // starting values for the :appear animation
    GUI_STATE_DISAPPEAR = 5, // ending values for the :disappear animation (deferred)
    GUI_STATE_COUNT     = 6
} gui_node_state;

/**
 *easing functions for animations. matches the named curves CSS
 *exposes via cubic-bezier shortcuts. linear is the identity; the
 *other three sit at the fast-start, slow-start, and slow-start-and-end
 *positions on the speed envelope.
 *
 *eased(t) maps a normalized progress t in [0,1] to the eased value
 *in [0,1]:
 *
 *  LINEAR        eased = t                      constant speed.
 *  EASE_IN       eased = t * t                  starts slow, ends fast.
 *  EASE_OUT      eased = 1 - (1-t)^2            starts fast, ends slow. natural for "pop in" effects.
 *  EASE_IN_OUT   smooth at both ends, fast in the middle (cubic).
 */
typedef enum gui_easing
{
    GUI_EASING_LINEAR           = 0,
    GUI_EASING_EASE_IN          = 1,
    GUI_EASING_EASE_OUT         = 2, // default for :appear -- the most natural "pop"
    GUI_EASING_EASE_IN_OUT      = 3,

    //
    // CSS-derived keyword presets. `ease` is the default CSS transition
    // timing function (cubic-bezier(0.25, 0.1, 0.25, 1.0)). The *_BACK
    // variants overshoot past the end value and ease back — good for
    // springy panel pops. *_BOUNCE decelerates in diminishing oscillations
    // as if the element were a dropped ball settling on the target.
    //
    GUI_EASING_EASE             = 4,
    GUI_EASING_EASE_IN_BACK     = 5,
    GUI_EASING_EASE_OUT_BACK    = 6,
    GUI_EASING_EASE_IN_OUT_BACK = 7,
    GUI_EASING_EASE_OUT_BOUNCE  = 8,

    //
    // Parameterised curves. Values are read from the matching
    // `_easing_params[4]` slot on the style (appear_easing_params,
    // animation_easing_params, transition_easing_params). The parser
    // populates the slot when these kinds are chosen; the animator
    // reads them at apply time.
    //
    //   CUBIC_BEZIER: params = { x1, y1, x2, y2 } — same convention as
    //     CSS `cubic-bezier(x1, y1, x2, y2)`. The curve goes (0,0) -> P1
    //     -> P2 -> (1,1) with the two control points shaping the arc.
    //
    //   SPRING: params = { stiffness, damping, _, _ } — see animator.c
    //     for the exponentially-damped-oscillator formula. Higher
    //     stiffness = more oscillations in the visible range, higher
    //     damping = faster settle. `spring(20, 5)` is a mild 2-3 bounce.
    //
    GUI_EASING_CUBIC_BEZIER     = 9,
    GUI_EASING_SPRING           = 10
} gui_easing;

/**
 *css-like overflow modes for a single axis. controls both clipping
 *(not yet implemented -- see widget_div.c note) and whether a scrollbar
 *is drawn.
 *
 *  GUI_OVERFLOW_VISIBLE  default; content can draw outside the bounds;
 *                        no scrollbar; no clipping.
 *  GUI_OVERFLOW_HIDDEN   content is clipped to bounds (once renderer
 *                        scissor lands); no scrollbar shown.
 *  GUI_OVERFLOW_SCROLL   always show a scrollbar for the axis even if
 *                        content fits; clip to bounds.
 *  GUI_OVERFLOW_AUTO     show a scrollbar only if content exceeds
 *                        bounds on that axis; clip to bounds.
 *
 *the two "scroll" modes (SCROLL, AUTO) consume scroll input on the
 *containing div and offset children by scroll_x / scroll_y.
 */
typedef enum gui_overflow
{
    GUI_OVERFLOW_VISIBLE = 0, // default; no clip, no scrollbar.
    GUI_OVERFLOW_HIDDEN  = 1, // clip (when supported); no scrollbar.
    GUI_OVERFLOW_SCROLL  = 2, // always show bar; clip.
    GUI_OVERFLOW_AUTO    = 3  // show bar only when content exceeds bounds; clip.
} gui_overflow;

/**
 *css-like "is this element painted?" switch. orthogonal to display:
 *a hidden element still reserves layout space, its neighbours stay
 *put. Closely mirrors the CSS `visibility` property, but it doesn't
 *cascade in our implementation -- each node decides for itself.
 *Children of a hidden parent continue to paint based on their own
 *resolved.visibility. If you want to hide a subtree entirely, use
 *GUI_DISPLAY_NONE on the parent.
 *
 *  GUI_VISIBILITY_VISIBLE  default; node's emit_draws runs normally.
 *  GUI_VISIBILITY_HIDDEN   node itself is skipped by emit_draws, but
 *                          layout treats it as fully present (same
 *                          bounds, children in their usual places).
 */
typedef enum gui_visibility
{
    GUI_VISIBILITY_VISIBLE = 0, // default; drawn.
    GUI_VISIBILITY_HIDDEN  = 1  // not drawn; still reserves layout space.
} gui_visibility;

/**
 *css-like "is this element in the layout?" switch. Orthogonal to
 *visibility: display: none removes the element AND its subtree from
 *both layout and rendering, so siblings collapse into the space it
 *would otherwise take.
 *
 *Only two values in this PoC; the HTML "block / inline / flex / grid /
 *table ..." distinctions collapse into a single "participates in
 *layout" vs "doesn't exist" bit because we only have one layout model
 *(column + row + fill). Future layouts (grid etc.) could grow this.
 *
 *  GUI_DISPLAY_BLOCK   default; node participates normally.
 *  GUI_DISPLAY_NONE    node and its entire subtree are skipped for
 *                      layout and rendering; parent's column/row
 *                      layout advances PAST this child as if it
 *                      weren't there.
 */
typedef enum gui_display
{
    GUI_DISPLAY_BLOCK = 0, // default; laid out and drawn.
    GUI_DISPLAY_NONE  = 1  // excluded from both layout and rendering.
} gui_display;

/**
 * CSS-aligned positioning model. Static is the default and produces
 * normal column / row sibling flow. Absolute lifts the child out of
 * its parent's flow entirely -- siblings act as if it didn't exist
 * for the purpose of cursor advance and total-extent measurement --
 * and lays the child out against the parent's content rect using
 * the inset_* + size fields. Used to overlay the task switcher on
 * top of the launcher grid in meek-shell, where the two need to
 * coexist visually instead of swap.
 *
 * Containing block: the IMMEDIATE parent's content rect (post-pad,
 * post-border). CSS proper has a "nearest positioned ancestor"
 * walk; we don't bother. Tighten if needed later.
 */
typedef enum gui_position
{
    GUI_POSITION_STATIC   = 0, // default; participates in column/row flow.
    GUI_POSITION_RELATIVE = 1, // flows normally, then shifts by inset_l/t (without affecting siblings).
    GUI_POSITION_ABSOLUTE = 2, // lifted out of flow, anchored to nearest positioned ancestor's content rect via insets.
    GUI_POSITION_FIXED    = 3, // anchored to the viewport (root) via insets, regardless of ancestors.
    //
    // sticky is intentionally NOT here yet -- it requires per-frame
    // scroll-context awareness which neither widget_window nor
    // widget_div expose to the layout pass cleanly. Add when needed.
    //
} gui_position;

/**
 * Direction for the two-color linear gradient used when bg-gradient is
 * set. The four cardinal values cover the common launcher/card looks
 * without dragging full angle arithmetic into the parser yet.
 */
typedef enum gui_gradient_dir
{
    GUI_GRADIENT_VERTICAL     = 0, // top -> bottom (default).
    GUI_GRADIENT_HORIZONTAL   = 1, // left -> right.
    GUI_GRADIENT_DIAGONAL_TL  = 2, // top-left -> bottom-right.
    GUI_GRADIENT_DIAGONAL_TR  = 3  // top-right -> bottom-left.
} gui_gradient_dir;

/**
 * Layout mode for <collection>. GRID: fixed number of columns, rows
 * grown to fit; each cell sized to item_size (or auto from child).
 * LIST: single-row strip, children placed horizontally. FLOW: wrap
 * children onto as many rows as fit in the available width.
 */
typedef enum gui_collection_layout
{
    GUI_COLLECTION_GRID = 0,
    GUI_COLLECTION_LIST = 1,
    GUI_COLLECTION_FLOW = 2
} gui_collection_layout;

/**
 * How an image is fitted inside a container's bounds. Mirrors CSS's
 * `object-fit` / `background-size` keyword set.
 *
 *   FILL      stretch to fully fill bounds, breaking aspect ratio.
 *             same as the pre-property <image> behavior.
 *   CONTAIN   scale uniformly until the LONGER side fits, letterbox
 *             the rest. whole image is always visible.
 *   COVER     scale uniformly until the SHORTER side fills, crop the
 *             overflow via renderer scissor. image fills bounds but
 *             some of it is clipped.
 *   NONE      draw at the image's natural pixel size, centered inside
 *             bounds. if the image is bigger than bounds, the excess
 *             is scissored off.
 *
 * Accepted spellings in .style: `fill`, `stretch` (alias for fill),
 * `contain`, `cover`, `none`. Default is FILL for <image> (preserves
 * the old behavior) and FILL for container background-size too.
 */
typedef enum gui_image_fit
{
    GUI_FIT_FILL    = 0,
    GUI_FIT_CONTAIN = 1,
    GUI_FIT_COVER   = 2,
    GUI_FIT_NONE    = 3
} gui_image_fit;

/**
 * Horizontal alignment of children inside a column/div/window (applies
 * per-child within the container's inner width), or of the WHOLE ROW
 * of children inside a row container (group-level).
 */
typedef enum gui_halign
{
    GUI_HALIGN_LEFT   = 0,
    GUI_HALIGN_CENTER = 1,
    GUI_HALIGN_RIGHT  = 2
} gui_halign;

/**
 * Vertical alignment. Mirror of gui_halign. In a column-style
 * container it positions the WHOLE STACK of children within the
 * parent's inner height; in a row-style container it applies
 * per-child.
 */
typedef enum gui_valign
{
    GUI_VALIGN_TOP    = 0,
    GUI_VALIGN_CENTER = 1,
    GUI_VALIGN_BOTTOM = 2
} gui_valign;

/**
 *visual style for a node. populated by the selector resolver in scene.c.
 *zero floats are treated as "not specified" by the overlay logic;
 *has_background_color / has_accent_color / has_font_color are the
 *explicit flags for color fields.
 */
//
// Border line style. CSS-shaped subset: solid, dashed, dotted, plus
// NONE which is also the default (no border drawn even if a width
// or color is set). The renderer-side dashed / dotted paths are
// stubbed for the first iteration -- they parse, store correctly,
// and currently render as SOLID. Switching them to a real
// per-edge stipple is a future pass.
//
typedef enum gui_border_style
{
    GUI_BORDER_NONE   = 0,
    GUI_BORDER_SOLID  = 1,
    GUI_BORDER_DASHED = 2,
    GUI_BORDER_DOTTED = 3,
} gui_border_style;

typedef struct gui_style
{
    gui_color background_color;       // container fill. CSS-name: background-color.
    boole     has_background_color;
    gui_color accent_color;           // active widget part: slider thumb, checkbox tick, radio dot, select chevron, image tint. CSS-4-name: accent-color.
    boole     has_accent_color;
    gui_color font_color;             // text color. CSS-name: color, but renamed for unambiguity.
    boole     has_font_color;
    //
    // Border. border_width is in logical pixels (multiplied by the
    // global scale at draw time, like every other size field).
    // Border draws when border_width > 0 AND border_style != NONE
    // AND has_border_color is TRUE -- all three must align so a
    // partial declaration (just `border-width: 2;`) doesn't paint
    // an invisible black border by accident.
    //
    float            border_width;
    gui_color        border_color;
    boole            has_border_color;
    //
    // Optional gradient form of border-color. When the parser sees
    // `border-color: gradient(<from>, <to>[, direction])` it populates
    // these fields and sets has_border_gradient=TRUE; the emitter then
    // draws the outer border rect with submit_rect_gradient rather than
    // the solid submit_rect. Solid border_color remains the fallback
    // (stored with has_border_color=TRUE) so a caller that reads the
    // struct without gradient-awareness still gets a sensible color.
    //
    gui_color        border_gradient_from;
    gui_color        border_gradient_to;
    gui_gradient_dir border_gradient_dir;
    boole            has_border_gradient;
    gui_border_style border_style;
    float     radius; // corner radius in pixels. 0 = square. uniform for now; per-corner radii arrive in a later iteration.
    float     pad_t;
    float     pad_r;
    float     pad_b;
    float     pad_l;
    //
    // CSS-aligned margin: space AROUND this element, outside its
    // own bounds. Distinct from `gap` (space BETWEEN siblings inside
    // a flex/grid container) and `pad` (space INSIDE this element,
    // between its border and its children). Currently parsed but
    // NOT YET honored by the layout pass -- wiring widgets to read
    // these offsets is a follow-up.
    //
    float     margin_t;
    float     margin_r;
    float     margin_b;
    float     margin_l;
    float     gap;
    float     size_w; // 0.0f means "auto" in the poc.
    float     size_h; // 0.0f means "auto" in the poc.
    //
    // size_w_explicit / size_h_explicit / font_size_explicit are
    // "did a style rule write a px-valued width / height / font-size
    // for the rule's state slot". The animator uses them to gate
    // size+font interpolation: a transition only lerps the property
    // when BOTH endpoints are flagged as rule-defined. Stops a
    // transient unset endpoint (which the resolver may emit during
    // bootstrap or class re-evaluation) from corrupting layout.
    //
    boole     size_w_explicit;
    boole     size_h_explicit;
    //
    // width/height as a percentage of the PARENT's inner content
    // area (post-pad). 0 means "not set" -- fall back to size_w /
    // size_h. Non-zero values WIN over size_w / size_h so a style
    // with both ends up percent-driven, which is what you'd
    // intuitively expect from CSS. Accepted as `width: 100%;` or
    // `height: 50%;` in .style files. Values >100 are legal (if
    // the parent allows overflow) but uncommon.
    //
    float     width_pct;
    float     height_pct;
    float     min_w;
    float     min_h;
    char      font_family[64]; // empty = default (first registered font). 64 bytes to fit longer family names like "SourceCodePro-Semibold-Italic".
    float     font_size; // 0 = inherit/default.
    boole     font_size_explicit; // see size_w_explicit block above.

    //
    // ===== overflow + scrollbar styling =====================================
    //
    // controls whether the node shows scrollbars and, when it does, their
    // visual appearance. currently only respected by <div> -- other
    // container widgets ignore these fields. the two axes are independent
    // so you can get `overflow-y: auto` behavior without touching x.
    //
    // layout interaction (widget_div.c):
    //   when either axis is SCROLL or AUTO, the div lays out its children
    //   at (child_x, child_y - scroll_y) and computes content_w/_h from
    //   the resulting child extents. the scrollbar thumb's size and
    //   position are derived from content_h vs bounds.h.
    //
    // zero fields = use built-in defaults (12px bar, 6px radius, neutral
    // dark track + lighter thumb). this keeps .style files terse for the
    // common case where you just want "working" scrollbars.
    //
    gui_overflow overflow_x;
    gui_overflow overflow_y;
    float        scrollbar_size;   // 0 = default (~12px). thickness of the bar.
    float        scrollbar_radius; // 0 = default (~scrollbar_size/2). rounds track + thumb.
    gui_color    scrollbar_track;  // bar background (the "channel" behind the thumb).
    boole        has_scrollbar_track;
    gui_color    scrollbar_thumb;  // the draggable thumb.
    boole        has_scrollbar_thumb;

    //
    // ===== animation =========================================================
    //
    // Two ways to drive an appear/disappear animation:
    //
    // 1. SHORTCUT (fade-in only):
    //      Window { appear: 600 ease_out; }
    //    Sets appear_ms / appear_easing on this rule. The animator
    //    multiplies every color's alpha by the eased progress factor
    //    (everything fades in from invisible). Inherited by descendants
    //    like font_family / font_size, so one declaration on Window
    //    cascades to the whole tree.
    //
    // 2. PROPER PSEUDO-STATE (per-property control):
    //      Button.primary {
    //          bg: #6366f1;
    //          :appear {
    //              bg: #6366f100;             /* starting value (8-digit hex = with alpha) */
    //              transition: 600 ease_out;  /* duration + easing for THIS state's animation */
    //          }
    //      }
    //    The :appear block becomes style[GUI_STATE_APPEAR]. The
    //    transition declaration sets transition_duration_ms +
    //    transition_easing on that same slot. The animator
    //    interpolates each property listed in :appear FROM its
    //    :appear value TO whatever the resolver computed for the
    //    node's current state -- so the animation works even if the
    //    user is hovering during the fade-in.
    //
    // Both modes are checked on every node; the pseudo-state mode
    // wins when transition_duration_ms is set (so a button with
    // both inherits Window's appear_ms but overrides it with its
    // own per-property pseudo-state animation).
    //
    // Defaults: zero everywhere = no animation. inherited like the
    // other "typography-style" properties.
    //
    float      appear_ms;
    gui_easing appear_easing;
    //
    // Parameters for GUI_EASING_CUBIC_BEZIER / GUI_EASING_SPRING on the
    // `appear` curve. Zero-initialised; only read when appear_easing is
    // one of the parameterised kinds. See gui_easing enum comment for
    // parameter meanings.
    //
    float      appear_easing_params[4];

    // Per-state animation spec (lives on the :appear / :disappear /
    // future per-state slot, not the default slot). animation_duration_ms
    // > 0 means "the animator interpolates the per-property values in
    // this state slot to the matching values in the default slot over
    // this many milliseconds, using the given easing curve". Declared
    // in .style as `animation: <ms> [easing];`.
    //
    // If a :appear block sets values but omits the animation declaration,
    // the animator falls back to a sensible default (300 ms ease_out)
    // -- the user just needs `:appear { bg: ...; }` to get a working pop.
    float      animation_duration_ms;
    gui_easing animation_easing;
    float      animation_easing_params[4];

    //
    // Generic property transition (state-change interpolation). Applied
    // by the animator when any animated property changes its resolved
    // value between frames (e.g. hover -> default). Distinct from the
    // :appear / :disappear mechanism above: those play a one-shot
    // entrance/exit, `transition` smooths every subsequent delta.
    //
    // Declared in .style as `transition: <prop> <ms> [easing]`.
    // `<prop>` is currently `all` only; named properties are a future
    // refinement. Zero duration disables the feature (default).
    //
    float      transition_duration_ms;
    gui_easing transition_easing;
    float      transition_easing_params[4];

    //
    // ===== visibility + display ============================================
    //
    // Matches CSS semantics one-for-one:
    //   visibility: hidden  -> node is painted INVISIBLE but still
    //                          occupies its layout bounds (siblings
    //                          don't reflow).
    //   display: none       -> node disappears from layout AND paint;
    //                          siblings collapse into the space it
    //                          would have taken.
    // Neither cascades (the resolver doesn't walk these from parent
    // to child). A hidden parent with a visible child produces a
    // visible child inside a blank container -- same as CSS when the
    // child explicitly sets `visibility: visible`. To hide a whole
    // subtree, use display: none on the parent (it short-circuits
    // traversal).
    //
    // The animator treats transitions on either property as equivalent
    // to "the node just entered the tree" -- appear_age_ms is reset
    // to 0 on hidden->visible or none->block, so :appear replays.
    //
    gui_visibility visibility;
    gui_display    display;

    //
    // Absolute positioning. position: absolute pulls the child out
    // of the parent's column/row flow; the insets (top/right/bottom/
    // left in px, *_pct in % of the parent's content rect) anchor
    // it. Pixel insets win over % insets if both are set on the
    // same edge (mirrors how size_w wins over width_pct).
    //
    // Mixing two opposing edges (e.g. left=0 + right=0) lets the
    // child stretch between them; in that case width is derived
    // (parent_w - left - right) and any explicit width is ignored.
    // Same on the vertical axis.
    //
    // Defaults: position=STATIC, all insets=0. position=ABSOLUTE
    // with no insets pins the child at (0,0) of the parent's
    // content rect with whatever size its width/height fields say.
    //
    gui_position position;
    float        inset_t;
    float        inset_r;
    float        inset_b;
    float        inset_l;
    float        inset_t_pct;
    float        inset_r_pct;
    float        inset_b_pct;
    float        inset_l_pct;

    //
    // ===== scrolling =======================================================
    //
    // scroll_smooth_ms: if > 0, wheel / thumb-drag updates on a
    // scrollable container write into scroll_y_target and the animator
    // lerps scroll_y toward scroll_y_target over this time constant
    // (exponential decay, so the first 63 % of the distance is covered
    // in scroll_smooth_ms milliseconds; tail asymptotes toward the
    // target). 0 = instant -- scroll_y follows user input with no
    // animation (same behaviour as before this field existed).
    //
    float scroll_smooth_ms;

    //
    // ===== box-shadow ========================================================
    //
    // Drawn BEHIND the node's bg rect. Matches the CSS four-value form:
    //   box-shadow: <dx> <dy> <blur> <color>;
    //
    // Shadow footprint = bg rect, offset by (dx, dy) in logical pixels,
    // with a `blur`-wide soft edge (via SDF falloff in the fragment
    // shader). Uses the node's radius so rounded rects cast rounded
    // shadows. Not scaled by ui scale for now -- values read in raw px.
    //
    float     shadow_dx;
    float     shadow_dy;
    float     shadow_blur;
    gui_color shadow_color;
    boole     has_shadow;

    //
    // ===== opacity ===========================================================
    //
    // Multiplied into every descendant's (and this node's) color alphas
    // at emit-time. Cascades through the tree via a runtime effective
    // opacity accumulator, NOT via style inheritance -- so a parent at
    // 0.5 and a child at 0.5 compose to 0.25, the CSS way.
    //
    // 1.0 = fully opaque (default); 0.0 = invisible (descendants aren't
    // culled though -- they still lay out and hit-test, same as CSS).
    //
    // Stored as `has_opacity` + value because zero has a semantic
    // meaning (fully transparent) distinct from "not set".
    //
    float opacity;
    boole has_opacity;

    //
    // ===== z-index ===========================================================
    //
    // Sibling-only stacking order. At emit time, scene sorts a parent's
    // children by (z_index, original order) and draws them in that
    // sequence. Greater z_index draws on top. Does NOT reorder across
    // ancestor boundaries the way CSS stacking contexts do -- siblings
    // only, which covers the cases people actually hit.
    //
    // Default 0. Negative values are allowed and draw below z = 0.
    //
    int z_index;

    //
    // ===== blur ==============================================================
    //
    // Applies to this node as a visual softening pass. Currently
    // approximated by submitting a translucent darken over the bg
    // (cheap, O(1), no FBO). True separable-Gaussian blur-of-contents
    // is a bigger port across the four renderer backends and sits on
    // the deferred list.
    //
    // 0.0 = no blur (default). Positive = blur radius in logical pixels.
    //
    float blur_px;

    //
    // ===== background gradient ===============================================
    //
    // Two-color linear gradient painted in place of `background-color`
    // when set. Resolver decides which to emit: has_bg_gradient
    // wins over has_background_color.
    // Direction is one of gui_gradient_dir; alpha in either endpoint
    // works (a transparent endpoint makes a fade-to-nothing effect).
    //
    gui_color        bg_gradient_from;
    gui_color        bg_gradient_to;
    gui_gradient_dir bg_gradient_dir;
    boole            has_bg_gradient;

    //
    // ===== child alignment ==================================================
    //
    // horizontal-alignment: left | center | right   (default left)
    // vertical-alignment:   top  | center | bottom  (default top)
    //
    // Semantics depend on the container's layout direction:
    //   <column> / <div> / <window>:
    //     horizontal-alignment  -> each child's x inside parent's inner width
    //     vertical-alignment    -> the whole stack's y inside parent's inner height
    //   <row>:
    //     horizontal-alignment  -> the whole row's x inside parent's inner width
    //     vertical-alignment    -> each child's y inside parent's inner height
    //
    // Zero values = defaults (left / top), so a style that never sets
    // alignment keeps today's top-left flow.
    //
    gui_halign halign;
    gui_valign valign;

    //
    // ===== image fitting =====================================================
    //
    // background-image: <path>         paints an image as the container's
    //                                  bg (via scene__emit_default_bg).
    //                                  fitted according to background-size.
    // background-size:  fit-keyword    one of fill / stretch / contain /
    //                                  cover / none. default FILL.
    // object-fit:       fit-keyword    <image> widget's own fit behaviour.
    //                                  default FILL (= pre-property stretch).
    //
    char          background_image[128];
    boole         has_background_image;
    gui_image_fit background_size;
    gui_image_fit object_fit;

    //
    // ===== collection layout =================================================
    //
    // Only read by widget_collection. Parser accepts `columns: N`,
    // `item-width: N`, `item-height: N`, `layout: grid/list/flow`.
    //
    gui_collection_layout collection_layout;
    int                   collection_columns;
    float                 item_width;
    float                 item_height;

    //
    // scroll_fade_px: width of the top-and-bottom soft-fade region
    // inside a scrollable container. Declared on the container (same
    // rule that carries overflow_y: auto / scroll) and applies to its
    // descendants: the animator multiplies a child's alpha by a ramp
    // that goes from 0 at the container's inner edge to 1 once the
    // child sits scroll_fade_px inside the visible area. Lets content
    // smoothly appear and disappear as it's scrolled into / out of
    // view instead of getting hard-clipped by the scissor at the
    // viewport edge. 0 = no fade (default; same behaviour as before
    // this field landed).
    //
    float scroll_fade_px;
} gui_style;

/**
 *retained-tree node. first-child / next-sibling representation so that
 *tree walks are a simple linked-list traversal.
 */
typedef struct gui_node gui_node;
struct gui_node
{
    gui_node_type  type;
    char           id[64];
    char           klass[64];
    gui_style      style[GUI_STATE_COUNT];
    gui_style      resolved;
    gui_node_state state;
    gui_rect       bounds; // bounds in pixels, top-left origin. filled by the layout pass.
    uint           on_click_hash; // fnv-1a hash of on_click_name. 0 = no handler.
    char           on_click_name[64]; // original handler name from the .ui file. used as the GetProcAddress fallback when the registration table misses.
    uint           on_change_hash; // fnv-1a hash of on_change_name. 0 = no handler.
    char           on_change_name[64]; // original handler name from the .ui file (slider on_change attribute).
    float          value;     // generic scalar value held by the node. used by Slider; 0 for other kinds.
    float          value_min; // lower bound of `value` (slider only). 0 for other kinds.
    float          value_max; // upper bound of `value` (slider only). 0 for other kinds (range collapses, GUI_NODE_SLIDER defaults to 1).
    char           text[256]; // text content held by the node. used by Text (Label), Button, Input. set via the `text` attribute in .ui.
    int64          text_len;  // current byte length of text (for Input where text mutates via keystrokes).
    gui_node*      parent;
    gui_node*      first_child;
    gui_node*      last_child;
    gui_node*      next_sibling;
    int64          child_count;

    //
    // ===== scrollable-container state ========================================
    //
    // populated at layout time for nodes that opt into scrolling via
    // overflow_x / overflow_y. persisted across frames so the scroll
    // position survives layout re-runs (layout is rebuilt every frame
    // in the PoC).
    //
    // scroll_x / scroll_y     current scroll offset in pixels. children
    //                         are drawn at child_y - scroll_y.
    // content_w / content_h   total extent of children (tight bounding
    //                         box of what the div "could" show without
    //                         a scrollbar). measured each layout pass.
    // scroll_drag_axis        which axis (if any) is currently being
    //                         dragged by the scrollbar thumb. 0 = none,
    //                         1 = y-axis, 2 = x-axis (reserved).
    // scroll_drag_mouse_start mouse coordinate (y or x, matching axis)
    //                         captured at mouse-down on the thumb. used
    //                         to convert subsequent drag positions into
    //                         a scroll delta.
    // scroll_drag_scroll_start scroll_x/_y value at the start of the
    //                         drag; drag delta is added to this to
    //                         compute the new scroll offset.
    //
    float scroll_x;
    float scroll_y;
    //
    // scroll_{x,y}_target: the desired scroll position. Wheel input and
    // thumb drag both write here; the animator then lerps scroll_{x,y}
    // toward this value over resolved.scroll_smooth_ms. When
    // scroll_smooth_ms is 0, scroll_{x,y} is snapped to
    // scroll_{x,y}_target immediately so the behaviour is identical to
    // pre-smooth-scroll code. Initialized to 0 by calloc, same as the
    // scroll_* pair above.
    //
    float scroll_x_target;
    float scroll_y_target;
    float content_w;
    float content_h;
    int64 scroll_drag_axis;           // 0 = not dragging, 1 = y, 2 = x.
    float scroll_drag_mouse_start;
    float scroll_drag_scroll_start;

    //
    // ===== animator state ===================================================
    //
    // appear_age_ms: milliseconds the node has been in the tree. The
    // animator increments this each frame in animator__tick. When
    // appear_age_ms < resolved.appear_ms, the animator multiplies the
    // node's resolved color alphas by an eased fade factor that ramps
    // from 0 at age=0 to 1 at age=appear_ms. Initialized to 0 in
    // scene__node_new (calloc), which is exactly what we want -- a
    // freshly-created node starts invisible and fades in.
    //
    // prev_visibility / prev_display: the resolved values from the
    // PREVIOUS frame. animator__tick compares the current resolved
    // values against these; when a node transitions out of "hidden"
    // (either direction: GUI_VISIBILITY_HIDDEN -> _VISIBLE or
    // GUI_DISPLAY_NONE -> _BLOCK), appear_age_ms is reset to 0 so the
    // :appear animation plays again. While a node is hidden / none,
    // appear_age_ms is not advanced, so the timer doesn't "use up"
    // the animation while the node is out of view. Initialized to 0
    // (= GUI_VISIBILITY_VISIBLE / GUI_DISPLAY_BLOCK) by calloc so a
    // node that starts hidden doesn't spuriously replay on frame 1.
    //
    float          appear_age_ms;
    gui_visibility prev_visibility;
    gui_display    prev_display;

    //
    // Transition-snapshot state. Populated by the animator when the
    // `transition:` property is active on the resolved style.
    //
    //   transition_age_ms: counts up from 0 each time any tracked
    //     property changes its resolved target. Idle when it reaches
    //     the transition duration -- no interpolation contribution in
    //     that case.
    //
    //   transition_from_*: the DISPLAYED value at the moment the most
    //     recent change started. Cached so an interpolation that itself
    //     gets interrupted by another change produces a smooth chain
    //     (new start = current displayed, not the old target).
    //
    //   transition_prev_target_*: the resolver's OUTPUT last frame.
    //     Compared against this frame's resolver output to detect a
    //     "target changed" moment.
    //
    //   transition_seen: 0 = fields are uninitialised (first frame,
    //     nothing to compare against), 1 = populated. Set to 1 after
    //     the first capture pass.
    //
    float     transition_age_ms;
    gui_color transition_from_bg;
    gui_color transition_from_fg;
    gui_color transition_from_text;
    float     transition_from_radius;
    gui_color transition_from_bg_grad_from;
    gui_color transition_from_bg_grad_to;
    float     transition_from_size_w;
    float     transition_from_size_h;
    float     transition_from_font_size;
    boole     transition_from_size_w_explicit;
    boole     transition_from_size_h_explicit;
    boole     transition_from_font_size_explicit;
    gui_color transition_prev_target_bg;
    gui_color transition_prev_target_fg;
    gui_color transition_prev_target_text;
    float     transition_prev_target_radius;
    gui_color transition_prev_target_bg_grad_from;
    gui_color transition_prev_target_bg_grad_to;
    float     transition_prev_target_size_w;
    float     transition_prev_target_size_h;
    float     transition_prev_target_font_size;
    boole     transition_prev_target_size_w_explicit;
    boole     transition_prev_target_size_h_explicit;
    boole     transition_prev_target_font_size_explicit;
    boole     transition_seen;

    //
    // disappear_remaining_ms: counts down from the :disappear slot's
    // duration to zero while the node plays its outro. Triggered on the
    // was-shown -> not-shown edge (visibility: visible -> hidden or
    // display: block -> none). While non-zero, the animator overrides
    // the resolved visibility/display back to visible so the node keeps
    // rendering while its values interpolate toward the :disappear
    // slot's override values.
    //
    // Zero-initialised (via calloc) matches the intended idle state:
    // freshly-created nodes that are born hidden don't spuriously play
    // a disappear animation -- only the shown-to-hidden edge sets the
    // counter to the slot's full duration, and each tick it drains
    // back down.
    //
    float     disappear_remaining_ms;

    //
    // ===== generic widget state =============================================
    //
    // is_open: used by widgets that have a "popped open" state distinct
    // from the standard hover/pressed/disabled states. <select> sets
    // this when its dropdown menu is showing; the popup draws via the
    // scene overlay mechanism (see scene__set_overlay). 0 = closed,
    // 1 = open. Other widgets (modal dialogs, accordions, tooltips...)
    // can repurpose the same flag.
    //
    // value (also used by Slider) is the additional payload: for
    // <checkbox> it's 0/1 (unchecked/checked), for <radio> it's 0/1
    // (deselected/selected), for <select> it's the index of the
    // currently selected <option> child.
    //
    boole is_open;

    //
    // Per-widget opaque pointer. Holds widget-specific state that
    // doesn't fit any of the named fields above (e.g. widget_image
    // stashes its decoded texture + dimensions here). Allocated by
    // the widget's init_defaults / apply_attribute hook; released by
    // its on_destroy hook so scene__node_free doesn't leak.
    //
    void* user_data;

    //
    // Effective opacity (runtime-only). scene__emit_draws writes
    // this before dispatching to the widget's emit_draws so the
    // widget can multiply it into every color alpha it submits.
    // Computed as parent.effective_opacity * resolved.opacity (or
    // 1.0 when resolved.has_opacity is FALSE). Not a style field --
    // scene owns it.
    //
    float effective_opacity;

    //
    // Host-side color overrides. Set via
    // scene__set_background_color_override / scene__set_font_color_override
    // etc. from handler code; scene applies them AFTER rule overlay
    // + animator each frame, so they survive style re-resolve
    // without the host having to re-register rules or mutate
    // style[] (which the resolver's memset would clobber). Typical
    // use: theme toggles, quick swatch previews, editor drag-to-
    // recolor. Clear with has_*_override = FALSE.
    //
    gui_color background_color_override;
    boole     has_background_color_override;
    gui_color font_color_override;
    boole     has_font_color_override;
};

/**
 *event kinds delivered to handlers. extends as new event types are added.
 */
typedef enum gui_event_type
{
    GUI_EVENT_CLICK,
    GUI_EVENT_CHANGE
} gui_event_type;

/**
 *one event delivered to a handler. the anonymous union body holds
 *payload that varies by type. ev.mouse.* for click events,
 *ev.change.scalar for slider on_change.
 */
typedef struct gui_event
{
    gui_event_type type;
    gui_node*      sender;
    union
    {
        struct
        {
            int64 x;
            int64 y;
            int64 button;
        } mouse;
        struct
        {
            float scalar;
        } change;
        //
        // change.color: RGBA 0..1 payload for colorpicker on_change events.
        // Distinct from `change.scalar` so a single handler can read either
        // without guessing; the event type + sender's node type disambiguate.
        //
        struct
        {
            gui_color value;
        } color;
        //
        // popup dismiss payload. `confirmed` = TRUE when the user clicked
        // OK / Yes, FALSE for Cancel / No. For type="option-select" the
        // popup also writes the selected option index into `index`.
        //
        struct
        {
            boole confirmed;
            int64 index;
        } popup;
    };
} gui_event;

typedef void (*gui_handler_fn)(gui_event* ev);

#endif
