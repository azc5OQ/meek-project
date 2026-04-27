//
//scene_input.c -- pointer + touch + keyboard input state machine,
//split out of scene.c. Owns:
//  * hover / pressed / focus tracking (_input struct)
//  * scene__on_mouse_move / scene__on_mouse_button
//  * scene__on_mouse_wheel / scene__on_touch_scroll
//  * scene__on_char / scene__on_key / scene__deliver_*
//  * scene__set_focus / scene__focus / scene__pressed
//  * scene__on_resize (stub; layout runs every frame anyway)
//
//Reaches the tree via scene__root() / scene__hit_test() from
//scene.h, and fires keyboard auto-show/hide through the existing
//scene__show_keyboard / hide_keyboard / virtual_keyboard_enabled
//public API. scene.c's node-free path calls
//scene_input__on_node_freed so a destroyed node can't be left
//hanging as focus/press/hover target.
//

#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "scene_input.h"
#include "scroll.h"
#include "third_party/log.h"
#include "debug_definitions.h"

//
// Optional char-redirect hook. Set by a host (meek-shell) to
// forward character input from widget_keyboard to an alternative
// sink (e.g., a zwp_text_input_v3 commit_string on the compositor).
// When non-null, scene__deliver_char and scene__on_char call the
// redirect; if it returns TRUE, normal vtable delivery is skipped.
// NULL means "disabled" -- characters go to the focused widget's
// on_char as before.
//
static scene_char_redirect_fn _scene_input_internal__char_redirect = NULL;

void scene__set_char_redirect(scene_char_redirect_fn fn)
{
    _scene_input_internal__char_redirect = fn;
}

boole scene__has_char_redirect(void)
{
    return (boole)(_scene_input_internal__char_redirect != NULL);
}

//
// Active modifier bitmask. widget_keyboard writes before each
// scene__on_char / scene__on_key call; the char redirect
// (meek_shell_v1_client on the shell side) reads during the
// redirect callback to forward ctrl / shift state to the target
// wl_keyboard client.
//
static uint _scene_input_internal__active_modifiers = 0;

void scene__set_active_modifiers(uint mask)
{
    _scene_input_internal__active_modifiers = mask;
}

uint scene__get_active_modifiers(void)
{
    return _scene_input_internal__active_modifiers;
}

//
//current input state for the hover + press + focus state machine.
//
typedef struct _scene_input_internal__state
{
    gui_node* hover;
    gui_node* pressed;
    gui_node* focus;
} _scene_input_internal__state;

static _scene_input_internal__state _scene_input_internal__input = { NULL, NULL, NULL };

static void _scene_input_internal__set_state(gui_node* n, gui_node_state s)
{
    if (n != NULL) { n->state = s; }
}

//
// Walk from `deepest` up the parent chain, setting `state` on each
// node. Lets `:active` / `:hover` style rules fire on ancestors of
// the hit node, not just on the hit node itself.
//
// Why: meek-ui's hit-test returns the deepest node under the touch
// point; its `state` field gets the new value, and the resolver
// overlays `style[state]` for THAT node. Parents in the same
// click-chain stay at GUI_STATE_DEFAULT, so a `:active` rule on a
// container (e.g. a launcher tile wrapping an icon image) never
// fires when the inner image is the deepest hit.
//
// CSS proper propagates :active and :hover up to the document
// element. We do the same -- walk all the way up to root. Cheap
// (chain is ~5 nodes deep in practice) and matches the mental
// model devs bring from CSS.
//
//
// Recursive descent helper for set_state_chain. Walks the entire
// subtree rooted at `n` setting each node's state to `s`. Used by
// the chain to also propagate state DOWN to a "press group" -- the
// hit node's immediate parent's full subtree -- so siblings of the
// deepest hit (and their descendants) light up alongside the hit
// itself. Without this, clicking a hit-tested leaf only marks the
// up-chain (leaf + ancestors); a sibling's :active rule wouldn't
// fire even though the user thinks of the whole tile-like
// container as a single press unit.
//
static void _scene_input_internal__set_state_subtree(gui_node* n, gui_node_state s)
{
    if (n == NULL) { return; }
    n->state = s;
    for (gui_node* c = n->first_child; c != NULL; c = c->next_sibling)
    {
        _scene_input_internal__set_state_subtree(c, s);
    }
}

