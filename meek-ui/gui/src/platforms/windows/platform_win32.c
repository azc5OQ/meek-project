//
//platform_win32.c - window creation + message pump + input dispatch.
//
//renderer-agnostic. creates an HWND, pumps WM_* messages through the
//window procedure, translates mouse events into scene__on_* calls, then
//hands the HWND to whichever renderer backend was compiled in.
//
//everything graphics-API-specific (OpenGL context, D3D device+swapchain,
//Present/SwapBuffers) lives in the renderer backend -- see
//renderers/opengl3_renderer.c for the working reference, and d3d11/d3d9
//stubs for what a different backend would do. this file knows zero about
//the GPU.
//

#include <windows.h>
#include <stdio.h>

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "renderer.h"
#include "widget_registry.h"
#include "widgets/widget_image_cache.h"
#include "font.h"
#include "animator.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"

//
// DPI-awareness APIs. Declared inline instead of pulling in
// <shellscalingapi.h> because the SDK headers gate those functions
// behind Windows-version macros that aren't always set in the
// project's WIN32_LEAN_AND_MEAN build, and we runtime-resolve
// anyway so we compile + run on older Windows (falls through to
// system-DPI awareness on Windows 7, per-monitor V1 on Windows 8.1,
// per-monitor V2 on Windows 10 1703+).
//
typedef HANDLE DPI_AWARENESS_CONTEXT_compat;
#define _PLATFORM_WIN32_INTERNAL__DPI_V2_CTX ((DPI_AWARENESS_CONTEXT_compat)(-4))
//
// V1 per-monitor awareness is an enum value for SetProcessDpiAwareness
// (shcore.dll, Windows 8.1+):
//   PROCESS_DPI_UNAWARE           = 0
//   PROCESS_SYSTEM_DPI_AWARE      = 1
//   PROCESS_PER_MONITOR_DPI_AWARE = 2
//
#define _PLATFORM_WIN32_INTERNAL__PROCESS_PER_MONITOR_DPI_AWARE 2

typedef BOOL  (WINAPI* fncp_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT_compat);
typedef HRESULT (WINAPI* fncp_SetProcessDpiAwareness)(int);
typedef BOOL  (WINAPI* fncp_SetProcessDPIAware)(void);
typedef UINT  (WINAPI* fncp_GetDpiForWindow)(HWND);
typedef UINT  (WINAPI* fncp_GetDpiForSystem)(void);

//
// Try the modern per-monitor-V2 path first; fall back to the older
// ones in order. V2 is the only context that handles monitor-DPI
// changes and dialog scaling cleanly, but it's Windows 10 1703+; on
// anything older we settle for system-DPI awareness which is at
// least better than "Windows virtualises coordinates and hands us
// blurry pixels".
//
static void _platform_win32_internal__enable_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != NULL)
    {
        fncp_SetProcessDpiAwarenessContext p_ctx = (fncp_SetProcessDpiAwarenessContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (p_ctx != NULL && p_ctx(_PLATFORM_WIN32_INTERNAL__DPI_V2_CTX))
        {
            return;
        }
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != NULL)
    {
        fncp_SetProcessDpiAwareness p_dpi = (fncp_SetProcessDpiAwareness)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (p_dpi != NULL && SUCCEEDED(p_dpi(_PLATFORM_WIN32_INTERNAL__PROCESS_PER_MONITOR_DPI_AWARE)))
        {
            //
            // Leave shcore.dll handle alive; the call already took
            // effect process-wide.
            //
            return;
        }
    }
    if (user32 != NULL)
    {
        fncp_SetProcessDPIAware p_sys = (fncp_SetProcessDPIAware)GetProcAddress(user32, "SetProcessDPIAware");
        if (p_sys != NULL) { p_sys(); }
    }
}

