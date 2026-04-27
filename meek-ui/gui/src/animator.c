//
// animator.c - per-frame animation engine.
//
// The animator runs ONCE per frame, AFTER scene__resolve_styles has
// written resolved.* from the per-state style slot, and BEFORE
// scene__emit_draws reads resolved. It mutates resolved.* in place to
// produce time-varying effects.
//
// Time source: scene__frame_delta_ms, set by the platform layer at the
// top of each tick via scene__begin_frame_time(now_ms). Each platform
// converts its native monotonic clock (QueryPerformanceCounter on
// Windows, clock_gettime on Android, mach_absolute_time on macOS) to
// milliseconds inside platform__tick. The animator stays OS-agnostic.
//
// SUPPORTED ANIMATIONS
// --------------------
// Two appear-animation modes; both check appear_age_ms to drive
// progress, and both can coexist on the same node (the per-property
// pseudo-state mode wins on properties it covers; the shortcut mode
// applies as a generic alpha fade for whatever the pseudo-state mode
// didn't specifically interpolate).
//
//   1. SHORTCUT (appear: <ms> <easing>)
//      Stored as resolved.appear_ms / resolved.appear_easing.
//      Effect: every visible color's alpha (bg.a, fg.a, color.a) is
//      multiplied by the eased fade factor. Reads as "everything fades
//      in from invisible".
//
//   2. PSEUDO-STATE (:appear { property: value; ... animation: <ms>; })
//      Stored as style[GUI_STATE_APPEAR] (full per-property values
//      plus animation_duration_ms / animation_easing on that slot).
//      Effect: each property the :appear block populated (has_bg,
//      has_color, has_fg) is interpolated FROM the :appear value TO
//      the value the resolver computed for the node's current state.
//      Numeric properties (radius, pad, gap) likewise interpolate
//      when set in the :appear slot.
//
// LIFECYCLE
// ---------
// appear_age_ms starts at 0 (calloc) when scene__node_new allocates a
// node. Animator increments it by scene__frame_delta_ms each frame
// until it reaches the animation duration; after that the animation
// is idle and resolved.* shows the steady-state values.
//
// On a hot-reload, the entire tree is freed and rebuilt, so all nodes
// start at age 0 again -- the page replays its appear animation. This
// is intentional: it gives instant visual confirmation that an edit
// landed, and it's how the user can re-trigger the animation while
// iterating on its look.
//
// :disappear is parsed but the animator skips it for now -- needs a
// deferred-destruction mechanism so a removed node sticks around long
// enough to play the outro.
//

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "animator.h"
#include "clib/stdlib.h"
#include "third_party/log.h"
#include "debug_definitions.h"

//
// Default duration / easing the animator uses when a :appear block
// is populated with property values but doesn't include an explicit
// `animation: <ms> <easing>` declaration. 300 ms with ease_out is
// the standard "snappy pop" most UIs settle on.
//
#define _ANIMATOR_INTERNAL__DEFAULT_DURATION_MS 300.0f
#define _ANIMATOR_INTERNAL__DEFAULT_EASING      GUI_EASING_EASE_OUT

//
// Cubic Bezier evaluator: for a Bezier-based easing curve, animation time
// t is the X coordinate and we want the Y value where the curve crosses
// that X. Root-find t_bezier such that bezier_x(t_bezier) = t, then return
// bezier_y(t_bezier). CSS's cubic-bezier uses control points (x1, y1) and
// (x2, y2) on the unit square, with the endpoints fixed at (0,0)/(1,1).
//
// The X coordinate is monotonic in t when x1, x2 are in [0, 1] (the only
// case CSS allows), so a handful of Newton-Raphson iterations converge to
// machine precision. We bail out after 8 iterations (far more than needed)
// and fall back to linear interpolation over t if somehow we don't reach
// the tolerance -- shouldn't happen with well-formed control points.
//
static float _animator_internal__bezier_coord(float p1, float p2, float u)
{
    //
    // Bernstein form: B(u) = 3*(1-u)^2*u*p1 + 3*(1-u)*u^2*p2 + u^3,
    // with the endpoint 0 (for P0) and 1 (for P3) folded in. Valid for
    // either X or Y of a cubic Bezier on the unit square.
    //
    float omu = 1.0f - u;
    return 3.0f * omu * omu * u * p1 + 3.0f * omu * u * u * p2 + u * u * u;
}

static float _animator_internal__bezier_coord_deriv(float p1, float p2, float u)
{
    //
    // dB/du = 3*(1-u)^2*p1 + 6*(1-u)*u*(p2 - p1) + 3*u^2*(1 - p2).
    //
    float omu = 1.0f - u;
    return 3.0f * omu * omu * p1 + 6.0f * omu * u * (p2 - p1) + 3.0f * u * u * (1.0f - p2);
}

static float _animator_internal__cubic_bezier(float x1, float y1, float x2, float y2, float t)
{
    //
    // Newton-Raphson: start guess u = t (a decent starting point since
    // the curve is close-to-linear for most UI tuning values), then
    // correct by (B_x(u) - t) / B_x'(u) until |B_x(u) - t| < tolerance.
    //
    float u = t;
    for (int i = 0; i < 8; i++)
    {
        float x = _animator_internal__bezier_coord(x1, x2, u);
        float dx = _animator_internal__bezier_coord_deriv(x1, x2, u);
        float err = x - t;
        if (err > -1e-5f && err < 1e-5f) { break; }
        if (dx > -1e-6f && dx < 1e-6f)   { break; }  // avoid div-by-zero at tangent cliffs.
        u -= err / dx;
        if (u < 0.0f) { u = 0.0f; }
        if (u > 1.0f) { u = 1.0f; }
    }
    return _animator_internal__bezier_coord(y1, y2, u);
}