static void _scene_input_internal__set_state_chain(gui_node* deepest, gui_node_state s)
{
    //
    // Walk UP from the deepest-hit node, marking each ancestor with
    // the new state. CSS-equivalent of :active firing on the chain
    // of nodes from the hit to the root.
    //
    for (gui_node* n = deepest; n != NULL; n = n->parent)
    {
        n->state = s;
    }
    //
    // Press-group expansion: find the deepest ancestor (or self)
    // that carries an `on_click` handler -- that node defines the
    // "click target", and its entire subtree should visually
    // respond to the press, not just the leaf the hit-test landed
    // on. So a tile column with `on_click` set + a frame + image +
    // label inside reads as one press unit; tapping any of those
    // children marks the whole tile :active.
    //
    // If no ancestor has on_click, skip the down-walk (the up-walk
    // already covered the chain). Common case for non-clickable
    // chrome where there's nothing to "press" anyway.
    //
    gui_node* group_root = deepest;
    while (group_root != NULL && group_root->on_click_hash == 0 && group_root->parent != NULL)
    {
        group_root = group_root->parent;
    }
    if (group_root != NULL && group_root->on_click_hash != 0)
    {
        _scene_input_internal__set_state_subtree(group_root, s);
    }
}

void scene_input__on_node_freed(gui_node* n)
{
    if (n == NULL) { return; }
    if (_scene_input_internal__input.focus   == n) { _scene_input_internal__input.focus   = NULL; }
    if (_scene_input_internal__input.pressed == n) { _scene_input_internal__input.pressed = NULL; }
    if (_scene_input_internal__input.hover   == n) { _scene_input_internal__input.hover   = NULL; }
}

void scene__on_mouse_move(int64 x, int64 y)
{
    gui_node* root = scene__root();
    if (root == NULL) { return; }

    gui_node* pressed = _scene_input_internal__input.pressed;
    if (pressed != NULL)
    {
        const widget_vtable* vt = widget_registry__get(pressed->type);
        if (vt != NULL && vt->on_mouse_drag != NULL)
        {
            if (vt->on_mouse_drag(pressed, x, y)) { return; }
        }
    }

    gui_node* hit  = scene__hit_test(root, x, y);
    gui_node* prev = _scene_input_internal__input.hover;

    if (hit == prev) { return; }

    //
    // Hover-leave: walk the previous-hover chain back to default,
    // skipping any node still in the active-press chain (those
    // remain GUI_STATE_PRESSED until release).
    //
    gui_node* still_pressed = _scene_input_internal__input.pressed;
    if (prev != NULL)
    {
        for (gui_node* n = prev; n != NULL; n = n->parent)
        {
            //
            // If this node is also in the press chain, leave it
            // PRESSED. Cheap "is n an ancestor-or-self of pressed"
            // check via walking up from pressed.
            //
            boole in_press_chain = FALSE;
            for (gui_node* p = still_pressed; p != NULL; p = p->parent)
            {
                if (p == n) { in_press_chain = TRUE; break; }
            }
            if (!in_press_chain) { n->state = GUI_STATE_DEFAULT; }
        }
    }

    _scene_input_internal__input.hover = hit;

    //
    // Hover-enter: chain set to HOVER, but again skip nodes
    // currently in the press chain.
    //
    if (hit != NULL)
    {
        for (gui_node* n = hit; n != NULL; n = n->parent)
        {
            boole in_press_chain = FALSE;
            for (gui_node* p = still_pressed; p != NULL; p = p->parent)
            {
                if (p == n) { in_press_chain = TRUE; break; }
            }
            if (!in_press_chain) { n->state = GUI_STATE_HOVER; }
        }
    }
}

