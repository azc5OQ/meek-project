#ifndef SCENE_H
#define SCENE_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

//
//scene.h - tree, styles, handlers, layout, hit test, input.
//paired with scene.c. all of this is portable c (no os calls).
//
//public functions in this file are prefixed scene__ so call sites
//document the owning translation unit. types stay in the project
//namespace (gui_*) because they're shared.
//

//
//===== color constructors ===================================================
//

/**
 * Build an rgba color from four floats in 0..1.
 *
 * @function scene__rgba
 * @param {float} r - Red component.
 * @param {float} g - Green component.
 * @param {float} b - Blue component.
 * @param {float} a - Alpha component.
 * @return {gui_color} The composed color value.
 */
GUI_API gui_color scene__rgba(float r, float g, float b, float a);

/**
 * Build an rgb color from three floats in 0..1; alpha defaults to 1.0.
 *
 * @function scene__rgb
 * @param {float} r - Red component.
 * @param {float} g - Green component.
 * @param {float} b - Blue component.
 * @return {gui_color} The composed color value, fully opaque.
 */
GUI_API gui_color scene__rgb(float r, float g, float b);

/**
 * Build an rgb color from a packed 0xRRGGBB integer; alpha defaults to 1.0.
 *
 * @function scene__hex
 * @param {uint} rrggbb - 24-bit color, red in the high byte.
 * @return {gui_color} The composed color value, fully opaque.
 */
GUI_API gui_color scene__hex(uint rrggbb);

//
//===== handler registration =================================================
//

/**
 * Register a click handler under the given name. parser_xml resolves
 * on_click="foo" by hashing "foo" and looking it up in the handler table.
 *
 * @function scene__register_handler
 * @param {char*} name - Source-level name of the handler (typically the function's identifier).
 * @param {gui_handler_fn} fn - Function pointer invoked when an event matches.
 * @return {void}
 *
 * Logs a warning and ignores the registration if the table is full.
 */
GUI_API void scene__register_handler(char* name, gui_handler_fn fn);

//
//convenience macro: SCENE_HANDLER(do_thing) expands to
//scene__register_handler("do_thing", do_thing).
//
#define SCENE_HANDLER(fn) scene__register_handler(#fn, fn)

/**
 * Compute the fnv-1a 32-bit hash of a handler-name string. parser_xml uses
 * this to set on_click_hash at parse time so dispatch is a hash compare.
 *
 * @function scene__hash_name
 * @param {char*} name - Null-terminated string to hash.
 * @return {uint} 32-bit fnv-1a hash.
 */
GUI_API uint scene__hash_name(char* name);

//
//===== tree construction ====================================================
//

/**
 * Allocate a zero-initialised node of the given type.
 *
 * @function scene__node_new
 * @param {gui_node_type} type - Node kind.
 * @return {gui_node*} Newly allocated node, or NULL on allocation failure.
 *
 * The caller must eventually call scene__node_free on the root of the
 * subtree the node belongs to.
 */
GUI_API gui_node* scene__node_new(gui_node_type type);

/**
 * Append child to parent's child list and link parent <-> child.
 *
 * @function scene__add_child
 * @param {gui_node*} parent - Parent node.
 * @param {gui_node*} child - Node to append. Must not already be in any tree.
 * @return {void}
 *
 * No-op if either argument is NULL. Fires the child widget's
 * on_attach hook (if any) once the link is established.
 */
GUI_API void scene__add_child(gui_node* parent, gui_node* child);

/**
 * Lazy-alloc accessor for per-node widget state. If the widget's
 * vtable declares state_size > 0 and node->user_data is NULL, this
 * allocates `state_size` zero-initialized bytes via
 * GUI_CALLOC_T(MM_TYPE_GENERIC) and stashes the pointer on
 * node->user_data. Subsequent calls return the cached pointer.
 * scene__node_free auto-frees this allocation AFTER the widget's
 * on_destroy returns. Widgets that prefer to manage user_data
 * themselves can leave state_size = 0 and continue using the
 * legacy `_widget_X_internal__state_of` pattern.
 *
 * @function scene__widget_state
 * @param {gui_node*} n - The node.
 * @return {void*} Pointer to the state struct, or NULL if vtable->state_size is 0 or allocation failed.
 */
GUI_API void* scene__widget_state(gui_node* n);

/**
 * Wire a click handler onto a node by name. The name is hashed once
 * here; the handler must also be registered via scene__register_handler
 * or exported from the host exe with UI_HANDLER for auto-discovery.
 *
 * @function scene__set_on_click
 * @param {gui_node*} node - Target node.
 * @param {char*} handler_name - Handler name to look up at dispatch.
 * @return {void}
 */
GUI_API void scene__set_on_click(gui_node* node, char* handler_name);

/**
 * Wire a change handler onto a node by name. Used for sliders (and any
 * future widget that emits GUI_EVENT_CHANGE).
 *
 * @function scene__set_on_change
 * @param {gui_node*} node - Target node.
 * @param {char*} handler_name - Handler name to look up at dispatch.
 * @return {void}
 */
