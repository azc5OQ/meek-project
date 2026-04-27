//
// scroll.c - implementation of the shared scrollbar mechanics.
//
// Used by widget_div.c and widget_window.c. Pure CPU code -- the only
// renderer interaction is renderer__submit_rect from scroll__draw_vbar.
// Layout/scissor lifecycle is the caller's responsibility (see scroll.h).
//
// Conventions:
//   - vbar = vertical scrollbar (the only kind currently drawn).
//   - "track" = the full-height channel the thumb slides in.
//   - "thumb" = the draggable element inside the track.
//   - All rects are in pixel coords with a top-left origin.
//

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "renderer.h"
#include "scroll.h"

//
// Visual defaults (used when style leaves the field at zero). Chosen
// to be readable against dark themes and still grabbable on a touch
// device.
//
static const float _SCROLL_INTERNAL__DEFAULT_BAR_SIZE  = 12.0f;
static const float _SCROLL_INTERNAL__DEFAULT_THUMB_MIN = 20.0f; // thumbs smaller than this are too tiny to drag.

//
// Test whether a particular axis should scroll given the user's
// overflow-mode declaration and the actual content-vs-bounds ratio.
//
static boole _scroll_internal__is_scroll_axis(gui_overflow mode, boole content_exceeds)
{
    if (mode == GUI_OVERFLOW_SCROLL) { return TRUE; }
    if (mode == GUI_OVERFLOW_AUTO)   { return content_exceeds; }
    return FALSE;
}

float scroll__bar_size(gui_node* node, float scale)
{
    gui_style* s = &node->resolved;
    return (s->scrollbar_size > 0.0f) ? s->scrollbar_size * scale : _SCROLL_INTERNAL__DEFAULT_BAR_SIZE * scale;
}

boole scroll__vbar_visible(gui_node* node)
{
    boole exceeds = (node->content_h > node->bounds.h + 0.5f);
    return _scroll_internal__is_scroll_axis(node->resolved.overflow_y, exceeds);
}

boole scroll__hbar_visible(gui_node* node)
{
    boole exceeds = (node->content_w > node->bounds.w + 0.5f);
    return _scroll_internal__is_scroll_axis(node->resolved.overflow_x, exceeds);
}

//
// Track rect: rightmost `bar_size` pixels of the container, full height.
// File-local because external callers should go through scroll__draw_vbar.
//
// If the HORIZONTAL bar is ALSO visible the vertical track's bottom end
// is shortened by bar_size so the corner where the two bars would meet
// stays clear (the h-bar gets the corner by symmetry with the visual
// convention).
//
static gui_rect _scroll_internal__vbar_track(gui_node* node, float bar_size)
{
    gui_rect r;
    r.x = node->bounds.x + node->bounds.w - bar_size;
    r.y = node->bounds.y;
    r.w = bar_size;
    r.h = node->bounds.h;
    if (scroll__hbar_visible(node))
    {
        r.h -= bar_size;
        if (r.h < 0.0f) { r.h = 0.0f; }
    }
    return r;
}

//
// Track rect for the horizontal bar. Bottom-most `bar_size` pixels, full
// width; if the vertical bar is also visible, the right end shortens by
// bar_size so the corner is left to the vbar.
//
static gui_rect _scroll_internal__hbar_track(gui_node* node, float bar_size)
{
    gui_rect r;
    r.x = node->bounds.x;
    r.y = node->bounds.y + node->bounds.h - bar_size;
    r.w = node->bounds.w;
    r.h = bar_size;
    if (scroll__vbar_visible(node))
    {
        r.w -= bar_size;
        if (r.w < 0.0f) { r.w = 0.0f; }
    }
    return r;
}