void scene__on_mouse_button(int64 button, boole down, int64 x, int64 y)
{
    gui_node* root = scene__root();
    if (root == NULL) { return; }

    gui_node* hit = scene__hit_test(root, x, y);
    DBG_INPUT log_info("[dbg-input] mouse_button down=%d btn=%ld at (%ld,%ld) -> hit=%p type=%d",
                       (int)down, (long)button, (long)x, (long)y,
                       (void*)hit, hit ? (int)hit->type : -1);

    if (down)
    {
        if (hit != NULL)
        {
            _scene_input_internal__input.pressed = hit;
            _scene_input_internal__set_state_chain(hit, GUI_STATE_PRESSED);

            const widget_vtable* vt = widget_registry__get(hit->type);

            boole preserves = (boole)(vt != NULL && (vt->preserves_focus || (vt->flags & GUI_WF_PRESERVES_FOCUS)));
            boole takes     = (boole)(vt != NULL && (vt->takes_focus     || (vt->flags & GUI_WF_TAKES_FOCUS)));
            if (preserves)      { /* no-op */ }
            else if (takes)     { scene__set_focus(hit); }
            else                { scene__set_focus(NULL); }

            if (vt != NULL && vt->on_mouse_down != NULL)
            {
                vt->on_mouse_down(hit, x, y, button);
            }
        }
        else
        {
            scene__set_focus(NULL);
        }
        return;
    }

    //mouse up
    gui_node* pressed = _scene_input_internal__input.pressed;
    if (pressed != NULL)
    {
        const widget_vtable* vt             = widget_registry__get(pressed->type);
        boole                consumes_click = (boole)(vt != NULL && (vt->consumes_click || (vt->flags & GUI_WF_CONSUMES_CLICK)));
        if (vt != NULL && vt->on_mouse_up != NULL)
        {
            vt->on_mouse_up(pressed, x, y, button);
        }

        if (!consumes_click && hit == pressed)
        {
            DBG_INPUT log_info("[dbg-input] dispatch_click on node=%p type=%d",
                               (void*)pressed, (int)pressed->type);
            scene__dispatch_click(pressed, x, y, button);
        }

        //
        // Release: walk the press chain back. If the mouse is
        // still over the same deepest node, drop the chain into
        // HOVER (mouse is hovering over it post-release). Else
        // drop the chain into DEFAULT.
        //
        if (pressed == hit) { _scene_input_internal__set_state_chain(pressed, GUI_STATE_HOVER); }
        else                { _scene_input_internal__set_state_chain(pressed, GUI_STATE_DEFAULT); }
        _scene_input_internal__input.pressed = NULL;
    }
}

void scene__on_mouse_wheel(int64 x, int64 y, float delta)
{
    gui_node* root = scene__root();
    if (root == NULL) { return; }

    const float LINE_STEP_PX = 40.0f;

    gui_node* hit = scene__hit_test(root, x, y);
    for (gui_node* n = hit; n != NULL; n = n->parent)
    {
        gui_overflow oy = n->resolved.overflow_y;
        if (oy != GUI_OVERFLOW_SCROLL && oy != GUI_OVERFLOW_AUTO) { continue; }
        if (n->content_h <= n->bounds.h + 0.5f) { continue; }

        n->scroll_y_target -= delta * LINE_STEP_PX;
        scroll__clamp(n);
        if (n->resolved.scroll_smooth_ms <= 0.0f)
        {
            n->scroll_y = n->scroll_y_target;
        }
        return;
    }
    (void)x; (void)y; (void)delta;
}

gui_node* scene__pressed(void)
{
    return _scene_input_internal__input.pressed;
}

void scene__cancel_press(void)
{
    gui_node* p = _scene_input_internal__input.pressed;
    if (p == NULL) { return; }
    //
    // Walk the press chain back to default state. Mirrors the
    // release branch of scene__on_mouse_button when the up lands
    // off the originally-pressed node -- minus the click dispatch.
    // The next mouse_up sees pressed == NULL and the click is
    // skipped on the underlying widget.
    //
    _scene_input_internal__set_state_chain(p, GUI_STATE_DEFAULT);
    _scene_input_internal__input.pressed = NULL;
}

