//
// demo-linux-drm/main.c - Linux DRM/KMS host app for the gui library.
//
// SHARES the declarative parts (main.ui + main.style) with the
// Windows and Android demos -- they live in ../demo-windows/ and
// are copied into the build output by build.sh. So the visual
// tree, selector styles, animations, and effects are identical
// across targets.
//
// This file provides the host-side UI_HANDLER-marked callbacks
// that main.ui references. The library auto-resolves them via
// dlsym(RTLD_DEFAULT, name) (installed by platform_linux_drm.c).
// UI_HANDLER expands to __attribute__((visibility("default"),
// used)) so the symbols survive -fvisibility=hidden and
// --gc-sections.
//
// LINUX DRM SPECIFICS:
//   - No windowing system: this takes over /dev/dri/card0 as
//     DRM master, which requires no other DRM master (no X11 /
//     Wayland / GDM) on the tty. Run from a text console (Ctrl+
//     Alt+F3, log in, ./run.sh) or over SSH.
//   - Permissions: the user running this needs read access to
//     /dev/dri/card* (usually `video` group) and /dev/input/
//     event* (usually `input` group). Alternatively run as root.
//   - Input: raw evdev on every /dev/input/event*. A USB keyboard
//     + USB mouse + resistive/capacitive touchscreen all work out
//     of the box. Gamepads are ignored.
//   - The "Toggle theme" button uses scene__set_background_color_override for
//     runtime recolor -- overrides survive hot reload + rule
//     re-resolve automatically (same pattern as the other demos).
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
// overlay can't stomp it. Exactly the same pattern the Android
// demo uses.
//
static boole g_dark_theme = TRUE;

//
// Canvas paint state. Two-sentinel "previous stroke endpoint"
// pattern -- reset on press, connect prev->current on drag.
//
static int g_paint_prev_x = -1;
static int g_paint_prev_y = -1;

//
// ===== handlers referenced from ../demo-windows/main.ui ====================
//

UI_HANDLER void on_primary_click(gui_event* ev)
{
    (void)ev;
    log_info("[linux-drm] primary click");
}

UI_HANDLER void on_primary_click1(gui_event* ev)
{
    (void)ev;
    log_info("[linux-drm] primary click (second button)");
}

UI_HANDLER void on_secondary_click(gui_event* ev)
{
    (void)ev;
    log_info("[linux-drm] secondary click");
}

UI_HANDLER void on_scale_change(gui_event* ev)
{
    scene__set_scale(ev->change.scalar);
}

UI_HANDLER void on_name_change(gui_event* ev)
{
    char* txt = (ev->sender != NULL) ? ev->sender->text : "";
    log_info("[linux-drm] name: \"%s\"", txt);
}

UI_HANDLER void on_toggle_display(gui_event* ev)
{
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
    log_info("[linux-drm] theme: %s", g_dark_theme ? "dark" : "light");
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
    log_info("[linux-drm] popup dismissed: %s", ev->popup.confirmed ? "yes/ok" : "no/cancel");
}

UI_HANDLER void on_toggle_vkb(gui_event* ev)
{
    //
    // On a keyboardless kiosk (touchscreen only) this lets the
    // user force the on-screen keyboard on. Default is whatever
    // scene.c decides based on platform -- FALSE on non-Android
    // targets, so flipping it here enables the vkb when the demo
    // runs on a touchscreen panel.
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
    cfg.title       = L"gui poc (linux drm)";     // unused on DRM (no window title)
    cfg.width       = 0;                          // 0 = follow the display's native mode
    cfg.height      = 0;
    cfg.clear_color = scene__hex(0x101010);

    if (!platform__init(&cfg))
    {
        log_error("platform__init failed");
        return 1;
    }

    //
    // DEMO_SOURCE_DIR is baked at compile time by build.sh. On
    // DRM it points at the staged asset dir next to the binary
    // (same layout as demo-android uses for its APK assets). The
    // fs_linux.c POSIX open() resolves the absolute path
    // directly.
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