//
// Read the DPI associated with a window (or system default). Per-
// monitor-V2 returns the monitor the window is currently on; older
// modes may return a cached value. 96 is the "100% scaling" default
// on Windows -- treat that as the 1.0 scale anchor.
//
static float _platform_win32_internal__scale_for_window(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    UINT dpi = 96;
    if (user32 != NULL && hwnd != NULL)
    {
        fncp_GetDpiForWindow p_win = (fncp_GetDpiForWindow)GetProcAddress(user32, "GetDpiForWindow");
        if (p_win != NULL) { dpi = p_win(hwnd); }
        else
        {
            fncp_GetDpiForSystem p_sys = (fncp_GetDpiForSystem)GetProcAddress(user32, "GetDpiForSystem");
            if (p_sys != NULL) { dpi = p_sys(); }
        }
    }
    //
    // GetDpiForWindow can return 0 during some transitions; clamp
    // to 96 in that case so we at least don't scale by 0.
    //
    if (dpi < 72) { dpi = 96; }
    return (float)dpi / 96.0f;
}

//
//Pull in the unified platform.h declarations so platform__init /
//tick / shutdown wrappers below compile. _PLATFORM_INTERNAL
//suppresses the `#define main app_main` rename inside that header,
//since this file deliberately defines the REAL `int main(...)`
//entry that forwards to the host's (already-renamed) app_main.
//
#define _PLATFORM_INTERNAL
#include "platform.h"
#undef _PLATFORM_INTERNAL

/**
 *platform-wide state. kept as file-local globals because this is the
 *single process-wide window the poc manages.
 */
typedef struct _platform_win32_internal__state
{
    HINSTANCE hinst;        // owning module instance.
    HWND      hwnd;         // window handle. the renderer borrows this via renderer__init.
    int64     viewport_w;   // current client-area width.
    int64     viewport_h;   // current client-area height.
    int64     mouse_x;      // last cursor x in client coords.
    int64     mouse_y;      // last cursor y in client coords.
    boole     should_close; // set TRUE when wm_close is received.
    gui_color clear_color;  // back-buffer clear colour each frame.
} _platform_win32_internal__state;

static _platform_win32_internal__state _platform_win32_internal__g;

static const wchar_t* _platform_win32_internal__class_name = L"GuiDemoWindowClass";

//
//forward declarations of statics.
//
static LRESULT CALLBACK _platform_win32_internal__wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static boole            _platform_win32_internal__register_class(HINSTANCE hinst);
static void             _platform_win32_internal__log_win32_error(char* where);
static gui_handler_fn   _platform_win32_internal__resolve_host_symbol(char* name);

