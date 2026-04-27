//
// text_input_v3.h -- register the zwp_text_input_manager_v3 global
// on `display`. Called once from globals__register_all.
//
// See text_input_v3.c for what the stub does (currently: accept
// every request, emit no events). Future: wire to meek-shell's
// widget_keyboard as the real input method.
//

#ifndef MEEK_COMPOSITOR_TEXT_INPUT_V3_H
#define MEEK_COMPOSITOR_TEXT_INPUT_V3_H

#include <stdint.h>

struct wl_display;
void text_input_v3__register(struct wl_display* display);

//
// Forward `text` to the currently-active zwp_text_input_v3 resource
// as commit_string + done. Called by meek_shell_v1 when the bound
// shell issues ime_commit_string. No-op if no text_input is active.
//
void text_input_v3__forward_commit_string(const char* text);

#endif
