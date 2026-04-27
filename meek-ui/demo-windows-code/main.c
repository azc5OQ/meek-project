//
// demo-windows-code/main.c
//
// Alternative Windows host: builds the scene entirely in C. No .ui,
// no .style, no parser, no hot reload. Demonstrates that the parser
// path is optional -- the library's scene graph + style resolver +
// widget vtable dispatch + animator are all reachable from plain C.
//
// Sibling of demo-windows/ (which loads .ui / .style and hot-reloads
// them). Linked against the same gui.dll. Same handler auto-discovery
// via UI_HANDLER + GetProcAddress.
//
// HOW IT WORKS
// ------------
// Scene construction:
//   - scene__node_new(GUI_NODE_*) allocates a node of the given type.
//   - scene__add_child(parent, child) links it into the tree.
//   - direct writes to node->style[GUI_STATE_DEFAULT].background_color / .has_background_color /
//     .radius / .pad_* / .size_* / ... set per-state style.
//   - scene__set_on_click(node, "name") attaches a click handler.
//   - scene__set_root(root) hands the whole tree over.
//
// Style resolver:
//   scene.c's resolver skips its per-tick memset when no CSS rules are
//   registered, so programmatic style[] writes survive across frames.
//   With no parser_style__load_styles call, we never register rules,
//   so the memset stays off and our inline styles work.
//
// Animator:
//   Writing style[].appear_ms = 600 + appear_easing = GUI_EASING_EASE_OUT
//   on window cascades to every descendant via the standard inheritance
//   path (color / font-family / appear are the inherited properties).
//   Everything fades in on first render.
//

#include <stdlib.h>
#include <string.h>

#include "gui_api.h"
#include "types.h"
#include "gui.h"
#include "scene.h"
#include "platform.h"
#include "third_party/log.h"

//
// Handlers. Same UI_HANDLER pattern as demo-windows -- the library
// looks them up by name via GetProcAddress(GetModuleHandleW(NULL), ...)
// the first time a node with that handler name fires.
//
UI_HANDLER void on_code_primary_click(gui_event* ev)
{
    (void)ev;
    log_info("[code-demo] primary clicked");
}

UI_HANDLER void on_code_ghost_click(gui_event* ev)
{
    (void)ev;
    log_info("[code-demo] ghost clicked");
}

UI_HANDLER void on_code_scale_change(gui_event* ev)
{
    scene__set_scale(ev->change.scalar);
    log_info("[code-demo] scale = %.3f", (double)ev->change.scalar);
}

//
// Small helpers that read naturally at call sites. All they do is
// open up node->style[GUI_STATE_DEFAULT] and set one field + its
// has_* companion flag where required.
//
static void _set_bg(gui_node* n, gui_color c)
{
    n->style[GUI_STATE_DEFAULT].background_color     = c;
    n->style[GUI_STATE_DEFAULT].has_background_color = TRUE;
}

static void _set_fg(gui_node* n, gui_color c)
{
    n->style[GUI_STATE_DEFAULT].accent_color     = c;
    n->style[GUI_STATE_DEFAULT].has_accent_color = TRUE;
}

static void _set_color(gui_node* n, gui_color c)
{
    n->style[GUI_STATE_DEFAULT].font_color     = c;
    n->style[GUI_STATE_DEFAULT].has_font_color = TRUE;
}

static void _set_size(gui_node* n, float w, float h)
{
    n->style[GUI_STATE_DEFAULT].size_w = w;
    n->style[GUI_STATE_DEFAULT].size_h = h;
}

static void _set_pad(gui_node* n, float p)
{
    n->style[GUI_STATE_DEFAULT].pad_t = p;
    n->style[GUI_STATE_DEFAULT].pad_r = p;
    n->style[GUI_STATE_DEFAULT].pad_b = p;
    n->style[GUI_STATE_DEFAULT].pad_l = p;
}