//
// Damped-spring easing. Parameters:
//   stiffness: oscillation frequency (radians over normalized [0,1]).
//              Higher = more visible wobbles. 20 ~ 3 cycles, 30 ~ 5.
//   damping:   exponential envelope decay. Higher = faster settle.
//              3 gives a floaty oscillation, 10 a crisp snap.
//
// Formula: x(t) = 1 - exp(-damping * t) * cos(stiffness * t). At t=0
// both cos and exp are 1, so x=0. At t=1 the envelope exp(-damping) is
// small for any reasonable damping, so x converges near 1 (possibly
// after overshoots). We do NOT clamp the return to [0, 1] -- the
// overshoot IS the spring character that makes it visible.
//
static float _animator_internal__spring(float stiffness, float damping, float t)
{
    //
    // Defensive defaults for zero-inited params (appear_easing_params
    // is calloc'd, so if the parser didn't hit spring(...) the zero
    // values would produce a degenerate constant). A user who wrote
    // `spring(0, 0)` gets linear; anyone who picks non-zero wins.
    //
    if (stiffness <= 0.0f) { stiffness = 20.0f; }
    if (damping   <= 0.0f) { damping   = 5.0f;  }
    float envelope = stdlib__expf(-damping * t);
    float wave     = stdlib__cosf(stiffness * t);
    return 1.0f - envelope * wave;
}

//
// Eased(t): map normalized progress t in [0,1] -> eased value in [0,1]
// (or slightly outside [0,1] for back/spring curves that overshoot).
// Defensive clamp on t to handle animator overshoot from a single large
// frame_delta. Params are read for kinds that need them (CUBIC_BEZIER,
// SPRING); ignored otherwise.
//
static float _animator_internal__ease(gui_easing kind, const float* params, float t)
{
    if (t < 0.0f) { t = 0.0f; }
    if (t > 1.0f) { t = 1.0f; }

    switch (kind)
    {
        case GUI_EASING_LINEAR:
        {
            return t;
        }
        case GUI_EASING_EASE_IN:
        {
            //
            // Quadratic ease-in: zero starting velocity, accelerating.
            //
            return t * t;
        }
        case GUI_EASING_EASE_OUT:
        {
            //
            // Quadratic ease-out: starts fast, gently decelerates.
            // Most natural "pop" curve -- elements zoom in and land.
            //
            float u = 1.0f - t;
            return 1.0f - u * u;
        }
        case GUI_EASING_EASE_IN_OUT:
        {
            //
            // Cubic ease-in-out: smooth at both ends, fast through
            // the middle. The "object slides between two resting
            // positions" curve.
            //
            if (t < 0.5f)
            {
                return 4.0f * t * t * t;
            }
            float u = -2.0f * t + 2.0f;
            return 1.0f - (u * u * u) * 0.5f;
        }
        case GUI_EASING_EASE:
        {
            //
            // CSS's default `ease` keyword, equivalent to
            // cubic-bezier(0.25, 0.1, 0.25, 1.0). Starts moderately
            // fast, decelerates through the middle, glides to rest.
            //
            return _animator_internal__cubic_bezier(0.25f, 0.1f, 0.25f, 1.0f, t);
        }
        case GUI_EASING_EASE_IN_BACK:
        {
            //
            // Robert Penner's back ease-in. Dips below 0 near the start
            // (overshoot in the anticipation direction) before zooming
            // forward. c1 = 1.70158 is the standard "slight" overshoot.
            //
            const float c1 = 1.70158f;
            const float c3 = c1 + 1.0f;
            return c3 * t * t * t - c1 * t * t;
        }
        case GUI_EASING_EASE_OUT_BACK:
        {
            //
            // Overshoots past 1 near the end and eases back down to 1.
            // Good for panel pops where you want a "landing with a
            // little bounce" feel.
            //
            const float c1 = 1.70158f;
            const float c3 = c1 + 1.0f;
            float u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        }
        case GUI_EASING_EASE_IN_OUT_BACK:
        {
            //
            // Anticipates below 0 and overshoots above 1 symmetrically.
            //
            const float c1 = 1.70158f;
            const float c2 = c1 * 1.525f;
            if (t < 0.5f)
            {
                float u = 2.0f * t;
                return (u * u * ((c2 + 1.0f) * u - c2)) * 0.5f;
            }
            float u = 2.0f * t - 2.0f;
            return (u * u * ((c2 + 1.0f) * u + c2) + 2.0f) * 0.5f;
        }
        case GUI_EASING_EASE_OUT_BOUNCE:
        {
            //
            // Standard bounce: 4-segment piecewise quadratic. Each
            // segment is the motion of a ball bouncing with diminishing
            // energy -- first big hop, then smaller, smaller, dribble.
            // Constants n1=7.5625 and d1=2.75 come from the classic
            // Penner formulation.
            //
            const float n1 = 7.5625f;
            const float d1 = 2.75f;
            if (t < 1.0f / d1)
            {
                return n1 * t * t;
            }
            else if (t < 2.0f / d1)
            {
                float u = t - 1.5f / d1;
                return n1 * u * u + 0.75f;
            }
            else if (t < 2.5f / d1)
            {
                float u = t - 2.25f / d1;
                return n1 * u * u + 0.9375f;
            }
            else
            {
                float u = t - 2.625f / d1;
                return n1 * u * u + 0.984375f;
            }
        }
        case GUI_EASING_CUBIC_BEZIER:
        {
            //
            // params = { x1, y1, x2, y2 }. Parser checks they parse;
            // does not clamp to [0, 1] so the user can request CSS-
            // invalid curves if they want them.
            //
            return _animator_internal__cubic_bezier(params[0], params[1], params[2], params[3], t);
        }
        case GUI_EASING_SPRING:
        {
            //
            // params = { stiffness, damping, _, _ }.
            //
            return _animator_internal__spring(params[0], params[1], t);
        }
    }
    return t;
}