//
//public api: initialize window and hand it to the renderer.
//
static boole platform_win32__init(const gui_app_config* cfg)
{
    //
    //bring the allocation tracker up before anything else so every
    //subsequent MEM_* call lands in the live-list. No-op unless the
    //tracker wrapper is compiled in (GUI_TRACK_ALLOCATIONS).
    //
    memory_manager__init();

    //
    //Declare this process DPI-aware BEFORE creating any window.
    //Order matters: if awareness is flipped on after a window
    //exists, Windows may have already baked in a virtualised DPI
    //for that window and moving to a per-monitor mode can produce
    //sizes that don't agree with what GetDpiForWindow reports.
    //With awareness enabled, every coordinate we get from the OS
    //(WM_SIZE, WM_MOUSEMOVE, etc.) is in real pixels, and our
    //scene__set_scale multiplier compensates for high-DPI by
    //sizing widgets/fonts up. Matches what Android's density-
    //bucket auto-scale does.
    //
    _platform_win32_internal__enable_dpi_awareness();

    //
    //register every built-in widget into the registry first so that
    //parser_xml + scene see them all before any tree is loaded. host
    //apps that ship custom widgets call widget_registry__register
    //after this returns.
    //
    widget_registry__bootstrap_builtins();

    _platform_win32_internal__g.hinst        = GetModuleHandleW(NULL);
    _platform_win32_internal__g.viewport_w   = cfg->width;
    _platform_win32_internal__g.viewport_h   = cfg->height;
    _platform_win32_internal__g.should_close = FALSE;
    _platform_win32_internal__g.clear_color  = cfg->clear_color;

    if (!_platform_win32_internal__register_class(_platform_win32_internal__g.hinst))
    {
        _platform_win32_internal__log_win32_error("RegisterClassExW");
        return FALSE;
    }

    //
    //adjust client-area size so the window ends up at the requested size.
    //CreateWindowExW's width/height include the frame + title bar; we
    //want the client area to match cfg->width/height.
    //
    RECT r;
    r.left   = 0;
    r.top    = 0;
    r.right  = (LONG)cfg->width;
    r.bottom = (LONG)cfg->height;
    DWORD style    = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    DWORD style_ex = 0;
    AdjustWindowRectEx(&r, style, FALSE, style_ex);

    _platform_win32_internal__g.hwnd = CreateWindowExW(
        style_ex,
        _platform_win32_internal__class_name,
        cfg->title,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left,
        r.bottom - r.top,
        NULL, NULL,
        _platform_win32_internal__g.hinst,
        NULL
    );
    if (_platform_win32_internal__g.hwnd == NULL)
    {
        _platform_win32_internal__log_win32_error("CreateWindowExW");
        UnregisterClassW(_platform_win32_internal__class_name, _platform_win32_internal__g.hinst);
        return FALSE;
    }

    //
    //Now that we have a window on a specific monitor, read its DPI
    //and set the global UI scale. The cfg->width / height were
    //interpreted as LOGICAL pixels (100%-scaling reference); with
    //DPI awareness on, Windows created the window at those pixel
    //counts but at e.g. 200% DPI they'll occupy only half the
    //physical space a 100% user would expect. Resize the client
    //area to the DPI-scaled equivalent so a `width: 800` request
    //produces an 800-logical-pixel visual regardless of monitor.
    //
    float ui_scale = _platform_win32_internal__scale_for_window(_platform_win32_internal__g.hwnd);
    scene__set_scale(ui_scale);

    if (ui_scale > 1.01f || ui_scale < 0.99f)
    {
        //
        //Bump the window size to match. AdjustWindowRectExForDpi
        //would be the ideal call here but it's Windows 10+ only;
        //the plain AdjustWindowRectEx already gives non-client
        //areas in physical pixels under DPI awareness, so using
        //it is fine.
        //
        RECT target;
        target.left   = 0;
        target.top    = 0;
        target.right  = (LONG)((float)cfg->width  * ui_scale);
        target.bottom = (LONG)((float)cfg->height * ui_scale);
        AdjustWindowRectEx(&target, style, FALSE, style_ex);
        SetWindowPos(_platform_win32_internal__g.hwnd, NULL, 0, 0, target.right - target.left, target.bottom - target.top, SWP_NOMOVE | SWP_NOZORDER);
        log_info("platform_win32: DPI %.0f%%  (scale %.2fx)", (double)(ui_scale * 100.0f), (double)ui_scale);
    }

    //
    //hand the window over to whichever renderer backend was linked in.
    //the backend creates its own graphics context against this HWND:
    //OpenGL grabs an HDC + makes a GL context; D3D backends create a
    //swapchain; etc. we stay ignorant of all that.
    //
    if (!renderer__init((void*)_platform_win32_internal__g.hwnd))
    {
        log_error("renderer__init failed");
        //
        // renderer__init is documented as safe-on-partial-state, so
        // call shutdown to release whatever it managed to allocate
        // (HDC, partial GL context, vertex buffers) before we drop
        // the window. Without this, every failed launch leaks all
        // those resources for the lifetime of the process.
        //
        renderer__shutdown();
        DestroyWindow(_platform_win32_internal__g.hwnd);
        UnregisterClassW(_platform_win32_internal__class_name, _platform_win32_internal__g.hinst);
        _platform_win32_internal__g.hwnd = NULL;
        return FALSE;
    }

    //
    //font subsystem. runs after renderer__init because atlas upload
    //(lazily, on first font__at) needs the graphics context live.
    //font__init scans gui/src/fonts/ and auto-registers every .ttf
    //found there -- host apps don't call any font functions; they just
    //reference families by name in their .style files.
    //
    if (!font__init())
    {
        log_error("font__init failed");
        //
        // Tear down in reverse of the work done above. font__shutdown
        // is a no-op if font__init bailed early enough that nothing
        // was registered.
        //
        font__shutdown();
        renderer__shutdown();
        DestroyWindow(_platform_win32_internal__g.hwnd);
        UnregisterClassW(_platform_win32_internal__class_name, _platform_win32_internal__g.hinst);
        _platform_win32_internal__g.hwnd = NULL;
        return FALSE;
    }

    //
    //install the host-symbol resolver. scene's dispatch falls back to
    //this when the registration table doesn't know a handler hash; we
    //look up the name in the host exe's export table so any function
    //marked UI_HANDLER in the host app is reachable from .ui files.
    //
    scene__set_symbol_resolver(_platform_win32_internal__resolve_host_symbol);

    return TRUE;
}