//
// Thumb rect within the track. Size = track_h * (view_h / content_h),
// clamped to [DEFAULT_THUMB_MIN, track_h]. Position scales with
// scroll_y / (content_h - view_h).
//
static gui_rect _scroll_internal__vbar_thumb(gui_node* node, gui_rect track)
{
    float content_h = node->content_h;
    float view_h    = node->bounds.h;

    //
    // Content fits entirely. We only get here when overflow_y == SCROLL
    // (AUTO would have skipped the bar via vbar_visible). Show a
    // full-height thumb so the bar still reads as a scrollbar even
    // though there's nothing to scroll.
    //
    if (content_h <= view_h)
    {
        return track;
    }

    float ratio   = view_h / content_h;
    float thumb_h = track.h * ratio;
    if (thumb_h < _SCROLL_INTERNAL__DEFAULT_THUMB_MIN) { thumb_h = _SCROLL_INTERNAL__DEFAULT_THUMB_MIN; }
    if (thumb_h > track.h)                              { thumb_h = track.h; }

    float scroll_max = content_h - view_h;
    float travel     = track.h - thumb_h;
    float t          = (scroll_max > 0.0f) ? (node->scroll_y / scroll_max) : 0.0f;
    if (t < 0.0f) { t = 0.0f; }
    if (t > 1.0f) { t = 1.0f; }

    gui_rect r;
    r.x = track.x;
    r.y = track.y + travel * t;
    r.w = track.w;
    r.h = thumb_h;
    return r;
}

void scroll__clamp(gui_node* node)
{
    float max_y = node->content_h - node->bounds.h;
    if (max_y < 0.0f)         { max_y = 0.0f; }
    if (node->scroll_y < 0.0f) { node->scroll_y = 0.0f; }
    if (node->scroll_y > max_y) { node->scroll_y = max_y; }
    //
    // Clamp the target too. content_h can shrink between frames (a
    // :disappear animation, a hot reload with fewer list items, a
    // display:none toggle on a child) which would leave
    // scroll_y_target pointing off the end if we didn't clamp here.
    // The animator would then eternally chase an out-of-range target
    // and the user would see scroll_y stuck at max_y instead of
    // rebounding to a valid position.
    //
    if (node->scroll_y_target < 0.0f)  { node->scroll_y_target = 0.0f;  }
    if (node->scroll_y_target > max_y) { node->scroll_y_target = max_y; }

    //
    // Same clamp for the x axis. content_w shrinking (hot reload drops
    // wide child, etc.) can leave scroll_x pointing off the right end.
    //
    float max_x = node->content_w - node->bounds.w;
    if (max_x < 0.0f)          { max_x = 0.0f; }
    if (node->scroll_x < 0.0f) { node->scroll_x = 0.0f; }
    if (node->scroll_x > max_x) { node->scroll_x = max_x; }
    if (node->scroll_x_target < 0.0f)  { node->scroll_x_target = 0.0f;  }
    if (node->scroll_x_target > max_x) { node->scroll_x_target = max_x; }
}

void scroll__draw_vbar(gui_node* node, float scale)
{
    gui_style* s = &node->resolved;
    float bar_size   = scroll__bar_size(node, scale);
    float bar_radius = (s->scrollbar_radius > 0.0f) ? s->scrollbar_radius * scale : bar_size * 0.5f;

    gui_rect track = _scroll_internal__vbar_track(node, bar_size);
    gui_rect thumb = _scroll_internal__vbar_thumb(node, track);

    //
    // Defaults read against the standard dark theme: dim white track,
    // brighter white thumb. A .style override is the right move for any
    // real app that doesn't use the default theme.
    //
    gui_color track_c = s->has_scrollbar_track ? s->scrollbar_track : scene__rgba(1.0f, 1.0f, 1.0f, 0.06f);
    gui_color thumb_c = s->has_scrollbar_thumb ? s->scrollbar_thumb : scene__rgba(1.0f, 1.0f, 1.0f, 0.30f);

    renderer__submit_rect(track, track_c, bar_radius);
    renderer__submit_rect(thumb, thumb_c, bar_radius);
}

