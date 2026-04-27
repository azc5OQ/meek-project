#ifndef SCROLL_H
#define SCROLL_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

//
// scroll.h - shared scrollbar mechanics for any scrollable container.
//
// Used by widget_div.c (a styled scrollable block) and widget_window.c
// (the whole-page scrollbar). Both widgets:
//   - Read overflow_x / overflow_y from gui_style.
//   - Lay out their children with a -scroll_y offset.
//   - Need a draggable scrollbar drawn on the right edge.
//   - Need to clip children to their bounds (renderer scissor).
//
// All the logic lives here so adding a third scrollable container kind
// is a copy-paste of the call sequence, not a copy-paste of the math.
//
// USAGE PATTERN (widget side):
//
//   In layout(): after computing the node's bounds and content extent,
//     parent->content_h = ...measured...;
//     scroll__clamp(parent);
//
//   In emit_draws(): bracket children with scissor + bg + push:
//     if (scroll__vbar_visible(node))
//         renderer__push_scissor(node->bounds);
//     ...submit bg...
//
//   In emit_draws_post(): scrollbar on top of children, then pop:
//     if (scroll__vbar_visible(node)) {
//         scroll__draw_vbar(node, scale);
//         renderer__pop_scissor();
//     }
//
//   In on_mouse_down/drag/up:
//     scroll__on_mouse_down(node, x, y, scale);
//     scroll__on_mouse_drag(node, y, scale);  // returns TRUE if consumed
//     scroll__on_mouse_up(node);
//

/**
 * Vertical scrollbar visible? Encodes the overflow_y rules:
 *   VISIBLE / HIDDEN  -> never visible.
 *   SCROLL            -> always visible.
 *   AUTO              -> visible iff content_h exceeds bounds.h.
 *
 * @function scroll__vbar_visible
 * @param {gui_node*} node - The container node (must have content_h + bounds populated by layout).
 * @return {boole} TRUE if a vertical scrollbar should be drawn.
 */
GUI_API boole scroll__vbar_visible(gui_node* node);

/**
 * Horizontal scrollbar visible? Mirror of scroll__vbar_visible for the
 * x axis. Encodes overflow_x and content_w vs bounds.w instead of the
 * y equivalents. Both bars coexist when the container overflows on
 * both axes; the bottom-right corner where they'd meet is occupied by
 * the vertical bar (horizontal ends before it).
 *
 * @function scroll__hbar_visible
 * @param {gui_node*} node - The container node.
 * @return {boole} TRUE if a horizontal scrollbar should be drawn.
 */
GUI_API boole scroll__hbar_visible(gui_node* node);

/**
 * Resolve the scrollbar thickness in pixels, applying the style override
 * if present and otherwise the toolkit default (~12 px scaled).
 *
 * @function scroll__bar_size
 * @param {gui_node*} node - The container node.
 * @param {float} scale - Global UI scale factor.
 * @return {float} Bar thickness in logical pixels.
 */
GUI_API float scroll__bar_size(gui_node* node, float scale);

/**
 * Clamp scroll_y into [0, max(0, content_h - bounds.h)]. Called at the
 * end of layout (so a content-shrink resets stale offsets) and during
 * mid-drag updates (so the thumb can't fall off the end of the track).
 *
 * @function scroll__clamp
 * @param {gui_node*} node - The container node.
 * @return {void}
 */
GUI_API void  scroll__clamp(gui_node* node);

/**
 * Submit the vertical scrollbar's track + thumb rects via renderer__submit_rect.
 * Caller is responsible for checking scroll__vbar_visible first and
 * for managing scissor state (the bar itself sits inside the container's
 * bounds so it's safe to draw inside or outside the scissor region).
 *
 * @function scroll__draw_vbar
 * @param {gui_node*} node - The container node.
 * @param {float} scale - Global UI scale factor.
 * @return {void}
 */
GUI_API void  scroll__draw_vbar(gui_node* node, float scale);

/**
 * Submit the horizontal scrollbar's track + thumb rects. Mirror of
 * scroll__draw_vbar for the x axis. If the vertical bar is ALSO visible,
 * the horizontal bar's right end is shortened by scroll__bar_size to
 * leave the corner clear for the vbar.
 *
 * @function scroll__draw_hbar
 * @param {gui_node*} node - The container node.
 * @param {float} scale - Global UI scale factor.
 * @return {void}
 */
GUI_API void  scroll__draw_hbar(gui_node* node, float scale);

/**
 * Mouse-down hook for a scrollable container. If the cursor lands on
 * the scrollbar thumb, records drag-start state on the node so a
 * subsequent on_mouse_drag converts mouse delta into scroll delta.
 *
 * @function scroll__on_mouse_down
 * @param {gui_node*} node - The container node.
 * @param {int64} x - Cursor x.
 * @param {int64} y - Cursor y.
 * @param {float} scale - Global UI scale factor.
 * @return {void}
 */
GUI_API void  scroll__on_mouse_down(gui_node* node, int64 x, int64 y, float scale);

/**
 * Mouse-drag hook. If a thumb drag is in progress, applies the delta
 * to scroll_y, clamps, and returns TRUE so the caller swallows the
 * event (no hover updates while scrolling). Otherwise returns FALSE
 * so normal hover semantics continue.
 *
 * @function scroll__on_mouse_drag
 * @param {gui_node*} node - The container node.
 * @param {int64} y - Cursor y at the drag event.
 * @param {float} scale - Global UI scale factor.
 * @return {boole} TRUE if the event was consumed by an active scrollbar drag.
 */
GUI_API boole scroll__on_mouse_drag(gui_node* node, int64 y, float scale);

/**
 * Mouse-up hook. Clears any active drag state on the node.
 *
 * @function scroll__on_mouse_up
 * @param {gui_node*} node - The container node.
 * @return {void}
 */
GUI_API void  scroll__on_mouse_up(gui_node* node);

#endif
