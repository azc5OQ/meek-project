//
//demo/main.c - host app for the gui library.
//
//this file is the application, not the library. it lives in its own
//top-level directory (sibling to ../gui), next to main.ui + main.style,
//links to gui.dll, and exposes its click handlers via UI_HANDLER so
//the library can find them by name.
//
//flow:
//  1. configure the platform (window size, title, clear color)
//  2. platform__init() opens the window, gets a gl context,
//     boots the renderer, and installs a host-symbol resolver
//     (Win32 GetProcAddress on this exe).
//  3. hot_reload__watch_style() loads + watches main.style.
//  4. hot_reload__watch_ui()    loads + watches main.ui.
//  5. drive the frame loop until the window closes.
//
//handlers below: marked UI_HANDLER (= __declspec(dllexport)) so
//GetProcAddress finds them. nothing else needed -- no SCENE_HANDLER
//calls, no name strings duplicated. the .ui file's on_click="foo"
//resolves to the C function "foo" automatically.
//

#include <stdlib.h>
#include <string.h>

#include "gui_api.h"
#include "types.h"
#include "gui.h"
#include "scene.h"          // for scene__hex used in cfg.clear_color
#include "platform.h"       // unified platform__init / platform__tick / platform__shutdown + `main` -> app_main rename.
#include "hot_reload.h"
#include "canvas.h"         // canvas__clear / stroke_line / screen_to_pixel for the paint demo.
#include "third_party/log.h"

//
// Simple theme model: two colour swatches the toggle button flips
// between. Applied to a handful of nodes by id when the toggle fires.
// Keeps the demo host-side change minimal -- no CSS re-parse required.
//
static boole g_dark_theme = TRUE;

//
// Previous canvas pointer state so on_mouse_drag can connect line
// segments instead of dropping isolated pixels. Reset on mouse-down
// via the -1 sentinels so a new stroke doesn't inherit the tail of
// the previous one.
//
static int g_paint_prev_x = -1;
static int g_paint_prev_y = -1;

UI_HANDLER void on_primary_click(gui_event* ev)
{
    (void)ev;
    log_info("primary button clicked");
}

UI_HANDLER void on_primary_click1(gui_event* ev)
{
    (void)ev;
    log_info("primary button #1 clicked");
}

UI_HANDLER void on_secondary_click(gui_event* ev)
{
    (void)ev;
    log_info("secondary button clicked");
}

UI_HANDLER void on_scale_change(gui_event* ev)
{
    //
    //the slider's on_change passes the new value as ev->change.scalar.
    //feeding it straight into scene__set_scale grows / shrinks every
    //subsequent layout pass.
    //
    scene__set_scale(ev->change.scalar);
    log_info("scale = %.3f", (double)ev->change.scalar);
}

UI_HANDLER void on_name_change(gui_event* ev)
{
    //
    //Input widgets fire on_change after every text mutation with the
    //new length in ev->change.scalar. ev->sender points at the mutated
    //node, so reading its text buffer gives the current string.
    //
    char* txt = (ev->sender != NULL) ? ev->sender->text : "";
    log_info("name input: \"%s\" (len = %d)", txt, (int)ev->change.scalar);
}

//
// Show/hide handlers for the checkbox-toggles-panel demo. Both write
// into node->style[GUI_STATE_DEFAULT] directly (the resolver copies
// into resolved each frame, the animator picks up the transition
// next visit, :appear replays).
//
// ev->change.scalar is 0.0 (unchecked) or 1.0 (checked) for
// checkboxes. We use the conventional "checked = shown" polarity so
// the checkbox label can read naturally ("Show the panel").
//

UI_HANDLER void on_toggle_display(gui_event* ev)
{
    gui_node* target = scene__find_by_id("collapsible-panel");
    if (target == NULL)
    {
        log_warn("on_toggle_display: no node with id=collapsible-panel");
        return;
    }
    boole shown = (ev->change.scalar > 0.5f);
    //
    // display: none removes the node + its subtree from layout AND
    // rendering. Siblings below will collapse the gap on the next
    // layout pass. Flipping back to block retriggers :appear on the
    // panel's subtree because the animator detects the transition.
    //
    target->style[GUI_STATE_DEFAULT].display = shown ? GUI_DISPLAY_BLOCK : GUI_DISPLAY_NONE;
    log_info("collapsible-panel display = %s", shown ? "block" : "none");
}