//
// Linear interpolation for a single channel. Used componentwise for
// colors, and directly for scalar properties (radius, pad, gap).
//
//
// Runtime toggle for the size_w / size_h / font_size transition
// extension. Default OFF so the animator behaves identically to
// before the extension was added until a host explicitly opts in.
// meek-shell flips it on once it's ready to drive layout-affecting
// transitions; the test harness flips it on/off to reproduce + bisect
// the layout-corruption bug. Public setter / getter at the bottom of
// the file.
//
static boole _animator_internal__size_transitions_enabled = FALSE;

void animator__set_size_transitions_enabled(boole on)
{
    _animator_internal__size_transitions_enabled = on;
    log_info("[dbg-anim] size_transitions %s", on ? "ENABLED" : "disabled");
}

boole animator__size_transitions_enabled(void)
{
    return _animator_internal__size_transitions_enabled;
}

static float _animator_internal__lerp_f(float a, float b, float t)
{
    return a + (b - a) * t;
}

//
// Color lerp: per-channel linear interpolation. RGB channels are in
// linear 0..1 space (see renderer.h's VISUAL CONTRACT) so a plain
// linear blend is correct -- no sRGB conversion needed.
//
static gui_color _animator_internal__lerp_color(gui_color a, gui_color b, float t)
{
    gui_color c;
    c.r = _animator_internal__lerp_f(a.r, b.r, t);
    c.g = _animator_internal__lerp_f(a.g, b.g, t);
    c.b = _animator_internal__lerp_f(a.b, b.b, t);
    c.a = _animator_internal__lerp_f(a.a, b.a, t);
    return c;
}

//
//compute the scroll-edge fade factor for `n` relative to its
//nearest scrollable ancestor `fade_ancestor`. returns 1.0 when the
//node sits comfortably inside the viewport (at least fade_px from
//both top and bottom edges), ramps down to 0.0 as the node
//approaches either edge, and stays at 0.0 once the node has moved
//fully past the edge (though scissor will have already clipped it
//at that point -- this branch matters mostly for the ramp region).
//
//the math is purely geometric: take the node's top Y versus the
//ancestor's top Y for the top ramp, and the node's bottom Y versus
//the ancestor's bottom Y for the bottom ramp. min of the two wins
//so a node straddling the middle of the viewport gets the full
//alpha but one approaching either edge fades.
//
static float _animator_internal__scroll_fade_alpha(gui_node* n, gui_node* fade_ancestor)
{
    if (fade_ancestor == NULL)
    {
        return 1.0f;
    }
    float fade_px = fade_ancestor->resolved.scroll_fade_px;
    if (fade_px <= 0.0f)
    {
        return 1.0f;
    }

    //
    //ancestor's visible rect (scroll-free; scroll_y has already been
    //baked into its children's bounds by div/window layout).
    //
    float top_edge = fade_ancestor->bounds.y;
    float bot_edge = fade_ancestor->bounds.y + fade_ancestor->bounds.h;

    //
    //this node's vertical extent.
    //
    float cy = n->bounds.y;
    float cb = n->bounds.y + n->bounds.h;

    float alpha = 1.0f;

    //
    //top ramp: how far below the ancestor's top edge is THIS node's
    //top? 0 = exactly at the edge, fade_px = just out of the ramp.
    //negative (node above the edge) means fully faded out.
    //
    float d_top = cy - top_edge;
    if (d_top < fade_px)
    {
        float a = d_top / fade_px;
        if (a < 0.0f) { a = 0.0f; }
        if (a > 1.0f) { a = 1.0f; }
        if (a < alpha) { alpha = a; }
    }

    //
    //bottom ramp: how far above the ancestor's bottom edge is THIS
    //node's bottom? same polarity as the top ramp.
    //
    float d_bot = bot_edge - cb;
    if (d_bot < fade_px)
    {
        float a = d_bot / fade_px;
        if (a < 0.0f) { a = 0.0f; }
        if (a > 1.0f) { a = 1.0f; }
        if (a < alpha) { alpha = a; }
    }

    return alpha;
}