//
//public api: pump one frame of os messages, then render.
//returns FALSE once the window has been asked to close.
//
static boole platform_win32__tick(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            _platform_win32_internal__g.should_close = TRUE;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (_platform_win32_internal__g.should_close)
    {
        return FALSE;
    }

    //
    //capture this frame's start time in milliseconds and hand it to
    //scene. anything time-sensitive in the rest of this tick (the
    //animator, future hot-reload poll) reads scene__frame_time_ms /
    //scene__frame_delta_ms instead of taking its own clock sample,
    //so all subsystems see one consistent timestamp per frame.
    //
    //QueryPerformanceCounter is the precise Win32 monotonic clock.
    //we convert ticks to ms via the frequency, sampled once per
    //tick (cheap; cached by the kernel).
    //
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        int64 now_ms = (int64)((now.QuadPart * 1000) / freq.QuadPart);
        scene__begin_frame_time(now_ms);
    }

    //
    //resolve styles, run animator, layout, draw. ordering matters:
    //  1. resolve writes resolved.* from style[state].
    //  2. animator mutates resolved.* in place (alpha fades, etc.).
    //  3. layout reads resolved.* (pad, gap, size).
    //  4. emit_draws reads resolved.* (bg, color).
    //
    scene__resolve_styles();
    animator__tick();
    scene__layout(_platform_win32_internal__g.viewport_w, _platform_win32_internal__g.viewport_h);

    renderer__begin_frame(_platform_win32_internal__g.viewport_w, _platform_win32_internal__g.viewport_h, _platform_win32_internal__g.clear_color);

    //
    //type-aware tree walk lives in scene -- it knows that sliders emit
    //two rects (track + thumb), regular nodes emit one. the platform
    //layer just hands the renderer to it via the previously configured
    //begin_frame.
    //
    scene__emit_draws();

    //
    //end_frame finalizes the draw AND presents the back buffer (GL:
    //SwapBuffers, D3D: swapchain->Present). no platform-side present
    //call needed -- the renderer backend owns that.
    //
    renderer__end_frame();
    return TRUE;
}

//
//public api: tear down in reverse order of platform_win32__init.
//
static void platform_win32__shutdown(void)
{
    //
    // Image cache first: every cached entry holds a renderer-owned
    // texture, so we release them while the renderer is still alive.
    // Without this, the textures + path strings show up as leaks in
    // memory_manager's exit-time report.
    //
    widget_image__cache_shutdown();

    //
    //font subsystem next: it releases atlas textures through the
    //renderer's destroy_atlas entry point, so it has to run while the
    //renderer is still alive.
    //
    font__shutdown();

    //
    //shutdown the renderer next -- it releases its HDC (for GL
    //backends), its context, its GPU resources. must happen while the
    //window still exists so GDI/DX can release the HDC cleanly.
    //
    renderer__shutdown();

    if (_platform_win32_internal__g.hwnd != NULL)
    {
        DestroyWindow(_platform_win32_internal__g.hwnd);
        _platform_win32_internal__g.hwnd = NULL;
    }

    UnregisterClassW(_platform_win32_internal__class_name, _platform_win32_internal__g.hinst);

    //
    //tracker shutdown logs any leaks and walks the live list. Runs
    //after every subsystem has freed its owned blocks; anything still
    //outstanding at this point is a real leak.
    //
    memory_manager__shutdown();
}

//============================================================================
//UNIFIED platform__* API (see gui/src/platform.h)
//============================================================================
//
//Thin wrappers so host code can call platform__init / platform__tick /
//platform__shutdown regardless of target. The internal platform_win32__*
//names are kept for backward compatibility.
//