UI_HANDLER void on_toggle_visibility(gui_event* ev)
{
    gui_node* target = scene__find_by_id("dimming-panel");
    if (target == NULL)
    {
        log_warn("on_toggle_visibility: no node with id=dimming-panel");
        return;
    }
    boole shown = (ev->change.scalar > 0.5f);
    //
    // visibility: hidden keeps the layout reservation -- siblings
    // don't move -- but emit_draws skips the node. Useful for things
    // that should fade out but not reflow their neighbours.
    //
    target->style[GUI_STATE_DEFAULT].visibility = shown ? GUI_VISIBILITY_VISIBLE : GUI_VISIBILITY_HIDDEN;
    log_info("dimming-panel visibility = %s", shown ? "visible" : "hidden");
}

//
// Override a node's bg via the scene's host-override channel so
// the resolver (which re-applies rules from main.style every
// frame) doesn't stomp the change.
//
static void _demo__override_bg_by_id(char* id, gui_color c)
{
    gui_node* n = scene__find_by_id(id);
    if (n == NULL) { return; }
    scene__set_background_color_override(n, c);
}

UI_HANDLER void on_theme_toggle(gui_event* ev)
{
    (void)ev;
    g_dark_theme = !g_dark_theme;

    //
    // Root window + the outer panel get the page/card bg override.
    // Text color cascades from root down through the resolver's
    // inheritance pass, so setting it on root is enough for every
    // label / button text that doesn't have an explicit color rule.
    //
    gui_node* root = scene__root();
    if (root != NULL)
    {
        scene__set_background_color_override(root, g_dark_theme ? scene__hex(0x0a0a0d) : scene__hex(0xf5f5f7));
        scene__set_font_color_override(root, g_dark_theme ? scene__hex(0xe5e7eb) : scene__hex(0x14151b));
    }
    _demo__override_bg_by_id("fancy-div", g_dark_theme ? scene__hex(0x14151b) : scene__hex(0xffffff));

    log_info("theme: %s", g_dark_theme ? "dark" : "light");
}

UI_HANDLER void on_color_change(gui_event* ev)
{
    //
    // ev->color.value is the picker's current RGBA 0..1. Log as #RRGGBB
    // and mirror into the preview swatch's bg.
    //
    gui_color c = ev->color.value;
    int r = (int)(c.r * 255.0f + 0.5f);
    int g = (int)(c.g * 255.0f + 0.5f);
    int b = (int)(c.b * 255.0f + 0.5f);
    log_info("color: #%02x%02x%02x (a=%.2f)", r, g, b, (double)c.a);

    gui_node* preview = scene__find_by_id("color-preview");
    if (preview != NULL)
    {
        preview->style[GUI_STATE_DEFAULT].background_color     = c;
        preview->style[GUI_STATE_DEFAULT].has_background_color = TRUE;
    }
}

UI_HANDLER void on_popup_ok_click(gui_event* ev)
{
    (void)ev;
    scene__popup_ok("Settings saved successfully.", "on_popup_dismissed");
}

UI_HANDLER void on_popup_confirm_click(gui_event* ev)
{
    (void)ev;
    scene__popup_confirm("Discard all unsaved changes?", "on_popup_dismissed");
}

UI_HANDLER void on_popup_dismissed(gui_event* ev)
{
    //
    // Fires for both OK and confirm variants. ev->popup.confirmed is
    // TRUE for OK / Yes, FALSE for Cancel / No. OK-only popups always
    // pass TRUE (there's nothing else to pass).
    //
    log_info("popup dismissed: %s", ev->popup.confirmed ? "yes/ok" : "no/cancel");
}