GUI_API void scene__set_on_change(gui_node* node, char* handler_name);

/**
 * Recursively free a subtree rooted at node.
 *
 * @function scene__node_free
 * @param {gui_node*} node - Root of the subtree (may be NULL).
 * @return {void}
 */
GUI_API void scene__node_free(gui_node* node);

/**
 * Transfer runtime interaction state from `old_tree` into `new_tree`
 * in place, matching nodes by stable key (id first, else parent path
 * + type + sibling index). State that gets moved: scroll offset,
 * slider value, is_open, text content, user_data (widget-specific
 * state). After the call, `old_tree` still owns its topology but its
 * user_data pointers on matched nodes are NULLed so the subsequent
 * scene__node_free(old_tree) does not double-free.
 *
 * Use in hot reload: freeze the focused id, reparse, reconcile,
 * scene__set_root(new), scene__node_free(old), re-focus by id.
 * Without this, every hot reload resets scroll + slider + popup +
 * input text to their .ui declaration, which is jarring in active
 * dev workflows.
 *
 * @function scene__reconcile_tree
 * @param {gui_node*} old_tree - Tree about to be freed. May be NULL.
 * @param {gui_node*} new_tree - Freshly parsed tree that takes over. May be NULL.
 * @return {void}
 */
GUI_API void scene__reconcile_tree(gui_node* old_tree, gui_node* new_tree);

//
//===== root + layout + resolve ==============================================
//

/**
 * Set the active scene root. The previous root is not freed.
 *
 * @function scene__set_root
 * @param {gui_node*} root - New root node, or NULL to clear.
 * @return {void}
 */
GUI_API void scene__set_root(gui_node* root);

/**
 * Get the currently set scene root.
 *
 * @function scene__root
 * @return {gui_node*} Active root node, or NULL if none has been set.
 */
GUI_API gui_node* scene__root(void);

/**
 * Run the layout pass on the active tree using the given viewport.
 *
 * @function scene__layout
 * @param {int64} viewport_w - Viewport width in pixels.
 * @param {int64} viewport_h - Viewport height in pixels.
 * @return {void}
 */
GUI_API void scene__layout(int64 viewport_w, int64 viewport_h);

/**
 * Lay out a single node by dispatching through its widget vtable.
 * Widget layout functions call this to lay out their children.
 *
 * @function scene__layout_node
 * @param {gui_node*} n - Node to lay out (may be NULL; no-op).
 * @param {float} x - Origin x.
 * @param {float} y - Origin y.
 * @param {float} avail_w - Available width.
 * @param {float} avail_h - Available height.
 * @return {void}
 */
GUI_API void scene__layout_node(gui_node* n, float x, float y, float avail_w, float avail_h);

/**
 * Lay out parent's children inside parent's bounds as a vertical
 * column, honoring the parent's resolved padding and gap. Widgets
 * (Window, Column) call this to do the standard column placement
 * without re-implementing the math.
 *
 * @function scene__layout_children_as_column
 * @param {gui_node*} parent - Parent whose children to lay out.
 * @return {void}
 */
GUI_API void scene__layout_children_as_column(gui_node* parent);

/**
 * Horizontal counterpart of scene__layout_children_as_column. Lays
 * each child left-to-right inside parent's bounds, advancing by the
 * child's measured width plus the parent's `gap`. Children receive
 * the full inner-height (parent.h minus pad_t/pad_b) so widgets that
 * auto-size to their content (Text, checkbox, etc.) can pick the
 * size they want; widgets that expand to fill (e.g. explicit size_w)
 * just consume that width and the cursor advances past them.
 *
 * @function scene__layout_children_as_row
 * @param {gui_node*} parent - Parent whose children to lay out.
 * @return {void}
 */
GUI_API void scene__layout_children_as_row(gui_node* parent);

/**
 * Walk the active tree and submit one or more renderer__submit_rect
 * calls per node (one for plain nodes; track + thumb for sliders).
 * Called from platform_win32__tick between begin_frame and end_frame.
 *
 * @function scene__emit_draws
 * @return {void}
 */
GUI_API void scene__emit_draws(void);

/**
 * Set the global UI scale factor. All sizing, padding, gap, and corner
 * radius values are multiplied by this factor before being committed to
 * bounds (layout) or passed to the renderer (radius). A value of 1.0
 * is identity; 2.0 doubles every dimension.
 *
 * @function scene__set_scale
 * @param {float} scale - New scale factor. Clamped to a sensible positive range internally.
 * @return {void}
 */
GUI_API void scene__set_scale(float scale);

/**
 * Read the current global UI scale factor.
 *
 * @function scene__scale
 * @return {float} The currently-set scale factor (1.0 by default).
 */
GUI_API float scene__scale(void);