//
// Visit one node: advance its appear timer, then apply both animation
// modes (pseudo-state property lerps + shortcut alpha fade) to the
// already-resolved style. Recurses into children.
//
//fade_ancestor tracks the nearest enclosing scrollable container
//that has scroll_fade_px > 0. NULL means no fade ancestor in scope;
//non-NULL means we should multiply this node's color alphas by an
//edge-proximity ramp (see _scroll_fade_alpha above). passed down
//through recursion so deeply nested nodes fade against their
//nearest scrollable viewport, not the outermost one.
//
static void _animator_internal__visit(gui_node* n, float ms_delta, gui_node* fade_ancestor)
{
    if (n == NULL) { return; }

    gui_style* r = &n->resolved;
    gui_style* a = &n->style[GUI_STATE_APPEAR];

    //
    // ===== visibility / display transitions =============================
    //
    // A node counts as "shown" iff its resolved visibility is VISIBLE
    // AND its resolved display is BLOCK. Any other state (hidden or
    // none on either axis) means the node shouldn't be accumulating
    // appear_age_ms -- and when it transitions back to "shown", the
    // timer needs to reset to 0 so :appear replays from scratch, just
    // like the node was freshly inserted into the tree.
    //
    // Without the reset: the animator would advance appear_age_ms
    // normally even while the node was hidden (since its DEFAULT rule
    // might still have appear_ms > 0), and by the time the node became
    // visible the animation would already be "done" and the node would
    // pop into its steady state without an intro. That's visibly wrong
    // -- the whole point of these properties is to toggle UIs that
    // look as lively as initial load.
    //
    // Without the freeze (just the reset): on each show we'd restart
    // cleanly, but the animator still writes to alpha during hidden
    // frames, which is harmless but wasted work, and leaves subtle
    // per-node state fluctuations that make debugging hard.
    //
    boole was_shown = (n->prev_visibility != GUI_VISIBILITY_HIDDEN &&
                      n->prev_display    != GUI_DISPLAY_NONE);
    boole is_shown  = (r->visibility      != GUI_VISIBILITY_HIDDEN &&
                      r->display          != GUI_DISPLAY_NONE);

    if (!was_shown && is_shown)
    {
        DBG_ANIM log_info("[dbg-anim] appear RESET on node=%p type=%d (%s -> shown)",
                          (void*)n, (int)n->type,
                          (n->prev_display == GUI_DISPLAY_NONE) ? "display:none"
                            : (n->prev_visibility == GUI_VISIBILITY_HIDDEN) ? "visibility:hidden"
                            : "other");
        //
        // Transition "not shown" -> "shown": replay the :appear
        // animation from the beginning. Whether this is the node's
        // FIRST show (prev_* were initialized to 0 = visible/block by
        // calloc, but the node's default slot started hidden so
        // resolve_styles produced a hidden resolved style before this
        // point) or a subsequent show toggled by the handler, the
        // desired behaviour is the same: a full :appear play.
        //
        n->appear_age_ms = 0.0f;
        //
        // Show cancels any in-flight disappear. If the user toggles
        // visibility back to visible while the outro is playing, we
        // abandon the outro entirely and restart :appear.
        //
        n->disappear_remaining_ms = 0.0f;
    }

    //
    // Transition "shown" -> "not shown": start :disappear if the node
    // has either property overrides on the :disappear slot or an
    // animation declaration there. If neither, the node just hides
    // instantly like it always did.
    //
    gui_style* d = &n->style[GUI_STATE_DISAPPEAR];
    boole has_disappear_content = (boole)(d->has_background_color ||
                                          d->has_accent_color     ||
                                          d->has_font_color       ||
                                          d->radius > 0.0f        ||
                                          d->animation_duration_ms > 0.0f);
    float disappear_duration = d->animation_duration_ms > 0.0f ? d->animation_duration_ms : _ANIMATOR_INTERNAL__DEFAULT_DURATION_MS;

    if (was_shown && !is_shown && has_disappear_content)
    {
        n->disappear_remaining_ms = disappear_duration;
    }

    //
    // Snapshot the current resolved visibility/display for next frame's
    // transition detection. Captured from the UNMODIFIED resolver
    // output (before any disappear override below) so the next frame's
    // `was_shown` reflects the user's intent, not our internal hold.
    //
    n->prev_visibility = r->visibility;
    n->prev_display    = r->display;

    //
    // If the node is mid-disappear, override the resolved visibility /
    // display back to "shown" so the rest of this tick (and the emit
    // pass this frame) treats the node as still present. The overridden
    // values also power the interpolation block a few lines below.
    //
    boole disappear_playing = (n->disappear_remaining_ms > 0.0f);
    if (disappear_playing)
    {
        r->visibility = GUI_VISIBILITY_VISIBLE;
        r->display    = GUI_DISPLAY_BLOCK;
    }

    //
    // display:none short-circuits the whole subtree. children are
    // logically absent -- no layout, no draw, and no animation ticks.
    // When the parent's display flips back to block, children's
    // frozen appear_age_ms values will trigger their own hidden->shown
    // replays as they walk through this same detection on the next
    // frame.
    //
    if (r->display == GUI_DISPLAY_NONE)
    {
        return;
    }

    //
    //compute the fade_ancestor that applies to THIS node's children.
    //if this node is itself a scrollable container with scroll_fade_px
    //set, it takes over as the fade context for descendants. otherwise
    //descendants inherit the same context we got.
    //
    //done BEFORE the visibility/hidden early-out below so that a
    //hidden scroll container still establishes the context for its
    //children (rare but legal: visibility:hidden on a container
    //should leave its contents visible, and those contents still
    //belong to the same viewport).
    //
    gui_node* child_fade_ancestor = fade_ancestor;
    {
        boole is_scroll = (boole)(r->overflow_y == GUI_OVERFLOW_SCROLL ||
                                  r->overflow_y == GUI_OVERFLOW_AUTO);
        if (is_scroll && r->scroll_fade_px > 0.0f)
        {
            child_fade_ancestor = n;
        }
    }

    //
    // visibility:hidden skips THIS node's animation tick but still
    // descends into children. Matches the "ghost container" semantic
    // used in emit_draws: the node itself is invisible, children
    // render (and tick) independently.
    //
    if (r->visibility == GUI_VISIBILITY_HIDDEN)
    {
        gui_node* c = n->first_child;
        while (c != NULL)
        {
            _animator_internal__visit(c, ms_delta, child_fade_ancestor);
            c = c->next_sibling;
        }
        return;
    }

    //
    //scroll-edge fade. applied FIRST so subsequent :appear and
    //shortcut alpha passes compose multiplicatively on top of the
    //geometry-driven fade. ancestor NULL => no fade context => no-op.
    //
    if (fade_ancestor != NULL)
    {
        float sf = _animator_internal__scroll_fade_alpha(n, fade_ancestor);
        if (sf < 1.0f)
        {
            r->background_color.a *= sf;
            r->accent_color.a     *= sf;
            r->font_color.a       *= sf;
        }
    }

    //
    // Effective duration: prefer the :appear block's own animation
    // declaration, fall back to the inherited `appear:` shortcut on
    // the resolved style, fall back further to a default if the
    // :appear block is populated but didn't declare a duration.
    //
    float        duration = 0.0f;
    gui_easing   easing   = GUI_EASING_LINEAR;
    const float* ease_params = NULL;
    static const float _zeros[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    boole has_pseudo = (boole)(a->has_background_color || a->has_accent_color || a->has_font_color || a->radius > 0.0f);

    if (a->animation_duration_ms > 0.0f)
    {
        duration    = a->animation_duration_ms;
        easing      = a->animation_easing;
        ease_params = a->animation_easing_params;
    }
    else if (has_pseudo)
    {
        //
        // :appear block had property values but no explicit animation
        // declaration. Use sensible default so the user can write a
        // minimal `:appear { bg: ...; }` and still get a working pop.
        //
        duration    = _ANIMATOR_INTERNAL__DEFAULT_DURATION_MS;
        easing      = _ANIMATOR_INTERNAL__DEFAULT_EASING;
        ease_params = _zeros;
    }
    else if (r->appear_ms > 0.0f)
    {
        duration    = r->appear_ms;
        easing      = r->appear_easing;
        ease_params = r->appear_easing_params;
    }

    if (duration > 0.0f)
    {
        n->appear_age_ms += ms_delta;
        if (n->appear_age_ms > duration) { n->appear_age_ms = duration; }

        if (n->appear_age_ms < duration)
        {
            float t     = n->appear_age_ms / duration;
            float eased = _animator_internal__ease(easing, ease_params, t);

            //
            // PSEUDO-STATE MODE: per-property interpolation.
            // For each property the :appear block populated, lerp
            // FROM the :appear value TO whatever the resolver wrote
            // (which reflects the current state, default/hover/etc).
            //
            // Properties NOT covered by :appear fall through to the
            // shortcut alpha-fade below.
            //
            if (a->has_background_color)
            {
                r->background_color = _animator_internal__lerp_color(a->background_color, r->background_color, eased);
            }
            if (a->has_accent_color)
            {
                r->accent_color = _animator_internal__lerp_color(a->accent_color, r->accent_color, eased);
            }
            if (a->has_font_color)
            {
                r->font_color = _animator_internal__lerp_color(a->font_color, r->font_color, eased);
            }
            if (a->radius > 0.0f)
            {
                r->radius = _animator_internal__lerp_f(a->radius, r->radius, eased);
            }
            //
            // Numeric layout properties (pad/gap/size) are NOT
            // interpolated here because the layout pass already ran
            // BEFORE the animator -- changing them now would desync
            // bounds from drawn pixels. They're listed as a follow-up.
            //

            //
            // SHORTCUT MODE: generic alpha fade. Only applies when
            // appear_ms is set on the resolved style AND the pseudo-
            // state slot didn't already cover that color.
            //
            if (r->appear_ms > 0.0f)
            {
                if (!a->has_background_color) { r->background_color.a *= eased; }
                if (!a->has_accent_color)     { r->accent_color.a     *= eased; }
                if (!a->has_font_color)       { r->font_color.a       *= eased; }
            }
        }
    }

    //
    // ===== transition =====================================================
    //
    // Generic state-change interpolation for animated properties. For
    // `transition: all 200 ease-out`, every change in background_color,
    // accent_color, font_color or radius between frames eases in over
    // 200 ms. Skipped while :appear is still in progress for this node
    // -- the entrance animation wins until it completes, after which
    // transitions govern subsequent state flips.
    //
    // Implementation strategy:
    //   (1) Detect change: compare this frame's resolver output against
    //       a saved `transition_prev_target_*` snapshot.
    //   (2) On change, snapshot the currently DISPLAYED value (the
    //       resolved field as it stood at this point) into
    //       `transition_from_*` so an in-flight interpolation that gets
    //       interrupted by another change picks up smoothly.
    //   (3) Reset `transition_age_ms = 0` on change; advance by
    //       `ms_delta` each frame; cap at duration (idle state).
    //   (4) Write `lerp(from, target, eased(age/duration))` back into
    //       the resolved field so emit_draws shows the interpolated
    //       value.
    //
    {
        boole appear_in_progress = (duration > 0.0f && n->appear_age_ms < duration);
        if (r->transition_duration_ms > 0.0f && !appear_in_progress)
        {
            //
            // Target values as the resolver produced them THIS frame.
            // After :appear has finished (or was absent), r->* == the
            // clean target, which is what we want to animate toward.
            //
            gui_color cur_bg                 = r->background_color;
            gui_color cur_fg                 = r->accent_color;
            gui_color cur_text               = r->font_color;
            float     cur_radius             = r->radius;
            gui_color cur_bg_grad_from       = r->bg_gradient_from;
            gui_color cur_bg_grad_to         = r->bg_gradient_to;
            float     cur_size_w             = r->size_w;
            float     cur_size_h             = r->size_h;
            float     cur_font_size          = r->font_size;
            boole     cur_size_w_explicit    = r->size_w_explicit;
            boole     cur_size_h_explicit    = r->size_h_explicit;
            boole     cur_font_size_explicit = r->font_size_explicit;

            if (!n->transition_seen)
            {
                //
                // First-time bootstrap: nothing to animate FROM, so
                // mark the fields seen and park age at duration (idle).
                // Subsequent frames will detect changes properly.
                //
                n->transition_prev_target_bg                 = cur_bg;
                n->transition_prev_target_fg                 = cur_fg;
                n->transition_prev_target_text               = cur_text;
                n->transition_prev_target_radius             = cur_radius;
                n->transition_prev_target_bg_grad_from       = cur_bg_grad_from;
                n->transition_prev_target_bg_grad_to         = cur_bg_grad_to;
                n->transition_prev_target_size_w             = cur_size_w;
                n->transition_prev_target_size_h             = cur_size_h;
                n->transition_prev_target_font_size          = cur_font_size;
                n->transition_prev_target_size_w_explicit    = cur_size_w_explicit;
                n->transition_prev_target_size_h_explicit    = cur_size_h_explicit;
                n->transition_prev_target_font_size_explicit = cur_font_size_explicit;
                n->transition_from_bg                        = cur_bg;
                n->transition_from_fg                        = cur_fg;
                n->transition_from_text                      = cur_text;
                n->transition_from_radius                    = cur_radius;
                n->transition_from_bg_grad_from              = cur_bg_grad_from;
                n->transition_from_bg_grad_to                = cur_bg_grad_to;
                n->transition_from_size_w                    = cur_size_w;
                n->transition_from_size_h                    = cur_size_h;
                n->transition_from_font_size                 = cur_font_size;
                n->transition_from_size_w_explicit           = cur_size_w_explicit;
                n->transition_from_size_h_explicit           = cur_size_h_explicit;
                n->transition_from_font_size_explicit        = cur_font_size_explicit;
                n->transition_age_ms                         = r->transition_duration_ms;
                n->transition_seen                           = TRUE;
                DBG_ANIM log_info("[dbg-anim] BOOTSTRAP node=%p type=%d sw=%.0f(expl=%d) sh=%.0f(expl=%d) fs=%.0f(expl=%d) dur=%.0f",
                                  (void*)n, (int)n->type,
                                  cur_size_w, cur_size_w_explicit,
                                  cur_size_h, cur_size_h_explicit,
                                  cur_font_size, cur_font_size_explicit,
                                  r->transition_duration_ms);
            }

            //
            // Detect any target change. Any of the four properties
            // moving triggers a reset; each property carries its own
            // `from` snapshot so no change is lost if two fire together.
            //
            //
            // Color/radius changes are detected unconditionally.
            // Size+font changes are folded into the change detection
            // ONLY when the runtime toggle is on -- otherwise a
            // transient resolver-output diff in size_w (e.g. during a
            // class-set re-evaluation that briefly sees the rule
            // unmatched) would falsely trigger a transition.
            //
            boole changed =
                (cur_bg.r              != n->transition_prev_target_bg.r            ||
                 cur_bg.g              != n->transition_prev_target_bg.g            ||
                 cur_bg.b              != n->transition_prev_target_bg.b            ||
                 cur_bg.a              != n->transition_prev_target_bg.a            ||
                 cur_fg.r              != n->transition_prev_target_fg.r            ||
                 cur_fg.g              != n->transition_prev_target_fg.g            ||
                 cur_fg.b              != n->transition_prev_target_fg.b            ||
                 cur_fg.a              != n->transition_prev_target_fg.a            ||
                 cur_text.r            != n->transition_prev_target_text.r          ||
                 cur_text.g            != n->transition_prev_target_text.g          ||
                 cur_text.b            != n->transition_prev_target_text.b          ||
                 cur_text.a            != n->transition_prev_target_text.a          ||
                 cur_radius            != n->transition_prev_target_radius          ||
                 cur_bg_grad_from.r    != n->transition_prev_target_bg_grad_from.r  ||
                 cur_bg_grad_from.g    != n->transition_prev_target_bg_grad_from.g  ||
                 cur_bg_grad_from.b    != n->transition_prev_target_bg_grad_from.b  ||
                 cur_bg_grad_from.a    != n->transition_prev_target_bg_grad_from.a  ||
                 cur_bg_grad_to.r      != n->transition_prev_target_bg_grad_to.r    ||
                 cur_bg_grad_to.g      != n->transition_prev_target_bg_grad_to.g   ||
                 cur_bg_grad_to.b      != n->transition_prev_target_bg_grad_to.b   ||
                 cur_bg_grad_to.a      != n->transition_prev_target_bg_grad_to.a);
            if (_animator_internal__size_transitions_enabled)
            {
                changed = changed
                    || (cur_size_w              != n->transition_prev_target_size_w)
                    || (cur_size_h              != n->transition_prev_target_size_h)
                    || (cur_font_size           != n->transition_prev_target_font_size)
                    || (cur_size_w_explicit     != n->transition_prev_target_size_w_explicit)
                    || (cur_size_h_explicit     != n->transition_prev_target_size_h_explicit)
                    || (cur_font_size_explicit  != n->transition_prev_target_font_size_explicit);
            }

            if (changed)
            {
                //
                // Compute the currently displayed values (applying the
                // OLD in-flight interpolation if any) and store as the
                // new starting point. Chained transitions pick up
                // where the previous one left off.
                //
                if (n->transition_age_ms < r->transition_duration_ms)
                {
                    float t     = n->transition_age_ms / r->transition_duration_ms;
                    float eased = _animator_internal__ease(r->transition_easing, r->transition_easing_params, t);
                    n->transition_from_bg           = _animator_internal__lerp_color(n->transition_from_bg,           n->transition_prev_target_bg,           eased);
                    n->transition_from_fg           = _animator_internal__lerp_color(n->transition_from_fg,           n->transition_prev_target_fg,           eased);
                    n->transition_from_text         = _animator_internal__lerp_color(n->transition_from_text,         n->transition_prev_target_text,         eased);
                    n->transition_from_radius       = _animator_internal__lerp_f(    n->transition_from_radius,       n->transition_prev_target_radius,       eased);
                    n->transition_from_bg_grad_from = _animator_internal__lerp_color(n->transition_from_bg_grad_from, n->transition_prev_target_bg_grad_from, eased);
                    n->transition_from_bg_grad_to   = _animator_internal__lerp_color(n->transition_from_bg_grad_to,   n->transition_prev_target_bg_grad_to,   eased);
                    n->transition_from_size_w       = _animator_internal__lerp_f(    n->transition_from_size_w,       n->transition_prev_target_size_w,       eased);
                    n->transition_from_size_h       = _animator_internal__lerp_f(    n->transition_from_size_h,       n->transition_prev_target_size_h,       eased);
                    n->transition_from_font_size    = _animator_internal__lerp_f(    n->transition_from_font_size,    n->transition_prev_target_font_size,    eased);
                }
                else
                {
                    //
                    // Idle: displayed == previous target (already settled).
                    //
                    n->transition_from_bg                 = n->transition_prev_target_bg;
                    n->transition_from_fg                 = n->transition_prev_target_fg;
                    n->transition_from_text               = n->transition_prev_target_text;
                    n->transition_from_radius             = n->transition_prev_target_radius;
                    n->transition_from_bg_grad_from       = n->transition_prev_target_bg_grad_from;
                    n->transition_from_bg_grad_to         = n->transition_prev_target_bg_grad_to;
                    n->transition_from_size_w             = n->transition_prev_target_size_w;
                    n->transition_from_size_h             = n->transition_prev_target_size_h;
                    n->transition_from_font_size          = n->transition_prev_target_font_size;
                    n->transition_from_size_w_explicit    = n->transition_prev_target_size_w_explicit;
                    n->transition_from_size_h_explicit    = n->transition_prev_target_size_h_explicit;
                    n->transition_from_font_size_explicit = n->transition_prev_target_font_size_explicit;
                }
                n->transition_prev_target_bg                 = cur_bg;
                n->transition_prev_target_fg                 = cur_fg;
                n->transition_prev_target_text               = cur_text;
                n->transition_prev_target_radius             = cur_radius;
                n->transition_prev_target_bg_grad_from       = cur_bg_grad_from;
                n->transition_prev_target_bg_grad_to         = cur_bg_grad_to;
                n->transition_prev_target_size_w             = cur_size_w;
                n->transition_prev_target_size_h             = cur_size_h;
                n->transition_prev_target_font_size          = cur_font_size;
                n->transition_prev_target_size_w_explicit    = cur_size_w_explicit;
                n->transition_prev_target_size_h_explicit    = cur_size_h_explicit;
                n->transition_prev_target_font_size_explicit = cur_font_size_explicit;
                n->transition_age_ms                         = 0.0f;
                DBG_ANIM log_info("[dbg-anim] CHANGED node=%p type=%d sw %.0f->%.0f(expl from=%d cur=%d) sh %.0f->%.0f fs %.0f->%.0f",
                                  (void*)n, (int)n->type,
                                  n->transition_from_size_w, cur_size_w,
                                  n->transition_from_size_w_explicit, cur_size_w_explicit,
                                  n->transition_from_size_h, cur_size_h,
                                  n->transition_from_font_size, cur_font_size);
            }

            //
            // Apply the in-flight interpolation. When age_ms has
            // reached duration, the lerp collapses to the target, so
            // we could skip this block, but it's cheap and simplifies
            // control flow to always run.
            //
            if (n->transition_age_ms < r->transition_duration_ms)
            {
                float t     = n->transition_age_ms / r->transition_duration_ms;
                float eased = _animator_internal__ease(r->transition_easing, r->transition_easing_params, t);
                r->background_color = _animator_internal__lerp_color(n->transition_from_bg,     cur_bg,     eased);
                r->accent_color     = _animator_internal__lerp_color(n->transition_from_fg,     cur_fg,     eased);
                r->font_color       = _animator_internal__lerp_color(n->transition_from_text,   cur_text,   eased);
                r->radius           = _animator_internal__lerp_f(    n->transition_from_radius, cur_radius, eased);
                //
                // bg_gradient endpoints animate only when has_bg_gradient
                // is set on the resolved style. Without that gate, every
                // node with a transition would lerp the (zero,zero) of
                // an unset gradient between states -- harmless visually
                // (gradient isn't rendered without the flag) but wastes
                // work and could surprise downstream consumers reading
                // bg_gradient_from/to expecting a stable target.
                //
                if (r->has_bg_gradient)
                {
                    r->bg_gradient_from = _animator_internal__lerp_color(n->transition_from_bg_grad_from, cur_bg_grad_from, eased);
                    r->bg_gradient_to   = _animator_internal__lerp_color(n->transition_from_bg_grad_to,   cur_bg_grad_to,   eased);
                }
                //
                // Layout-affecting properties: gated on the runtime
                // toggle AND on both endpoints being rule-defined.
                // If the toggle is off, none of size_w/h/font_size
                // is touched (animator behaves like before the
                // extension). If the toggle is on but one endpoint
                // is unset, snap to cur (resolver's value) instead
                // of lerping from a transient zero.
                //
                if (_animator_internal__size_transitions_enabled)
                {
                    if (n->transition_from_size_w_explicit && cur_size_w_explicit)
                    {
                        float lerped = _animator_internal__lerp_f(n->transition_from_size_w, cur_size_w, eased);
                        DBG_ANIM log_info("[dbg-anim] LERP node=%p sw from=%.1f cur=%.1f eased=%.3f -> %.1f",
                                          (void*)n, n->transition_from_size_w, cur_size_w, eased, lerped);
                        r->size_w = lerped;
                    }
                    if (n->transition_from_size_h_explicit && cur_size_h_explicit)
                    {
                        r->size_h = _animator_internal__lerp_f(n->transition_from_size_h, cur_size_h, eased);
                    }
                    if (n->transition_from_font_size_explicit && cur_font_size_explicit)
                    {
                        r->font_size = _animator_internal__lerp_f(n->transition_from_font_size, cur_font_size, eased);
                    }
                }
            }

            n->transition_age_ms += ms_delta;
            if (n->transition_age_ms > r->transition_duration_ms)
            {
                n->transition_age_ms = r->transition_duration_ms;
            }
        }
    }

    //
    // ===== :disappear =====================================================
    //
    // When the node is mid-outro (disappear_remaining_ms > 0), interpolate
    // the resolved values from the current resolver output TOWARD the
    // :disappear slot's override values. The remaining counter drains by
    // ms_delta each frame; once it hits zero the override above (which
    // forced visibility/display back to visible/block) stops firing on
    // the next frame, and the node finally hides for real.
    //
    // t is 1 - (remaining/duration) so it goes 0 -> 1 over the duration.
    // Only properties the :disappear slot populated are interpolated;
    // unpopulated slots leave the resolver's value alone, which means
    // "the node keeps its current color until it finishes fading out".
    // For the common case of a pure opacity fade, the author writes
    // `:disappear { color: rgba(...); bg: rgba(...); }` with alpha = 0.
    //
    if (disappear_playing)
    {
        float t_raw = 1.0f - (n->disappear_remaining_ms / disappear_duration);
        if (t_raw < 0.0f) { t_raw = 0.0f; }
        if (t_raw > 1.0f) { t_raw = 1.0f; }
        float eased = _animator_internal__ease(d->animation_easing, d->animation_easing_params, t_raw);

        if (d->has_background_color)
        {
            r->background_color = _animator_internal__lerp_color(r->background_color, d->background_color, eased);
        }
        if (d->has_accent_color)
        {
            r->accent_color = _animator_internal__lerp_color(r->accent_color, d->accent_color, eased);
        }
        if (d->has_font_color)
        {
            r->font_color = _animator_internal__lerp_color(r->font_color, d->font_color, eased);
        }
        if (d->radius > 0.0f)
        {
            r->radius = _animator_internal__lerp_f(r->radius, d->radius, eased);
        }
        //
        // Shortcut path: if the :disappear slot declared NO property
        // overrides but does have animation_duration_ms, the intent is
        // "fade to alpha=0 over this duration", matching how :appear's
        // shortcut mode fades IN from alpha=0.
        //
        if (!d->has_background_color && !d->has_accent_color && !d->has_font_color)
        {
            float inv = 1.0f - eased;
            r->background_color.a *= inv;
            r->accent_color.a     *= inv;
            r->font_color.a       *= inv;
        }

        n->disappear_remaining_ms -= ms_delta;
        if (n->disappear_remaining_ms < 0.0f) { n->disappear_remaining_ms = 0.0f; }
    }

    //
    // ===== smooth scroll =================================================
    //
    // Bring scroll_y toward scroll_y_target over roughly
    // resolved.scroll_smooth_ms milliseconds. Exponential decay --
    // the delta shrinks by a factor of (1 - dt/tau) every tick, so
    // after one tau the delta is ~37 % of the original (the 1/e
    // point). Feels natural for wheel input because a single tick
    // produces a quick-then-decelerating catch-up motion without
    // visible bouncing.
    //
    // Two exit ramps:
    //   (1) scroll_smooth_ms == 0: snap immediately. This matches the
    //       behaviour that existed before this field was added.
    //   (2) |target - current| < 0.5 px: snap. Prevents the lerp
    //       asymptote from sitting near-but-not-at the target forever
    //       (which would leave a sub-pixel drift that's visible in
    //       the integer-clamped child placement).
    //
    {
        float delta = n->scroll_y_target - n->scroll_y;
        if (r->scroll_smooth_ms <= 0.0f || (delta > -0.5f && delta < 0.5f))
        {
            n->scroll_y = n->scroll_y_target;
        }
        else
        {
            float t = ms_delta / r->scroll_smooth_ms;
            if (t > 1.0f) { t = 1.0f; }
            n->scroll_y += delta * t;
        }
    }
    //
    // Same exponential-decay lerp for the x axis. Kept as a separate
    // block so scroll_smooth_ms governs both axes independently of each
    // other -- the ratio stays the same, so diagonal scrolling feels
    // consistent.
    //
    {
        float delta = n->scroll_x_target - n->scroll_x;
        if (r->scroll_smooth_ms <= 0.0f || (delta > -0.5f && delta < 0.5f))
        {
            n->scroll_x = n->scroll_x_target;
        }
        else
        {
            float t = ms_delta / r->scroll_smooth_ms;
            if (t > 1.0f) { t = 1.0f; }
            n->scroll_x += delta * t;
        }
    }

    gui_node* c = n->first_child;
    while (c != NULL)
    {
        _animator_internal__visit(c, ms_delta, child_fade_ancestor);
        c = c->next_sibling;
    }
}

//
// Walk the tree under `n` and return 1 as soon as we find any node
// whose appear-fade animation is still in progress (appear_age_ms <
// resolved.appear_ms with a positive duration). This is the cheap
// "is anything still animating?" probe used by render-gating
// platform backends to decide whether to skip a render when the
// frame callback is still pending and there's been no input.
//
// Walks early-out on the first hit. Doesn't allocate, doesn't mutate.
//
static int _animator_internal__node_has_active(gui_node* n)
{
    if (n == NULL) { return 0; }
    if (n->resolved.appear_ms > 0.0f
        && (float)n->appear_age_ms < n->resolved.appear_ms)
    {
        return 1;
    }
    //
    // The pseudo-state :appear block uses a separate duration slot;
    // honour it too so per-property animations also keep the loop
    // alive.
    //
    if (n->style[GUI_STATE_APPEAR].animation_duration_ms > 0.0f
        && (float)n->appear_age_ms < n->style[GUI_STATE_APPEAR].animation_duration_ms)
    {
        return 1;
    }
    gui_node* c = n->first_child;
    while (c != NULL)
    {
        if (_animator_internal__node_has_active(c)) { return 1; }
        c = c->next_sibling;
    }
    return 0;
}

int animator__has_active(void)
{
    gui_node* root = scene__root();
    if (root == NULL) { return 0; }
    return _animator_internal__node_has_active(root);
}

void animator__tick(void)
{
    gui_node* root = scene__root();
    if (root == NULL) { return; }

    //
    // Read the per-frame delta. On the FIRST frame after scene__set_root,
    // scene hasn't seen a previous timestamp yet, so delta_ms is 0.
    //
    // We CAN'T early-out here. If we did, every freshly-created node
    // would draw at FULL opacity for one frame (resolved.* still has
    // the steady-state values from scene__resolve_styles), then frame 2
    // would advance appear_age_ms from 0 to ~16 ms and suddenly drop
    // alpha to ~0 -- a visible "bright flash, then fade in from
    // invisible" sequence. On opengl3 specifically this flash survives
    // SwapBuffers and DWM presentation and is what the user sees as a
    // flicker during the page-load animation.
    //
    // Instead, ALWAYS visit the tree. With delta = 0, appear_age_ms
    // stays at 0, eased(0) = 0, and the alpha multiplications produce
    // 0 -- so frame 1 draws fully invisible. Frame 2 advances the
    // timer normally. Result: a clean monotonically-increasing fade
    // from invisible to opaque, no flash.
    //
    int64 delta_ms = scene__frame_delta_ms();
    if (delta_ms < 0) { delta_ms = 0; } // defensive: clock_gettime / QPC went backwards.

    //
    //root has no fade ancestor (the window itself doesn't fade its
    //own descendants unless it's a scrollable container with
    //scroll_fade_px, in which case _visit will pick it up and set
    //child_fade_ancestor = root for its children).
    //
    _animator_internal__visit(root, (float)delta_ms, NULL);
}
