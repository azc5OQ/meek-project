#ifndef PLATFORM_H
#define PLATFORM_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

//
// platform.h - uniform platform interface.
//

/**
 * Window + renderer init parameters. Shared across platforms;
 * previously lived in the now-deleted platform_win32.h /
 * platform_android.h. All fields are required.
 *
 * title:        Window title. wchar_t* for native Win32 Unicode;
 *               ignored on Android (no OS-level title bar there).
 * width/height: Initial client-area dimensions in pixels.
 * clear_color:  Back-buffer clear color, applied by the renderer at
 *               the start of every frame.
 */
typedef struct gui_app_config
{
    const wchar_t* title;
    int64          width;
    int64          height;
    gui_color      clear_color;
} gui_app_config;

//
// Every platform backend (Windows, Android, eventually Linux + macOS)
// implements the same four functions:
//
//     platform__init      stand up window + renderer + input.
//     platform__tick      one frame of event pump + render + present.
//     platform__shutdown  tear everything down.
//
// Host apps include this header and use the unified names; the right
// backend is linked in by the build system, so main.c reads exactly
// the same on every target platform.
//
// ENTRY POINT
// -----------
//
// Windows uses `int main(int argc, char** argv)`. Android uses
// `void android_main(struct android_app*)` called by
// android_native_app_glue. These can't be unified at the symbol
// level, so platform.h renames the host's entry to `app_main`:
//
//     #define main app_main
//
// The host writes `int main(int argc, char** argv)` as usual; the
// preprocessor rewrites it to `int app_main(...)`. The actual `int
// main()` (Windows) and `void android_main()` (Android) are
// provided by the platform backend and forward to `app_main`. On
// Android the backend also stashes the `struct android_app*` before
// dispatching, so `platform__init` can pick it up without the host
// having to touch the NDK glue struct.
//
// HOW TO USE
// ----------
//
//     #include "platform.h"
//
//     int main(int argc, char** argv)
//     {
//         gui_app_config cfg = { ... };
//         if (!platform__init(&cfg)) return 1;
//
//         // build scene...
//
//         while (platform__tick()) { /* optional per-frame work */ }
//
//         platform__shutdown();
//         return 0;
//     }
//
// Works unchanged on Windows and Android. The compiler / linker pick
// the right backend.
//

/**
 * Initialize the platform and renderer. Called once at startup.
 *
 * @function platform__init
 * @param {const gui_app_config*} cfg - Window title / size / clear color.
 * @return {boole} TRUE on success; FALSE if window creation or
 *   renderer init failed.
 */
GUI_API boole platform__init(const gui_app_config* cfg);

/**
 * Run one iteration of the platform's main loop: pump OS events,
 * capture the frame timestamp, resolve styles, tick the animator,
 * lay out the tree, emit draws, and present. Returns FALSE when
 * the user has asked to close the application (window close,
 * Android APP_CMD_DESTROY, etc.).
 *
 * @function platform__tick
 * @return {boole} TRUE to continue running; FALSE to exit.
 */
GUI_API boole platform__tick(void);

/**
 * Tear down in reverse of init. Safe to call if init bailed out.
 *
 * @function platform__shutdown
 * @return {void}
 */
GUI_API void platform__shutdown(void);

/**
 * Bring the platform window to the top of the z-order and pin it
 * there (Win32: SWP_TOPMOST + SetForegroundWindow). Called from the
 * visual-regression test runner before capturing so nothing
 * obscures the window's client area during BitBlt. No-op on
 * platforms other than Windows.
 *
 * @function platform__set_topmost
 * @return {void}
 */
GUI_API void platform__set_topmost(void);

/**
 * Capture the current window client area to a 32-bit BGRA .bmp file.
 * Returns TRUE on success, FALSE on any IO or API failure. Intended
 * for the visual-regression test runner under `tests/visual/`; not
 * used by normal host apps.
 *
 * Path is a filesystem path in the native encoding. Overwrites any
 * existing file. Format choice: BMP over PNG because BMP needs no
 * compression dependency (stb_image_write + zlib) and every browser
 * / image viewer displays it natively. Resulting files are larger
 * than PNG but test captures are small (typical 800x600 ~= 1.8 MB).
 *
 * Must be called AFTER a frame has been rendered + presented (the
 * captured pixels come from the window's composited client area);
 * typically the test runner ticks 5+ frames to let animations
 * settle, then calls this.
 *
 * Only the Win32 backend implements this today; other backends
 * return FALSE.
 *
 * @function platform__capture_bmp
 * @param {const char*} path - output filesystem path.
 * @return {boole} TRUE on success.
 */
GUI_API boole platform__capture_bmp(const char* path);

//
// Rename the host's `main` to `app_main` so the platform backend's
// own `main` / `android_main` entry points can forward to it. The
// backend's entry-point .c file deliberately does NOT include this
// header, so the real `int main()` it defines isn't macro-replaced.
//
// Guarded by _PLATFORM_INTERNAL so the backend can include this
// header (to pick up the API decls) without tripping the rename.
//
#ifndef _PLATFORM_INTERNAL
#define main app_main
#endif

#endif