/**
 * Convert a logical (style-file) pixel count to physical pixels at
 * the current UI scale. Equivalent to `logical * scene__scale()`;
 * named helper so overlay-drawing code (widget_popup, widget_select
 * dropdown, widget_keyboard) doesn't have to remember the
 * multiplication on every dimension. The normal widget layout
 * path already does this implicitly by taking `scale` as an
 * argument; the helper is for code paths outside that pipeline.
 *
 * @function scene__px
 * @param {float} logical - Logical-pixel dimension (same units as .style values).
 * @return {float} Physical-pixel dimension.
 */
GUI_API float scene__px(float logical);

//
// ===== frame timing ========================================================
//
// The platform layer captures a millisecond timestamp at the START of
// each frame and hands it here via scene__begin_frame_time. Every
// time-sensitive subsystem (animator today; future hot-reload, etc.)
// then reads scene__frame_time_ms and scene__frame_delta_ms during
// the frame instead of taking its own clock samples. One source of
// truth, no clock-skew between subsystems within a frame.
//

/**
 * Stamp the start of a new frame with the current monotonic-clock time
 * in milliseconds. Computes frame_delta_ms as (now - previous frame's
 * now), clamping huge jumps (debugger pause, system sleep) so a
 * resumed app doesn't fast-forward animations through to completion.
 *
 * @function scene__begin_frame_time
 * @param {int64} now_ms - Current monotonic time in milliseconds.
 * @return {void}
 */
GUI_API void  scene__begin_frame_time(int64 now_ms);

/**
 * Read the timestamp captured by the most recent scene__begin_frame_time
 * call. Stable for the entire frame.
 *
 * @function scene__frame_time_ms
 * @return {int64} Monotonic milliseconds, or 0 before the first frame.
 */
GUI_API int64 scene__frame_time_ms(void);

/**
 * Read the elapsed milliseconds since the previous frame's
 * scene__begin_frame_time. Animation code multiplies this by their own
 * speed parameters to advance state. Always positive; clamped to a
 * sensible upper bound to avoid debugger-pause weirdness.
 *
 * @function scene__frame_delta_ms
 * @return {int64} Milliseconds since previous frame, clamped to [0, 100].
 */
GUI_API int64 scene__frame_delta_ms(void);

/**
 * Switch the scene clock into deterministic mode. When `step_ms`
 * is > 0, scene__begin_frame_time IGNORES the wall-clock value it
 * receives from the platform and instead advances by exactly
 * `step_ms` each tick. When `step_ms` is 0, deterministic mode is
 * off and the clock tracks real time.
 *
 * Used by the visual-regression test runner: different graphics
 * backends present at different rates, so 8 frames of wall-clock
 * time varies substantially across backends. Deterministic mode
 * lets the runner say "every tick is 16 ms regardless of what the
 * GPU / vsync did", so an animation captured at frame N lands on
 * exactly the same progress on every backend. No effect on normal
 * host apps (they don't call this).
 *
 * @function scene__set_deterministic_clock
 * @param {int64} step_ms - Fixed ms increment per tick, or 0 to
 *                          revert to wall-clock tracking.
 * @return {void}
 */
GUI_API void scene__set_deterministic_clock(int64 step_ms);

//
// ===== overlay (top-most popup layer) =====================================
//
// A single global "overlay" slot used for widgets that need to draw ABOVE
// the rest of the scene -- currently just <select>'s dropdown popup, but
// the same mechanism would suit modal dialogs, tooltips, autocomplete
// menus, context menus, etc. Only ONE overlay can be active at a time
// (matches how OS dropdowns work -- opening a second auto-closes the
// first).
//
// Set: widget calls scene__set_overlay(self, popup_bounds, draw_fn) when
//      it opens. Bounds tell scene's hit-test which screen region is
//      "on top" so clicks inside that region route to the overlay node
//      first instead of falling through to whatever's underneath.
// Clear: scene__set_overlay(NULL, ..., NULL) closes the overlay.
// Draw: after the normal emit_draws walk completes, scene calls draw_fn
//       with the overlay node + scale, so the popup paints on top of
//       everything in the tree.
//

/**
 * Function pointer for the overlay draw callback. Receives the overlay
 * node + the global UI scale; called by scene__emit_draws after the
 * normal recursive walk is done.
 */
typedef void (*gui_overlay_draw_fn)(gui_node* node, float scale);

/**
 * Open (or close, when node is NULL) a top-most overlay. Subsequent
 * scene__hit_test calls return `node` for any cursor position inside
 * `bounds`; scene__emit_draws calls `draw_fn(node, scale)` after the
 * normal tree walk so the overlay paints on top of everything.
 *
 * @function scene__set_overlay
 * @param {gui_node*} node - Owning widget; passed back to draw_fn and returned by hit-test inside bounds. NULL closes the overlay.
 * @param {gui_rect} bounds - Screen rect the overlay covers. Hit-test uses this to determine "is this click on the popup or below it?".
 * @param {gui_overlay_draw_fn} draw_fn - Called once per frame after the main draw walk. NULL closes the overlay.
 * @return {void}
 */
GUI_API void scene__set_overlay(gui_node* node, gui_rect bounds, gui_overlay_draw_fn draw_fn);

