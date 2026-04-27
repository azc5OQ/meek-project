#ifndef WIDGET_PROCESS_WINDOW_H
#define WIDGET_PROCESS_WINDOW_H

//
// widget_process_window.h - public API for the <process-window/>
// widget. Consumed by meek-shell (the one consumer we expect) to
// attach the live texture of an external process's rendered buffer
// to a specific scene node.
//
// The widget itself (widget_process_window.c) lives in meek-ui
// because it's a regular meek-ui widget with a vtable; its
// TEXTURE SOURCE is external (the shell's toplevel_registry), which
// is why the setters here exist rather than a load-from-path
// attribute like widget_image uses.
//

#include <stdint.h>

#include "../gui_api.h"

struct gui_node;

/**
 * Register the <process-window> widget type with meek-ui. Must be
 * called once at startup, alongside the other widget_*__register
 * entries. meek-ui's widget_registry__register_all (or the host's
 * equivalent) fires this -- same place all other widget types get
 * wired.
 *
 * @function widget_process_window__register
 */
GUI_API void widget_process_window__register(void);

/**
 * Attach (or replace) the live texture for this process-window
 * node. `gl_texture` is a raw GL texture name (GLuint cast to
 * uint32_t to avoid GL headers leaking into this .h). The texture
 * MUST be bound in the same EGL context as meek-ui's renderer --
 * meek-shell imports buffers via the platform's EGL display/context
 * exactly so this is true.
 *
 * The widget does NOT take ownership. Caller (meek-shell's
 * toplevel_registry) must glDeleteTextures when the source process
 * exits. Passing `gl_texture == 0` signals "no texture available";
 * the widget renders its bg rect only.
 *
 * Safe to call every frame (60 Hz commits from the source process
 * end up here via meek_shell_v1_client.c's on_toplevel_buffer).
 *
 * @function widget_process_window__set_texture
 * @param n          target node; must have type GUI_NODE_PROCESS_WINDOW.
 * @param gl_texture GL texture name; 0 = unset.
 * @param width      source pixel width (used by layout for natural sizing).
 * @param height     source pixel height.
 */
GUI_API void widget_process_window__set_texture(struct gui_node* n, uint32_t gl_texture, int width, int height);

/**
 * Read back the meek_shell_v1 handle stored on this widget. Used by
 * the shell's scene-update logic to find the right node for a given
 * handle when an import arrives.
 *
 * Returns 0 if the node isn't a process-window or has no handle set.
 *
 * @function widget_process_window__get_handle
 */
GUI_API uint32_t widget_process_window__get_handle(struct gui_node* n);

/**
 * Set the meek_shell_v1 handle this widget tracks. Same thing the
 * `handle="N"` .ui attribute does, but available at runtime so the
 * shell can spawn process-window nodes dynamically (one per
 * toplevel_added event from the compositor).
 *
 * @function widget_process_window__set_handle
 */
GUI_API void widget_process_window__set_handle(struct gui_node* n, uint32_t handle);

#endif