boole platform__init(const gui_app_config* cfg)     { return platform_win32__init(cfg); }
boole platform__tick(void)                          { return platform_win32__tick();    }
void  platform__shutdown(void)                      {        platform_win32__shutdown(); }

//
// Bring the window to the top of the z-order and pin it there.
// Used by the visual-regression test runner so capture-via-BitBlt
// doesn't pick up whatever happens to be in front (debugger,
// console, another running test). SWP_TOPMOST sets the topmost
// flag so subsequent windows don't cover it; SetForegroundWindow
// actually brings it forward.
//
void platform__set_topmost(void)
{
    HWND hwnd = _platform_win32_internal__g.hwnd;
    if (hwnd == NULL) { return; }
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(hwnd);
}

//
// Capture the window client area to a 32-bit BGRA .bmp. Win32 path:
//   1. GetClientRect(hwnd) to get the size we need.
//   2. GetDC(hwnd) + CreateCompatibleDC + CreateCompatibleBitmap to
//      make an offscreen DIB-compatible bitmap.
//   3. BitBlt from the window DC to the offscreen DC.
//   4. GetDIBits into a host buffer with a BITMAPINFO asking for
//      32 bpp top-down (negative height) + BI_RGB so the decoded
//      layout is plain BGRA with no row padding beyond DWORD.
//   5. Write the BMP file: BITMAPFILEHEADER + BITMAPINFOHEADER +
//      pixels. Hand-rolled; avoids pulling in a PNG encoder
//      dependency just for test captures.
//
// Used by the visual-regression runner at tests/visual/test_runner.c.
// Intentionally tolerant -- failures log and return FALSE so the
// test driver can record the miss and continue.
//
boole platform__capture_bmp(const char* path)
{
    if (path == NULL || path[0] == 0)
    {
        log_error("platform__capture_bmp: NULL path");
        return FALSE;
    }
    HWND hwnd = _platform_win32_internal__g.hwnd;
    if (hwnd == NULL)
    {
        log_error("platform__capture_bmp: no window");
        return FALSE;
    }
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) { log_error("GetClientRect failed"); return FALSE; }
    LONG w = rc.right  - rc.left;
    LONG h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) { log_error("client area is 0x0"); return FALSE; }

    HDC     src_dc  = GetDC(hwnd);
    HDC     dst_dc  = NULL;
    HBITMAP dst_bmp = NULL;
    BYTE*   pixels  = NULL;
    boole   ok      = FALSE;
    FILE*   fp      = NULL;

    if (src_dc == NULL) { log_error("GetDC failed"); goto cleanup; }
    dst_dc = CreateCompatibleDC(src_dc);
    if (dst_dc == NULL) { log_error("CreateCompatibleDC failed"); goto cleanup; }

    //
    // Target bitmap in 32-bit BGRA, top-down (negative biHeight so
    // row 0 is the top row in memory). BI_RGB + 32 bpp = plain BGRA
    // with DWORD row alignment -- for any width the stride is
    // w * 4, no padding needed.
    //
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;          // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    dst_bmp = CreateDIBSection(src_dc, &bmi, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
    if (dst_bmp == NULL || pixels == NULL) { log_error("CreateDIBSection failed"); goto cleanup; }

    HGDIOBJ old_bmp = SelectObject(dst_dc, dst_bmp);
    BOOL blt_ok = BitBlt(dst_dc, 0, 0, w, h, src_dc, 0, 0, SRCCOPY);
    SelectObject(dst_dc, old_bmp);
    if (!blt_ok) { log_error("BitBlt failed"); goto cleanup; }

    //
    // BMP file: compose headers + pixels. biHeight in the stored
    // BITMAPINFOHEADER must be POSITIVE so legacy viewers flip correctly;
    // but our `pixels` buffer is top-down (because we asked for -h).
    // So: keep biHeight positive, and write rows bottom-up to the file.
    //
    DWORD row_bytes   = (DWORD)(w * 4);
    DWORD pixel_bytes = row_bytes * (DWORD)h;
    DWORD file_off    = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    DWORD file_size   = file_off + pixel_bytes;

    BITMAPFILEHEADER bfh;
    ZeroMemory(&bfh, sizeof(bfh));
    bfh.bfType      = 0x4D42; // 'BM'
    bfh.bfSize      = file_size;
    bfh.bfOffBits   = file_off;

    BITMAPINFOHEADER bih;
    ZeroMemory(&bih, sizeof(bih));
    bih.biSize        = sizeof(BITMAPINFOHEADER);
    bih.biWidth       = w;
    bih.biHeight      = h;        // POSITIVE: legacy readers expect rows stored bottom-up.
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage   = pixel_bytes;

    fp = fopen(path, "wb");
    if (fp == NULL) { log_error("fopen failed for %s", path); goto cleanup; }
    if (fwrite(&bfh, sizeof(bfh), 1, fp) != 1) { log_error("fwrite BFH"); goto cleanup; }
    if (fwrite(&bih, sizeof(bih), 1, fp) != 1) { log_error("fwrite BIH"); goto cleanup; }
    //
    // Pixel rows: buffer is top-down (row 0 = top), file wants bottom-up.
    // Walk from last row down to row 0 and write each out.
    //
    for (LONG row = h - 1; row >= 0; row--)
    {
        BYTE* src = pixels + (size_t)row * row_bytes;
        if (fwrite(src, 1, row_bytes, fp) != row_bytes) { log_error("fwrite row %ld", row); goto cleanup; }
    }
    ok = TRUE;