/**
 * Read the last viewport size passed to scene__layout. Written each
 * frame; popups / overlays use this to center themselves.
 *
 * @function scene__viewport
 * @param {int64*} out_w - Receives width in pixels. May be NULL.
 * @param {int64*} out_h - Receives height in pixels. May be NULL.
 * @return {void}
 */
GUI_API void scene__viewport(int64* out_w, int64* out_h);

/**
 * Translate a node and its entire descendant subtree by (dx, dy).
 * Pure bounds shift -- no re-measure, no widget vtable calls. Used
 * by layout helpers to apply alignment offsets after a child has
 * already been laid out.
 *
 * @function scene__shift_tree
 * @param {gui_node*} n - Subtree root to move.
 * @param {float} dx - Pixel shift on X.
 * @param {float} dy - Pixel shift on Y.
 * @return {void}
 */
GUI_API void scene__shift_tree(gui_node* n, float dx, float dy);

/**
 * Resolve a node's effective pixel width from its style, honoring
 * (in priority order): width_pct -> size_w*scale -> fallback to
 * avail_w. Widgets call this at the top of their layout function
 * instead of reading style->size_w directly, so `width: 100%` in
 * a .style file actually scales with the parent container.
 *
 * @function scene__layout_width
 * @param {gui_node*} n - Node being laid out.
 * @param {float} avail_w - Width available from the parent (the fallback).
 * @param {float} scale - Global UI scale (pass scene__scale()).
 * @return {float} Pixel width to assign to n->bounds.w.
 */
GUI_API float scene__layout_width(gui_node* n, float avail_w, float scale);

/** @function scene__layout_height -- mirror of scene__layout_width for the Y axis. */
GUI_API float scene__layout_height(gui_node* n, float avail_h, float scale);

/**
 * Host-side color overrides applied AFTER style resolve + animator
 * each frame. Survive rule re-registration and hot reload (neither
 * touches the override flags), unlike direct mutations to
 * node->style[] which the resolver's memset wipes. Clear by
 * explicitly calling scene__clear_background_color_override.
 * Typical use: theme toggles that want to recolor specific nodes
 * at runtime.
 *
 * @function scene__set_background_color_override
 * @param {gui_node*} n - Target node.
 * @param {gui_color} c - Replaces both resolved.background_color and has_background_color.
 * @return {void}
 */
GUI_API void scene__set_background_color_override(gui_node* n, gui_color c);

/** @function scene__clear_background_color_override */
GUI_API void scene__clear_background_color_override(gui_node* n);

/** @function scene__set_font_color_override -- same shape for text color. */
GUI_API void scene__set_font_color_override(gui_node* n, gui_color c);

/** @function scene__clear_font_color_override */
GUI_API void scene__clear_font_color_override(gui_node* n);

/**
 * Emit the default "bg rect + optional shadow + optional gradient"
 * triple for a node. Widgets whose emit_draws overrides still want
 * this common visual treatment can call this instead of duplicating
 * the logic. Honors the node's effective_opacity so every submitted
 * color alpha is multiplied by the accumulated parent-chain opacity.
 *
 * @function scene__emit_default_bg
 * @param {gui_node*} n - Node whose background decorations to emit.
 * @param {float} scale - Global UI scale (pass scene__scale()).
 * @return {void}
 */
GUI_API void scene__emit_default_bg(gui_node* n, float scale);

/**
 * Lay out one position:absolute (or :fixed) child against the given
 * content rect using its inset_l/t/r/b style fields. Public so widgets
 * with their own child-iteration loops (widget_window, widget_div)
 * can defer to the same logic that scene__layout_children_as_column
 * uses internally.
 *
 * @function scene__layout_absolute_child
 * @param {gui_node*} parent - The containing block (passed for context; bounds read from caller).
 * @param {gui_node*} c - The absolute / fixed child to position.
 * @param {float} content_x - Left edge of containing block in panel-space px.
 * @param {float} content_y - Top edge of containing block in panel-space px.
 * @param {float} content_w - Width of containing block in panel-space px.
 * @param {float} content_h - Height of containing block in panel-space px.
 * @return {void}
 */
GUI_API void scene__layout_absolute_child(gui_node* parent, gui_node* c, float content_x, float content_y, float content_w, float content_h);

/**
 * Draw a border around `bounds` per the node's resolved
 * border_width / border_color / border_style. No-op when any of
 * border_width <= 0, border_style == GUI_BORDER_NONE, or
 * has_border_color is FALSE -- a partial declaration
 * (`border-width: 2;` with no color) silently doesn't paint.
 *
 * Implementation: solid borders use the SDF "outer rect in border
 * color, inner rect at smaller bounds in bg color" inset trick,
 * which composes naturally with rounded corners (inner radius =
 * outer radius - border_width). Dashed and dotted styles parse
 * but currently render as solid; per-edge stipple is a later
 * pass.
 *
 * Color alpha is multiplied by `n->effective_opacity` so the
 * border fades with parent-chain opacity animations.
 *
 * @function scene__emit_border
 * @param {gui_node*} n - Source of the resolved border style.
 * @param {gui_rect} bounds - Where to draw (typically n->bounds).
 * @param {float} scale - Global UI scale (pass scene__scale()).
 * @return {void}
 */