UI_HANDLER void on_toggle_vkb(gui_event* ev)
{
    (void)ev;
    boole now = !scene__virtual_keyboard_enabled();
    scene__set_virtual_keyboard_enabled(now);
    //
    // If we just enabled it and a text widget already has focus,
    // show it straight away -- otherwise the user would have to
    // click out and back in. Same for the reverse direction.
    //
    gui_node* f = scene__focus();
    if (now && f != NULL && (f->type == GUI_NODE_INPUT || f->type == GUI_NODE_TEXTAREA))
    {
        scene__show_keyboard();
    }
    else if (!now)
    {
        scene__hide_keyboard();
    }
    log_info("virtual keyboard: %s", now ? "enabled" : "disabled");
}

UI_HANDLER void on_canvas_clear(gui_event* ev)
{
    (void)ev;
    gui_node* cv = scene__find_by_id("paint");
    if (cv == NULL) { return; }
    canvas__clear(cv, scene__hex(0x0a0a0d));
    g_paint_prev_x = -1;
    g_paint_prev_y = -1;
}

//
// Canvas paint handler. widget_canvas forwards mouse_down and
// mouse_drag to on_change; button=0 is "start stroke", button=1 is
// "continue stroke". We reset the previous-point sentinel on press
// so a new stroke doesn't connect from wherever the last stroke
// ended, and lay down a line from (prev) -> (now) on every drag.
//
UI_HANDLER void on_canvas_paint(gui_event* ev)
{
    gui_node* cv = ev->sender;
    if (cv == NULL) { return; }
    int px = 0, py = 0;
    canvas__screen_to_pixel(cv, ev->mouse.x, ev->mouse.y, &px, &py);
    //
    // Palette of hues. Cycles as the pointer moves so the line
    // varies colour without us tracking a brush picker yet -- keeps
    // the demo self-contained. Replace with a real palette once the
    // colorpicker is wired into a host-side selection state.
    //
    static int hue_step = 0;
    hue_step = (hue_step + 1) & 0x3f;
    gui_color brush = scene__hex(0x6366f1 + hue_step * 0x000400);
    brush.a = 1.0f;

    if (ev->mouse.button == 0)
    {
        //
        // Press -- start a new stroke. Record the point but don't
        // connect from anything prior.
        //
        g_paint_prev_x = px;
        g_paint_prev_y = py;
        canvas__fill_circle(cv, px, py, 2, brush);
        return;
    }
    //
    // Drag -- connect from the previous point to the current one.
    // If the sentinel is still -1 we got a drag without a press
    // (shouldn't happen in practice); fall back to a single pixel.
    //
    if (g_paint_prev_x >= 0)
    {
        canvas__stroke_line(cv, g_paint_prev_x, g_paint_prev_y, px, py, brush);
        //
        // Thicken the line a touch by dabbing a small disc at each
        // endpoint. Otherwise a single-pixel Bresenham line looks
        // wispy against the dark canvas bg.
        //
        canvas__fill_circle(cv, px, py, 2, brush);
    }
    g_paint_prev_x = px;
    g_paint_prev_y = py;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    gui_app_config cfg;
    cfg.title       = L"gui poc";
    cfg.width       = 800;
    cfg.height      = 600;
    cfg.clear_color = scene__hex(0x101010);

    if (!platform__init(&cfg))
    {
        log_error("platform__init failed");
        return 1;
    }

    //
    //load styles before tree so the first frame already has the right
    //visual properties resolved. order isn't strictly required (resolve
    //runs every tick) but it avoids a one-frame flash of unstyled
    //content if the demo ever shortens to a single render.
    //
    if (!hot_reload__watch_style(DEMO_SOURCE_DIR "/main.style"))
    {
        log_error("hot_reload__watch_style failed");
        platform__shutdown();
        return 1;
    }

    if (!hot_reload__watch_ui(DEMO_SOURCE_DIR "/main.ui"))
    {
        log_error("hot_reload__watch_ui failed");
        platform__shutdown();
        return 1;
    }

    while (platform__tick())
    {
        hot_reload__tick();
    }

    hot_reload__shutdown();
    platform__shutdown();
    return 0;
}
