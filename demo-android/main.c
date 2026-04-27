//
// demo-android/main.c - Android host app for the gui library.
//
// SHARES the declarative parts with the Windows demo: `main.ui` +
// `main.style` live in ../demo-windows/ and are staged into the
// APK's assets/ by build.ps1 / build.sh at build time. So the
// visual tree, selector styles, animations, and effects are
// identical between targets.
//
// What DOES differ is THIS file. Each platform's host exe/so
// provides its own set of UI_HANDLER-marked click / change
// callbacks, and the library auto-resolves them by name via the
// platform's symbol table (dlsym on Android, GetProcAddress on
// Windows). So handlers in main.ui like `on_click="on_save"` pick
// up whichever implementation this platform ships.
//
// ANDROID SPECIFICS:
//   - Virtual keyboard is enabled by default in scene.c (it checks
//     __ANDROID__), so typing into <input> / <textarea> auto-pops
//     the on-screen keyboard.
//   - Canvas drawing uses the same API; finger-drag events come
//     through the touch-state-machine in platform_android.c as
//     mouse_down + mouse_drag, identical to desktop.
//   - The "Toggle theme" button uses scene__set_background_color_override for
//     runtime recolor -- overrides survive hot reload + rule
//     re-resolve automatically.
//
// MIXING DECLARATIVE + CODE. The intended model is:
//   1. Start from .ui / .style for structure, layout, visuals.
//   2. Drop in <button on_click="..."> / <slider on_change="...">
//      attribute references for the dynamic bits.
//   3. Implement those handlers here in C. Handlers have full
//      access to the scene graph via scene__find_by_id,
//      scene__root, etc. so they can mutate nodes at runtime.
//
// Handlers below are the Android variant: they log what fired and
// apply the minimum mutation needed. Heavier host-side logic
// (canvas paint brush, color-preview swatch, etc.) are left as
// no-op logs on Android -- the demo ships for touch exploration
// of the widget set, not for authoring content.
//

#include <stdlib.h>
#include <string.h>

#include "gui_api.h"
#include "types.h"
#include "gui.h"
#include "scene.h"
#include "platform.h"
#include "hot_reload.h"
#include "canvas.h"
#include "third_party/log.h"

//
// Theme state. Flipped by on_theme_toggle; applied via
// scene__set_background_color_override so the per-frame style resolve + rule
// overlay can't stomp it.
//
static boole g_dark_theme = TRUE;

//
// Canvas paint state. Same two-sentinel pattern the Windows demo
// uses -- reset on press, connect prev->current on drag.
//
static int g_paint_prev_x = -1;
static int g_paint_prev_y = -1;

//
// ===== handlers referenced from ../demo-windows/main.ui ====================
//
// All handlers are light: log the event, do the minimum mutation.
// The intent is to prove the declarative parser + event pipeline
// flows end-to-end on Android without making this file own much
// app logic.
//

UI_HANDLER void on_primary_click(gui_event* ev)
{
    (void)ev;
    log_info("[android] primary click");
}

UI_HANDLER void on_primary_click1(gui_event* ev)
{
    (void)ev;
    log_info("[android] primary click (second button)");
}

UI_HANDLER void on_secondary_click(gui_event* ev)
{
    (void)ev;
    log_info("[android] secondary click");
}

UI_HANDLER void on_scale_change(gui_event* ev)
{
    //
    // Slider drag -> global UI scale. Same as on Windows; this is
    // where the fractional-font-cache integer-snap in font__at
    // earns its keep, since Android has no scroll wheel to jump
    // around the range in coarse steps.
    //
    scene__set_scale(ev->change.scalar);
}

UI_HANDLER void on_name_change(gui_event* ev)
{
    char* txt = (ev->sender != NULL) ? ev->sender->text : "";
    log_info("[android] name: \"%s\"", txt);
}

UI_HANDLER void on_toggle_display(gui_event* ev)
{
    //
    // display:none removes the panel from layout entirely --
    // siblings below collapse into the gap. The resolver preserves
    // display + visibility across the per-frame style wipe, so this
    // mutation sticks (unlike bg which needs scene__set_background_color_override).
    //
    gui_node* target = scene__find_by_id("collapsible-panel");
    if (target == NULL) { return; }
    boole shown = (ev->change.scalar > 0.5f);
    target->style[GUI_STATE_DEFAULT].display = shown ? GUI_DISPLAY_BLOCK : GUI_DISPLAY_NONE;
}