GUI_API void scene__emit_border(gui_node* n, gui_rect bounds, float scale);

/**
 * Effective scaled border thickness for a node, or 0.0f when the
 * node has no visible border. Encodes the same three-way gate as
 * scene__emit_border (border_width > 0, has_border_color set,
 * border_style != NONE) so layout code can ask "how much room
 * does the border eat?" without re-implementing the rule. Returns
 * `border_width * scale` when the gate passes.
 *
 * Container widgets (window/div/column/row/collection) call this
 * to shrink their inner content area and shift the child origin
 * so children sit INSIDE the border line, not under it.
 *
 * @function scene__border_width
 * @param {gui_node*} n - Node whose resolved border style to read.
 * @param {float} scale - Global UI scale (pass scene__scale()).
 * @return {float} Pixel border width, or 0.0f when no border applies.
 */
GUI_API float scene__border_width(gui_node* n, float scale);

/**
 * Submit a textured image rect fitted into `bounds` per the
 * gui_image_fit mode. Shared between <image>'s own emit_draws and
 * any container's background-image rendering so the fit math lives
 * in one place.
 *
 *   FILL     stretch across `bounds` (breaks aspect ratio).
 *   CONTAIN  uniform scale to fit inside, centered, letterboxed.
 *   COVER    uniform scale to cover, centered; excess is scissored.
 *   NONE     natural pixel size, centered; scissored if bigger.
 *
 * `tex` may be NULL (renderer backend without an image pipeline);
 * this function falls back to a muted gradient placeholder so the
 * slot is at least visible.
 *
 * @function scene__submit_fitted_image
 * @param {gui_rect} bounds - Destination rect in pixels.
 * @param {void*} tex - Texture from renderer__create_texture_rgba / widget_image__cache_get_or_load. NULL = placeholder.
 * @param {int} natural_w - Image natural width in pixels.
 * @param {int} natural_h - Image natural height in pixels.
 * @param {int} fit - gui_image_fit value (cast to int for ABI stability).
 * @param {gui_color} tint - Multiplied into sampled RGBA. Pass (1,1,1,1) for "draw as-is".
 * @param {float} radius - Corner radius for the placeholder rect. Ignored for the textured path.
 * @return {void}
 */
GUI_API void scene__submit_fitted_image(gui_rect bounds, void* tex, int natural_w, int natural_h, int fit, gui_color tint, float radius);

/**
 * Imperative popup helpers. Both are wrappers around a shared
 * internal popup implementation: they build a minimal popup node,
 * install it as the scene overlay, and fire the supplied handler
 * when the user dismisses it. Handler receives ev->popup.confirmed
 * (TRUE for OK / Yes, FALSE for Cancel / No) and, for option-select,
 * ev->popup.index.
 *
 * @function scene__popup_ok
 * @param {char*} message - Body text. Copied internally.
 * @param {char*} handler_name - Registered handler name; NULL for fire-and-forget.
 * @return {void}
 */
GUI_API void scene__popup_ok(char* message, char* handler_name);

/**
 * Yes/No confirmation popup. Pairs with scene__popup_ok; see above.
 * @function scene__popup_confirm
 */
GUI_API void scene__popup_confirm(char* message, char* handler_name);

/**
 * On-screen virtual keyboard. Builds a <keyboard> node, attaches it
 * as a direct child of the scene root, and lets it draw + handle
 * input at the bottom 40% of the viewport until dismissed. Pairs
 * with scene__hide_keyboard().
 *
 * Intended for touch-only targets. On Android the platform layer
 * can auto-show the keyboard when a text-editing widget takes focus
 * if scene__set_virtual_keyboard_enabled was called with TRUE.
 * Windows is opt-in: the host must explicitly call these helpers,
 * or flip the enabled flag on for hardware-keyboard-less setups.
 *
 * @function scene__show_keyboard
 * @return {void}
 */
GUI_API void scene__show_keyboard(void);

/** @function scene__hide_keyboard -- remove the keyboard from the scene. */
GUI_API void scene__hide_keyboard(void);

/**
 * Opt in/out of automatic keyboard management. When TRUE, scene will
 * show the keyboard the next time an <input> / <textarea> takes
 * focus and hide it when focus leaves. Defaults TRUE on Android,
 * FALSE on Windows. Host apps that want to force-show on Windows
 * (e.g. a kiosk with only touch input) pass TRUE.
 *
 * @function scene__set_virtual_keyboard_enabled
 * @param {boole} enabled
 * @return {void}
 */
GUI_API void scene__set_virtual_keyboard_enabled(boole enabled);

/** @function scene__virtual_keyboard_enabled -- read the flag. */
GUI_API boole scene__virtual_keyboard_enabled(void);