cleanup:
    if (fp      != NULL) { fclose(fp); }
    if (dst_bmp != NULL) { DeleteObject(dst_bmp); }
    if (dst_dc  != NULL) { DeleteDC(dst_dc); }
    if (src_dc  != NULL) { ReleaseDC(hwnd, src_dc); }
    return ok;
}

//
//The actual `int main()` entry that forwards to the host's
//(renamed) `app_main` lives in platforms/windows/main_entry_win32.c
//and gets compiled into the host EXECUTABLE, not into gui.dll. The
//DLL can't resolve `app_main` at link time because the symbol
//belongs to the exe -- putting the stub here would fail the gui.dll
//link with "undefined symbol: app_main". Each Windows host's
//CMakeLists (e.g. demo-windows/CMakeLists.txt) adds that entry
//stub file to its `add_executable(...)` source list.
//

//
//register the window class.
//
static boole _platform_win32_internal__register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = _platform_win32_internal__wnd_proc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = _platform_win32_internal__class_name;
    return (boole)(RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}

//
//the one and only window procedure. translates win32 messages into
//scene__on_* calls, keeps a bit of platform state up to date, and
//delegates the rest to DefWindowProcW.
//
static LRESULT CALLBACK _platform_win32_internal__wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CLOSE:
        {
            _platform_win32_internal__g.should_close = TRUE;
            PostQuitMessage(0);
            return 0;
        }

        case WM_SIZE:
        {
            int64 w = (int64)LOWORD(lp);
            int64 h = (int64)HIWORD(lp);
            _platform_win32_internal__g.viewport_w = w;
            _platform_win32_internal__g.viewport_h = h;
            scene__on_resize(w, h);
            return 0;
        }

        case 0x02E0: // WM_DPICHANGED (Windows 8.1+, defined by value so we don't need shellscalingapi.h)
        {
            //
            //Fired when the window moves to a monitor with a
            //different DPI, or when the user changes display scaling.
            //wParam low word = new DPI (high word same on V2; per-axis
            //on V1). lParam points to a suggested new window rect
            //that preserves physical size across the DPI change;
            //ignoring it would mean the window shrinks or grows
            //awkwardly, so we apply it via SetWindowPos.
            //
            UINT new_dpi = LOWORD(wp);
            if (new_dpi < 72) { new_dpi = 96; }
            float new_scale = (float)new_dpi / 96.0f;
            scene__set_scale(new_scale);
            RECT* suggested = (RECT*)lp;
            if (suggested != NULL)
            {
                SetWindowPos(hwnd, NULL,
                             suggested->left, suggested->top,
                             suggested->right  - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            log_info("platform_win32: DPI changed to %u (scale %.2fx)", new_dpi, (double)new_scale);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            int64 x = (int64)(SHORT)LOWORD(lp);
            int64 y = (int64)(SHORT)HIWORD(lp);
            _platform_win32_internal__g.mouse_x = x;
            _platform_win32_internal__g.mouse_y = y;
            scene__on_mouse_move(x, y);
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            SetCapture(hwnd);
            int64 x = (int64)(SHORT)LOWORD(lp);
            int64 y = (int64)(SHORT)HIWORD(lp);
            scene__on_mouse_button(0, TRUE, x, y);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            ReleaseCapture();
            int64 x = (int64)(SHORT)LOWORD(lp);
            int64 y = (int64)(SHORT)HIWORD(lp);
            scene__on_mouse_button(0, FALSE, x, y);
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            //
            // WM_MOUSEWHEEL delivers the cursor position in SCREEN
            // coordinates (not client) in lParam -- different from
            // the WM_MOUSE* messages. ScreenToClient converts back
            // to the window's coordinate space so scene__hit_test
            // finds the right node.
            //
            // wParam high word carries a signed 16-bit delta in
            // units of WHEEL_DELTA (120). A "standard" wheel tick
            // gives ±WHEEL_DELTA; high-res wheels pass smaller
            // fractions of WHEEL_DELTA. Positive = wheel moved AWAY
            // from the user (content scrolls up), which matches the
            // convention scene__on_mouse_wheel expects.
            //
            POINT p;
            p.x = (SHORT)LOWORD(lp);
            p.y = (SHORT)HIWORD(lp);
            ScreenToClient(hwnd, &p);
            int   raw   = (int)(SHORT)HIWORD(wp);
            float delta = (float)raw / (float)WHEEL_DELTA;
            scene__on_mouse_wheel((int64)p.x, (int64)p.y, delta);
            return 0;
        }

        case WM_CHAR:
        {
            //
            //WM_CHAR arrives after TranslateMessage synthesizes it from
            //WM_KEYDOWN, already carrying a codepoint (UTF-16 surrogate
            //halves for BMP chars land as two successive WM_CHAR messages;
            //the Input widget only accepts 32..255 so surrogates are
            //filtered out downstream). the widget's on_char handler
            //decides whether to append; control codes (backspace=0x08,
            //tab=0x09, escape=0x1B, ...) flow through but typically get
            //dropped by the widget.
            //
            scene__on_char((uint)wp);
            return 0;
        }

        case WM_KEYDOWN:
        case WM_KEYUP:
        {
            //
            //key-level events (as opposed to WM_CHAR's translated text).
            //forwarded so widgets can handle non-character actions:
            //backspace, delete, arrow keys, enter, ctrl+shortcuts. we
            //still let DefWindowProcW run afterward so TranslateMessage
            //can synthesize the matching WM_CHAR.
            //
            scene__on_key((int64)wp, msg == WM_KEYDOWN);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        case WM_ERASEBKGND:
        {
            //
            //the renderer owns the client area; skip gdi's default fill
            //to avoid flicker on resize.
            //
            return 1;
        }

        default:
        {
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }
}

//
//best-effort win32 error log. no-op if there's no error code pending.
//
static void _platform_win32_internal__log_win32_error(char* where)
{
    DWORD err = GetLastError();
    log_error("%s failed (error %lu)", where, (unsigned long)err);
}

//
//symbol resolver: find a function by name in the host process's exe.
//GetModuleHandleW(NULL) returns the host exe's module regardless of
//whether the caller lives in gui.dll or in the exe itself. functions
//marked UI_HANDLER (__declspec(dllexport)) end up in the exe's export
//table and are reachable through GetProcAddress.
//
static gui_handler_fn _platform_win32_internal__resolve_host_symbol(char* name)
{
    HMODULE host = GetModuleHandleW(NULL);
    if (host == NULL)
    {
        return NULL;
    }
    FARPROC sym = GetProcAddress(host, name);
    return (gui_handler_fn)(void*)sym;
}