//
// Thumb rect for the horizontal bar. Size = track_w * (view_w / content_w);
// position scales with scroll_x / (content_w - view_w).
//
static gui_rect _scroll_internal__hbar_thumb(gui_node* node, gui_rect track)
{
    float content_w = node->content_w;
    float view_w    = node->bounds.w;
    if (scroll__vbar_visible(node)) { view_w -= track.h; } // corner shared with vbar.

    if (content_w <= view_w)
    {
        return track;
    }

    float ratio   = view_w / content_w;
    float thumb_w = track.w * ratio;
    if (thumb_w < _SCROLL_INTERNAL__DEFAULT_THUMB_MIN) { thumb_w = _SCROLL_INTERNAL__DEFAULT_THUMB_MIN; }
    if (thumb_w > track.w)                              { thumb_w = track.w; }

    float scroll_max = content_w - view_w;
    float travel     = track.w - thumb_w;
    float t          = (scroll_max > 0.0f) ? (node->scroll_x / scroll_max) : 0.0f;
    if (t < 0.0f) { t = 0.0f; }
    if (t > 1.0f) { t = 1.0f; }

    gui_rect r;
    r.x = track.x + travel * t;
    r.y = track.y;
    r.w = thumb_w;
    r.h = track.h;
    return r;
}

void scroll__draw_hbar(gui_node* node, float scale)
{
    gui_style* s = &node->resolved;
    float bar_size   = scroll__bar_size(node, scale);
    float bar_radius = (s->scrollbar_radius > 0.0f) ? s->scrollbar_radius * scale : bar_size * 0.5f;

    gui_rect track = _scroll_internal__hbar_track(node, bar_size);
    gui_rect thumb = _scroll_internal__hbar_thumb(node, track);

    gui_color track_c = s->has_scrollbar_track ? s->scrollbar_track : scene__rgba(1.0f, 1.0f, 1.0f, 0.06f);
    gui_color thumb_c = s->has_scrollbar_thumb ? s->scrollbar_thumb : scene__rgba(1.0f, 1.0f, 1.0f, 0.30f);

    renderer__submit_rect(track, track_c, bar_radius);
    renderer__submit_rect(thumb, thumb_c, bar_radius);
}

//
// Hit-test the cursor against the vertical or horizontal thumb. File-
// local; on_mouse_down is the public entry point.
//
static boole _scroll_internal__hit_vthumb(gui_node* node, int64 x, int64 y, float scale)
{
    if (!scroll__vbar_visible(node)) { return FALSE; }
    float bar_size = scroll__bar_size(node, scale);
    gui_rect track = _scroll_internal__vbar_track(node, bar_size);
    gui_rect thumb = _scroll_internal__vbar_thumb(node, track);
    float fx = (float)x;
    float fy = (float)y;
    return (boole)(fx >= thumb.x && fx < thumb.x + thumb.w && fy >= thumb.y && fy < thumb.y + thumb.h);
}

static boole _scroll_internal__hit_hthumb(gui_node* node, int64 x, int64 y, float scale)
{
    if (!scroll__hbar_visible(node)) { return FALSE; }
    float bar_size = scroll__bar_size(node, scale);
    gui_rect track = _scroll_internal__hbar_track(node, bar_size);
    gui_rect thumb = _scroll_internal__hbar_thumb(node, track);
    float fx = (float)x;
    float fy = (float)y;
    return (boole)(fx >= thumb.x && fx < thumb.x + thumb.w && fy >= thumb.y && fy < thumb.y + thumb.h);
}