void scene__on_touch_scroll(int64 x, int64 y, float delta_pixels)
{
    gui_node* root = scene__root();
    if (root == NULL) { return; }

    gui_node* hit = scene__hit_test(root, x, y);
    for (gui_node* n = hit; n != NULL; n = n->parent)
    {
        gui_overflow oy = n->resolved.overflow_y;
        if (oy != GUI_OVERFLOW_SCROLL && oy != GUI_OVERFLOW_AUTO) { continue; }
        if (n->content_h <= n->bounds.h + 0.5f) { continue; }

        n->scroll_y_target -= delta_pixels;
        scroll__clamp(n);
        if (n->resolved.scroll_smooth_ms <= 0.0f)
        {
            n->scroll_y = n->scroll_y_target;
        }
        return;
    }
    (void)x; (void)y; (void)delta_pixels;
}

void scene__on_resize(int64 w, int64 h)
{
    (void)w; (void)h;
}

void scene__set_focus(gui_node* node)
{
    gui_node* prev = _scene_input_internal__input.focus;
    if (prev == node) { return; }

    if (prev != NULL)
    {
        const widget_vtable* pvt = widget_registry__get(prev->type);
        if (pvt != NULL && pvt->on_focus_lost != NULL) { pvt->on_focus_lost(prev); }
    }
    _scene_input_internal__input.focus = node;
    if (node != NULL)
    {
        const widget_vtable* nvt = widget_registry__get(node->type);
        if (nvt != NULL && nvt->on_focus_gained != NULL) { nvt->on_focus_gained(node); }
    }

    if (scene__virtual_keyboard_enabled())
    {
        boole was_text = prev != NULL && (prev->type == GUI_NODE_INPUT || prev->type == GUI_NODE_TEXTAREA);
        boole is_text  = node != NULL && (node->type == GUI_NODE_INPUT || node->type == GUI_NODE_TEXTAREA);
        if      (is_text && !was_text) { scene__show_keyboard(); }
        else if (!is_text && was_text) { scene__hide_keyboard(); }
    }
}

gui_node* scene__focus(void)
{
    return _scene_input_internal__input.focus;
}

void scene__on_char(uint codepoint)
{
    //
    // Redirect hook. Used by meek-shell's IME bridge to forward the
    // codepoint to an external sink (the zwp_text_input_v3 in the
    // foreign app's process) instead of a local widget. Hook returns
    // TRUE if it handled the char; we skip local delivery in that
    // case. FALSE (or hook not set) falls through to the focused
    // widget's on_char vtable.
    //
    if (_scene_input_internal__char_redirect != NULL &&
        _scene_input_internal__char_redirect(codepoint))
    {
        return;
    }
    gui_node* f = _scene_input_internal__input.focus;
    if (f == NULL) { return; }
    const widget_vtable* vt = widget_registry__get(f->type);
    if (vt == NULL || vt->on_char == NULL) { return; }
    vt->on_char(f, codepoint);
}

void scene__on_key(int64 vk, boole down)
{
    gui_node* f = _scene_input_internal__input.focus;
    if (f == NULL) { return; }
    const widget_vtable* vt = widget_registry__get(f->type);
    if (vt == NULL || vt->on_key == NULL) { return; }
    vt->on_key(f, vk, down);
}

void scene__deliver_char(gui_node* node, uint codepoint)
{
    //
    // Redirect hook takes priority over node-targeted delivery too.
    // This is the path widget_keyboard actually uses (it calls
    // scene__deliver_char(focus, ch) after a key tap). With the
    // hook set, keyboard taps forward to the IME bridge regardless
    // of the local focus target.
    //
    if (_scene_input_internal__char_redirect != NULL &&
        _scene_input_internal__char_redirect(codepoint))
    {
        return;
    }
    if (node == NULL) { return; }
    const widget_vtable* vt = widget_registry__get(node->type);
    if (vt == NULL || vt->on_char == NULL) { return; }
    vt->on_char(node, codepoint);
}

void scene__deliver_key(gui_node* node, int64 vk, boole down)
{
    if (node == NULL) { return; }
    const widget_vtable* vt = widget_registry__get(node->type);
    if (vt == NULL || vt->on_key == NULL) { return; }
    vt->on_key(node, vk, down);
}