static void _set_text(gui_node* n, char* text)
{
    size_t len = 0;
    while (text[len] != 0 && len + 1 < sizeof(n->text))
    {
        n->text[len] = text[len];
        len++;
    }
    n->text[len] = 0;
    n->text_len  = (int64)len;
}

static void _set_font_family(gui_node* n, char* family)
{
    size_t i = 0;
    while (family[i] != 0 && i + 1 < sizeof(n->style[GUI_STATE_DEFAULT].font_family))
    {
        n->style[GUI_STATE_DEFAULT].font_family[i] = family[i];
        i++;
    }
    n->style[GUI_STATE_DEFAULT].font_family[i] = 0;
}

//
// Tree builder. Top-down: window → card → (heading, slider, button row)
//                                    → info column → (label, value)
// Each node is allocated, styled in place, linked to its parent.
//
static gui_node* _build_scene(void)
{
    gui_color bg_page    = scene__hex(0x0a0a0d);
    gui_color bg_card    = scene__hex(0x14151b);
    gui_color bg_primary = scene__hex(0x6366f1);
    gui_color bg_primary_hover   = scene__hex(0x818cf8);
    gui_color bg_primary_pressed = scene__hex(0x4f46e5);
    gui_color bg_ghost   = scene__hex(0x2a2f38);
    gui_color bg_ghost_hover   = scene__hex(0x3a4150);
    gui_color bg_ghost_pressed = scene__hex(0x20242b);
    gui_color text_light = scene__hex(0xe5e7eb);
    gui_color text_dim   = scene__hex(0x9ca3af);
    gui_color thumb      = scene__hex(0x818cf8);
    gui_color track      = scene__hex(0x1f2129);

    gui_node* window = scene__node_new(GUI_NODE_WINDOW);
    _set_bg(window, bg_page);
    _set_color(window, text_light);
    _set_font_family(window, "Roboto-Regular");
    window->style[GUI_STATE_DEFAULT].font_size     = 14.0f;
    //
    // Whole-page fade-in. appear_ms is inherited by every descendant
    // (like font-family / color / font-size), so the entire tree fades
    // in together over 600 ms.
    //
    window->style[GUI_STATE_DEFAULT].appear_ms     = 600.0f;
    window->style[GUI_STATE_DEFAULT].appear_easing = GUI_EASING_EASE_OUT;
    _set_pad(window, 24.0f);

    gui_node* card = scene__node_new(GUI_NODE_COLUMN);
    _set_bg(card, bg_card);
    card->style[GUI_STATE_DEFAULT].radius = 12.0f;
    _set_pad(card, 24.0f);
    card->style[GUI_STATE_DEFAULT].gap    = 12.0f;
    _set_size(card, 380.0f, 0.0f); // fixed width, auto height
    scene__add_child(window, card);

    //
    // Heading -- use Roboto-Medium at 20 px. font_family / font_size on
    // a child override the inherited defaults from window.
    //
    gui_node* heading = scene__node_new(GUI_NODE_TEXT);
    _set_text(heading, "Pure-code demo");
    _set_color(heading, scene__hex(0xf4f5fa));
    _set_font_family(heading, "Roboto-Medium");
    heading->style[GUI_STATE_DEFAULT].font_size = 20.0f;
    _set_size(heading, 0.0f, 28.0f);
    scene__add_child(card, heading);

    gui_node* subtitle = scene__node_new(GUI_NODE_TEXT);
    _set_text(subtitle, "scene built in C with no .ui / .style / parser");
    _set_color(subtitle, text_dim);
    subtitle->style[GUI_STATE_DEFAULT].font_size = 12.0f;
    _set_size(subtitle, 0.0f, 18.0f);
    scene__add_child(card, subtitle);

    //
    // Accent bar.
    //
    gui_node* accent = scene__node_new(GUI_NODE_DIV);
    _set_bg(accent, bg_primary);
    accent->style[GUI_STATE_DEFAULT].radius = 2.0f;
    _set_size(accent, 0.0f, 3.0f);
    scene__add_child(card, accent);

    //
    // Scale slider. Drag drives scene__set_scale via on_code_scale_change.
    //
    gui_node* slider = scene__node_new(GUI_NODE_SLIDER);
    _set_bg(slider, track);
    _set_fg(slider, thumb);
    slider->style[GUI_STATE_DEFAULT].radius = 6.0f;
    _set_size(slider, 0.0f, 18.0f);
    slider->value_min = 0.5f;
    slider->value_max = 2.5f;
    slider->value     = 1.0f;
    scene__set_on_change(slider, "on_code_scale_change");
    scene__add_child(card, slider);

    //
    // Two buttons, side by side -- put them in a Row.
    //
    gui_node* btn_row = scene__node_new(GUI_NODE_ROW);
    btn_row->style[GUI_STATE_DEFAULT].gap = 8.0f;
    _set_size(btn_row, 0.0f, 36.0f);
    scene__add_child(card, btn_row);

    gui_node* btn_primary = scene__node_new(GUI_NODE_BUTTON);
    _set_text(btn_primary, "Save");
    _set_color(btn_primary, scene__hex(0xf4f5fa));
    _set_bg(btn_primary, bg_primary);
    btn_primary->style[GUI_STATE_DEFAULT].radius = 7.0f;
    //
    // Per-state bg overrides. scene__resolve_styles picks the slot
    // matching n->state each frame (HOVER while hovered, PRESSED
    // while the mouse is down on the node). Only has_bg = TRUE slots
    // contribute; un-set slots fall back to the default slot.
    //
    btn_primary->style[GUI_STATE_HOVER].background_color       = bg_primary_hover;
    btn_primary->style[GUI_STATE_HOVER].has_background_color   = TRUE;
    btn_primary->style[GUI_STATE_PRESSED].background_color     = bg_primary_pressed;
    btn_primary->style[GUI_STATE_PRESSED].has_background_color = TRUE;
    scene__set_on_click(btn_primary, "on_code_primary_click");
    scene__add_child(btn_row, btn_primary);

    gui_node* btn_ghost = scene__node_new(GUI_NODE_BUTTON);
    _set_text(btn_ghost, "Cancel");
    _set_color(btn_ghost, text_light);
    _set_bg(btn_ghost, bg_ghost);
    btn_ghost->style[GUI_STATE_DEFAULT].radius = 7.0f;
    btn_ghost->style[GUI_STATE_HOVER].background_color       = bg_ghost_hover;
    btn_ghost->style[GUI_STATE_HOVER].has_background_color   = TRUE;
    btn_ghost->style[GUI_STATE_PRESSED].background_color     = bg_ghost_pressed;
    btn_ghost->style[GUI_STATE_PRESSED].has_background_color = TRUE;
    scene__set_on_click(btn_ghost, "on_code_ghost_click");
    scene__add_child(btn_row, btn_ghost);

    //
    // Small info footer.
    //
    gui_node* footer = scene__node_new(GUI_NODE_TEXT);
    _set_text(footer, "drag the slider to rescale the whole UI");
    _set_color(footer, text_dim);
    footer->style[GUI_STATE_DEFAULT].font_size = 11.0f;
    _set_size(footer, 0.0f, 14.0f);
    scene__add_child(card, footer);

    return window;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    gui_app_config cfg;
    cfg.title       = L"gui poc (pure code)";
    cfg.width       = 640;
    cfg.height      = 480;
    cfg.clear_color = scene__hex(0x101010);

    if (!platform__init(&cfg))
    {
        log_error("platform__init failed");
        return 1;
    }

    gui_node* root = _build_scene();
    if (root == NULL)
    {
        log_error("failed to build scene");
        platform__shutdown();
        return 1;
    }
    scene__set_root(root);

    while (platform__tick()) { }

    platform__shutdown();
    return 0;
}