UI_HANDLER void on_toggle_visibility(gui_event* ev)
{
    gui_node* target = scene__find_by_id("dimming-panel");
    if (target == NULL) { return; }
    boole shown = (ev->change.scalar > 0.5f);
    target->style[GUI_STATE_DEFAULT].visibility = shown ? GUI_VISIBILITY_VISIBLE : GUI_VISIBILITY_HIDDEN;
}

UI_HANDLER void on_theme_toggle(gui_event* ev)
{
    (void)ev;
    g_dark_theme = !g_dark_theme;

    gui_node* root = scene__root();
    if (root != NULL)
    {
        scene__set_background_color_override(root, g_dark_theme ? scene__hex(0x0a0a0d) : scene__hex(0xf5f5f7));
        scene__set_font_color_override(root, g_dark_theme ? scene__hex(0xe5e7eb) : scene__hex(0x14151b));
    }
    gui_node* panel = scene__find_by_id("fancy-div");
    if (panel != NULL)
    {
        scene__set_background_color_override(panel, g_dark_theme ? scene__hex(0x14151b) : scene__hex(0xffffff));
    }
    log_info("[android] theme: %s", g_dark_theme ? "dark" : "light");
}

UI_HANDLER void on_color_change(gui_event* ev)
{
    gui_color c = ev->color.value;
    gui_node* preview = scene__find_by_id("color-preview");
    if (preview != NULL)
    {
        scene__set_background_color_override(preview, c);
    }
}

UI_HANDLER void on_popup_ok_click(gui_event* ev)
{
    (void)ev;
    scene__popup_ok("Settings saved.", "on_popup_dismissed");
}

UI_HANDLER void on_popup_confirm_click(gui_event* ev)
{
    (void)ev;
    scene__popup_confirm("Discard changes?", "on_popup_dismissed");
}

UI_HANDLER void on_popup_dismissed(gui_event* ev)
{
    log_info("[android] popup dismissed: %s", ev->popup.confirmed ? "yes/ok" : "no/cancel");
}

UI_HANDLER void on_toggle_vkb(gui_event* ev)
{
    //
    // Toggling is mainly a desktop thing -- on Android the keyboard
    // auto-shows when a text widget takes focus. Still useful for
    // letting users verify the behavior without touching an input.
    //
    (void)ev;
    boole now = !scene__virtual_keyboard_enabled();
    scene__set_virtual_keyboard_enabled(now);
    if (!now) { scene__hide_keyboard(); }
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

UI_HANDLER void on_canvas_paint(gui_event* ev)
{
    gui_node* cv = ev->sender;
    if (cv == NULL) { return; }
    int px = 0, py = 0;
    canvas__screen_to_pixel(cv, ev->mouse.x, ev->mouse.y, &px, &py);
    //
    // Constant indigo brush. Android's touch has less expressive
    // state than a mouse (no hover, single pointer) so we keep the
    // paint story simple.
    //
    gui_color brush = scene__hex(0x818cf8);
    brush.a = 1.0f;
    if (ev->mouse.button == 0)
    {
        g_paint_prev_x = px;
        g_paint_prev_y = py;
        canvas__fill_circle(cv, px, py, 3, brush);
        return;
    }
    if (g_paint_prev_x >= 0)
    {
        canvas__stroke_line(cv, g_paint_prev_x, g_paint_prev_y, px, py, brush);
        canvas__fill_circle(cv, px, py, 3, brush);
    }
    g_paint_prev_x = px;
    g_paint_prev_y = py;
}

//
// ===== entry ===============================================================
//

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    gui_app_config cfg;
    cfg.title       = L"gui poc (android)";
    cfg.width       = 800;
    cfg.height      = 600;
    cfg.clear_color = scene__hex(0x101010);

    if (!platform__init(&cfg))
    {
        log_error("platform__init failed");
        return 1;
    }

    //
    // On Android scene auto-enables the virtual keyboard (default-
    // TRUE on __ANDROID__). We could force it off here for kiosk
    // setups or testing, but leaving it on is the expected mobile
    // behavior.
    //
    // DEMO_SOURCE_DIR resolves to "" on Android via the generated
    // android_build_defines.h, so the path below ends up as
    // "/main.style" / "/main.ui". fs_android.c strips the leading
    // slash before handing off to AAssetManager, and the sideload
    // dir (/storage/emulated/0/Android/data/<pkg>/files/) is
    // checked first so `adb push main.ui <sideload>/main.ui` is the
    // hot-reload workflow.
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