/**
 * Route a key / character event to a specific node. Used by the
 * on-screen keyboard to deliver taps to whichever node currently
 * holds focus. Bypasses the normal platform input dispatcher.
 *
 * @function scene__deliver_char
 * @param {gui_node*} node - Target. NULL is a no-op.
 * @param {uint} codepoint - Unicode codepoint (ASCII/Latin-1 for now).
 * @return {void}
 */
GUI_API void scene__deliver_char(gui_node* node, uint codepoint);

/** @function scene__deliver_key -- VK-code key event to a specific node. */
GUI_API void scene__deliver_key(gui_node* node, int64 vk, boole down);

/**
 * Character-input redirect. When set to a non-NULL function, every
 * call to scene__on_char / scene__deliver_char first passes the
 * codepoint to the redirect. If the redirect returns TRUE, the
 * normal widget vtable delivery is skipped; FALSE falls through.
 *
 * Use case: meek-shell's IME bridge. When a third-party Wayland
 * client requests an IME via zwp_text_input_v3, meek-shell installs
 * a redirect that forwards codepoints through the meek_shell_v1
 * extension back to the compositor, which emits commit_string to
 * the focused text_input. Clears by passing NULL.
 *
 * @function scene__set_char_redirect
 * @param {scene_char_redirect_fn} fn - NULL = disabled (default).
 */
typedef boole (*scene_char_redirect_fn)(uint codepoint);
GUI_API void  scene__set_char_redirect(scene_char_redirect_fn fn);

/**
 * Query whether a char redirect is currently installed.
 *
 * @function scene__has_char_redirect
 * @return {boole} TRUE iff a non-null redirect was set via scene__set_char_redirect.
 *
 * Widgets that emit non-character keys (backspace, return) use this
 * to pick between two dispatch modes:
 *   * Redirect present -> convert the key to its ASCII equivalent
 *     (0x08 for backspace, 0x0D for return), call scene__on_char(ch).
 *     The redirect consumes it and can forward to a foreign sink
 *     (e.g. wl_keyboard.key via meek_shell_v1).
 *   * Redirect absent -> call scene__on_key(vk, down) so the locally
 *     focused widget (input / textarea) handles it in its on_key
 *     vtable, which is where VK_BACK / VK_RETURN live in the meek-ui
 *     widget contract.
 */
GUI_API boole scene__has_char_redirect(void);

/**
 * Active keyboard modifier bitmask, pushed by widget_keyboard
 * right before scene__on_char / scene__on_key, consumed by char
 * redirect callbacks that want to forward the active ctrl / shift
 * state along with the character (terminal keybindings: ctrl-C,
 * ctrl-D, etc.).
 *
 * Bit layout matches wl_keyboard.modifiers:
 *   1 << 0  shift (depressed)
 *   1 << 2  ctrl  (depressed)
 * Other bits (caps, alt, mod3/4/5) reserved for future use and
 * should be zero for now.
 *
 * @function scene__set_active_modifiers
 * @function scene__get_active_modifiers
 * @return {uint} bitmask of depressed modifiers at the time of the
 *   last scene__on_char / scene__on_key call
 *
 * Lifecycle: widget_keyboard writes the mask before every
 * dispatch. The char redirect reads it synchronously during the
 * scene__on_char callback. After dispatch, the widget's own logic
 * decides whether to latch (leave bit set for next char) or clear
 * (single-shot, typical after shift-A then 'b' = unshifted).
 */
GUI_API void  scene__set_active_modifiers(uint mask);
GUI_API uint  scene__get_active_modifiers(void);

/**
 * Read the currently active overlay node, or NULL if no overlay is set.
 *
 * @function scene__overlay_node
 * @return {gui_node*} The overlay's owner node, or NULL.
 */
GUI_API gui_node* scene__overlay_node(void);

/**
 * Dispatch a click event to a node's on_click handler. Resolves the
 * handler through the registration table + symbol-resolver fallback
 * and caches on first hit. Used by scene's input state machine and
 * by widgets that programmatically fire clicks.
 *
 * @function scene__dispatch_click
 * @param {gui_node*} n - Sender node.
 * @param {int64} x - Cursor x at release.
 * @param {int64} y - Cursor y at release.
 * @param {int64} button - Button index (0 = left).
 * @return {void}
 */
GUI_API void scene__dispatch_click(gui_node* n, int64 x, int64 y, int64 button);

/**
 * Look up a registered handler by name (FNV-1a of the name; same
 * hashing as on_click) and invoke it with a zero-initialized event
 * (ev->sender = NULL). Used for window-level events that don't
 * target a specific node -- notably gesture recognition
 * (`on_swipe_up_bottom` isn't a click on any particular widget).
 * If no handler with this name is registered, the call is a cheap
 * no-op.
 *
 * @function scene__dispatch_gesture_by_name
 * @param {const char*} handler_name - Name registered via
 *   UI_HANDLER or scene__register_handler.
 */
GUI_API void scene__dispatch_gesture_by_name(const char* handler_name);