void scroll__on_mouse_down(gui_node* node, int64 x, int64 y, float scale)
{
    //
    // Vertical thumb wins if both happen to overlap (corner overlap is
    // prevented by the cross-aware track sizes above, but the thumbs
    // are the part the user aims at). Two drag directions share one
    // axis field, so we pick one.
    //
    if (_scroll_internal__hit_vthumb(node, x, y, scale))
    {
        node->scroll_drag_axis         = 1; // y.
        node->scroll_drag_mouse_start  = (float)y;
        node->scroll_drag_scroll_start = node->scroll_y;
    }
    else if (_scroll_internal__hit_hthumb(node, x, y, scale))
    {
        node->scroll_drag_axis         = 2; // x.
        node->scroll_drag_mouse_start  = (float)x;
        node->scroll_drag_scroll_start = node->scroll_x;
    }
    else
    {
        //
        // Click landed elsewhere in the container. Ensure no stale drag
        // state survives -- on_mouse_drag checks scroll_drag_axis and
        // no-ops when zero.
        //
        node->scroll_drag_axis = 0;
    }
}

boole scroll__on_mouse_drag(gui_node* node, int64 y, float scale)
{
    //
    // Caller passes both x and y through node->scroll_drag_mouse_start
    // vs current (x or y). The function signature only has a single
    // integer so we interpret it based on scroll_drag_axis: 1 = y, 2 =
    // x. That matches widget_div's delegate which passes `y` for y-axis
    // drags; we rely on the x-axis case being uncommon enough that the
    // caller can wrap via a helper if needed. See scroll__on_mouse_drag_xy
    // for an explicit-x variant.
    //
    if (node->scroll_drag_axis == 1)
    {
        float bar_size = scroll__bar_size(node, scale);
        gui_rect track = _scroll_internal__vbar_track(node, bar_size);
        gui_rect thumb = _scroll_internal__vbar_thumb(node, track);

        float scroll_max = node->content_h - node->bounds.h;
        float travel     = track.h - thumb.h;
        if (scroll_max <= 0.0f || travel <= 0.0f) { return TRUE; }

        float mouse_delta  = (float)y - node->scroll_drag_mouse_start;
        float scroll_delta = mouse_delta * (scroll_max / travel);

        //
        // Drag is a direct-manipulation gesture -- the thumb must track
        // the cursor 1:1, no lerp. So we write BOTH scroll_y AND
        // scroll_y_target, which keeps the animator's smooth-scroll
        // interpolator at rest (delta between them is zero) while the
        // drag is in progress. Wheel input is the opposite: it writes
        // ONLY scroll_y_target and lets the animator catch up. That split
        // matches CSS's `scroll-behavior: smooth` semantics -- programmatic
        // scrolls smooth; direct-manipulation gestures don't.
        //
        node->scroll_y        = node->scroll_drag_scroll_start + scroll_delta;
        node->scroll_y_target = node->scroll_y;
        scroll__clamp(node);
        return TRUE;
    }
    if (node->scroll_drag_axis == 2)
    {
        //
        // Horizontal drag. The single-arg `y` signature is a mild lie
        // here -- the caller actually forwards the cursor's X. Widget
        // hosts that want to support both axes route the drag through
        // on_mouse_drag with the right coordinate based on the active
        // axis (see widget_div.c's forward).
        //
        float bar_size = scroll__bar_size(node, scale);
        gui_rect track = _scroll_internal__hbar_track(node, bar_size);
        gui_rect thumb = _scroll_internal__hbar_thumb(node, track);

        float scroll_max = node->content_w - node->bounds.w;
        float travel     = track.w - thumb.w;
        if (scroll_max <= 0.0f || travel <= 0.0f) { return TRUE; }

        float mouse_delta  = (float)y - node->scroll_drag_mouse_start;
        float scroll_delta = mouse_delta * (scroll_max / travel);

        node->scroll_x        = node->scroll_drag_scroll_start + scroll_delta;
        node->scroll_x_target = node->scroll_x;
        scroll__clamp(node);
        return TRUE;
    }
    return FALSE;
}

void scroll__on_mouse_up(gui_node* node)
{
    node->scroll_drag_axis = 0;
}