/**
 * Dispatch a change event to a node's on_change handler with a
 * scalar payload. Used by sliders and any future widget that emits
 * GUI_EVENT_CHANGE.
 *
 * @function scene__dispatch_change
 * @param {gui_node*} n - Sender node.
 * @param {float} new_value - The new scalar value to deliver via ev->change.scalar.
 * @return {void}
 */
GUI_API void scene__dispatch_change(gui_node* n, float new_value);

/**
 * Dispatch a prebuilt event. Lets widgets attach payloads that don't
 * fit the scalar/click model (e.g. colorpicker emits a gui_color via
 * ev->color.value; popup emits ev->popup.confirmed). Resolves the
 * handler via the same table + symbol-resolver fallback as
 * scene__dispatch_change / scene__dispatch_click.
 *
 * @function scene__dispatch_event
 * @param {gui_node*} n - Sender node; handler is looked up from its on_click / on_change slot based on ev->type.
 * @param {gui_event*} ev - Event to deliver. `sender` is overwritten.
 * @return {void}
 */
GUI_API void scene__dispatch_event(gui_node* n, gui_event* ev);

/**
 * Apply registered selectors to per-state slots, then resolve the
 * active state into each node's `resolved` style.
 *
 * @function scene__resolve_styles
 * @return {void}
 */
GUI_API void scene__resolve_styles(void);

/**
 * Find the deepest node whose bounds contain the given point.
 *
 * @function scene__hit_test
 * @param {gui_node*} root - Subtree to search.
 * @param {int64} x - Pixel x, top-left origin.
 * @param {int64} y - Pixel y, top-left origin.
 * @return {gui_node*} The deepest matching node, or NULL if no node contains the point.
 */
GUI_API gui_node* scene__hit_test(gui_node* root, int64 x, int64 y);

/**
 * Walk the current scene tree and return the first node whose id
 * matches the given string. NULL if no match. O(n) in tree size --
 * cheap for PoC-scale scenes; callers that query often should cache
 * the returned pointer across frames since tree identity is stable
 * between hot reloads.
 *
 * @function scene__find_by_id
 * @param {char*} id - id string to look up (case-sensitive).
 * @return {gui_node*} The matching node, or NULL.
 */
GUI_API gui_node* scene__find_by_id(char* id);

//
//===== input callbacks =====================================================
//
//called by the platform layer when raw input arrives. updates the
//hover/pressed state machine and dispatches click events.
//

/**
 * Update the hover-state machine for a new mouse position.
 *
 * @function scene__on_mouse_move
 * @param {int64} x - Pixel x.
 * @param {int64} y - Pixel y.
 * @return {void}
 */
GUI_API void scene__on_mouse_move(int64 x, int64 y);

/**
 * Update the pressed-state machine for a mouse button event. On
 * release-on-same-node, dispatches the bound click handler.
 *
 * @function scene__on_mouse_button
 * @param {int64} button - Button index (0 = left).
 * @param {boole} down - TRUE for press, FALSE for release.
 * @param {int64} x - Pixel x.
 * @param {int64} y - Pixel y.
 * @return {void}
 */
GUI_API void scene__on_mouse_button(int64 button, boole down, int64 x, int64 y);

/**
 * Deliver a mouse-wheel (or trackpad vertical scroll) event. The
 * scene walks from the node under (x, y) upward to find the nearest
 * ancestor with overflow_y set to scroll or auto AND content larger
 * than its bounds; that ancestor's scroll_y_target is bumped by
 * -delta * line_step (delta is positive when the wheel moves AWAY
 * from the user; we subtract so content scrolls UP in that case,
 * matching every browser + OS convention). If no scrollable ancestor
 * exists the event is silently ignored -- same as a browser.
 *
 * delta is typically +/-1.0 per wheel detent; fractional values come
 * from high-resolution wheels and trackpads and are passed through.
 *
 * @function scene__on_mouse_wheel
 * @param {int64} x - Pixel x of the wheel event.
 * @param {int64} y - Pixel y of the wheel event.
 * @param {float} delta - Normalized wheel delta (positive = scroll up / away from user).
 * @return {void}
 */
GUI_API void scene__on_mouse_wheel(int64 x, int64 y, float delta);

/**
 * Instant scroll from a touch drag. Identical to on_mouse_wheel in
 * that it walks up from the hit node to the nearest scrollable
 * ancestor, but bypasses scroll-smooth lerping: both scroll_y and
 * scroll_y_target are updated together so the container follows
 * the finger 1:1 with no lag. Used by the platform touch handler.
 *
 * @function scene__on_touch_scroll
 * @param {int64} x - Finger x in pixels.
 * @param {int64} y - Finger y in pixels.
 * @param {float} delta_pixels - Finger travel in pixels since last move. Positive = finger moved DOWN (scroll goes to earlier content; scroll_y decreases).
 * @return {void}
 */
GUI_API void scene__on_touch_scroll(int64 x, int64 y, float delta_pixels);

/**
 * Return the currently pressed node -- the one that received the
 * most recent mouse/touch DOWN and hasn't been released yet. NULL
 * when nothing is pressed. Used by the platform touch layer to ask
 * "should I convert this finger drag into a scroll, or leave it on
 * the pressed widget?" -- see the captures_drag flag on
 * widget_vtable.
 *
 * @function scene__pressed
 * @return {gui_node*}
 */
GUI_API gui_node* scene__pressed(void);

/**
 * Cancel the current press without dispatching `on_click`. Drops the
 * pressed slot to NULL and walks the press chain back to default
 * state. Use when host code decides to take over the touch midway
 * through (custom drag, long-press promotion, modal takeover, etc.) --
 * the next `mouse_up` sees `pressed == NULL` and skips the
 * normally-scheduled click on the underlying widget.
 *
 * Idempotent: no-op if no node is currently pressed.
 *
 * @function scene__cancel_press
 * @return {void}
 */
GUI_API void scene__cancel_press(void);

/**
 * Notification that the platform viewport changed size. Currently a
 * no-op (layout re-runs every frame); reserved for dirty-tracking.
 *
 * @function scene__on_resize
 * @param {int64} w - New viewport width.
 * @param {int64} h - New viewport height.
 * @return {void}
 */
GUI_API void scene__on_resize(int64 w, int64 h);

/**
 * Deliver a character typed by the user to the currently-focused
 * node's widget vtable (on_char hook). Called by platform_win32 on
 * WM_CHAR. No-op if no node is focused or if the focused widget
 * doesn't define on_char.
 *
 * @function scene__on_char
 * @param {uint} codepoint - Unicode codepoint (ASCII+Latin-1 subset for now).
 * @return {void}
 */
GUI_API void scene__on_char(uint codepoint);

/**
 * Deliver a non-character key event (BACKSPACE, ENTER, arrows, ...)
 * to the focused node's widget. Called by platform_win32 on
 * WM_KEYDOWN / WM_KEYUP.
 *
 * @function scene__on_key
 * @param {int64} vk - Win32 virtual-key code.
 * @param {boole} down - TRUE on press, FALSE on release.
 * @return {void}
 */
GUI_API void scene__on_key(int64 vk, boole down);

/**
 * Set which node receives keyboard input. Pass NULL to clear focus.
 * The previously-focused node is automatically de-focused.
 *
 * @function scene__set_focus
 * @param {gui_node*} node - Node to focus, or NULL.
 * @return {void}
 */
GUI_API void scene__set_focus(gui_node* node);

/**
 * Get the currently-focused node, or NULL.
 *
 * @function scene__focus
 * @return {gui_node*} The focused node, or NULL.
 */
GUI_API gui_node* scene__focus(void);

//
//===== selector-based style registration ===================================
//
//stand-in for the .style file parser. supported selector forms:
//    "Type"                  any node of that type
//    ".class"                any node with this class token
//    "#id"                   the unique node with this id
//    "Type.class"            both must match
//    "Type#id"               both must match
//    "<base>:hover"          applies to GUI_STATE_HOVER
//    "<base>:pressed"        applies to GUI_STATE_PRESSED
//    "<base>:disabled"       applies to GUI_STATE_DISABLED
//    "<base>:default"        applies to GUI_STATE_DEFAULT
//
//precedence within one state:
//    id beats class beats type. within one tier, last registered wins.
//

/**
 * Register a style under a selector. The style is copied; the caller
 * may free or reuse its source after the call returns.
 *
 * @function scene__register_style
 * @param {char*} selector - Selector text (see header comment for forms).
 * @param {const gui_style*} style - Style payload to apply when the selector matches.
 * @return {void}
 *
 * Logs and ignores the registration on selector-parse failure or when
 * the rule table is full.
 */
GUI_API void scene__register_style(char* selector, const gui_style* style);

/**
 * Forget every previously registered style.
 *
 * @function scene__clear_styles
 * @return {void}
 *
 * Useful for hot-reloading the .style file: clear, reparse, re-register.
 */
GUI_API void scene__clear_styles(void);

//
//===== symbol resolver hook ================================================
//
//handler resolution falls back through this hook when the registration
//table misses, so that handlers exported by the host exe (UI_HANDLER)
//can be discovered by name without a per-handler scene__register_handler
//call. the resolver is platform-specific (Win32 wires
//GetProcAddress(GetModuleHandleW(NULL), name) during platform_win32__init);
//scene.c never sees windows.h.
//

typedef gui_handler_fn (*gui_symbol_resolver_fn)(char* name);

/**
 * Install a host-symbol resolver. The resolver is invoked when the
 * registration table has no entry for a click target's hash; on
 * success the result is cached back into the table so subsequent
 * dispatches skip the resolver entirely.
 *
 * @function scene__set_symbol_resolver
 * @param {gui_symbol_resolver_fn} resolver - Function that maps a handler name to a function pointer, or NULL to disable the fallback.
 * @return {void}
 */
GUI_API void scene__set_symbol_resolver(gui_symbol_resolver_fn resolver);

#endif
