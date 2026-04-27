# demo-macos

macOS desktop host for the gui library. Cocoa + NSOpenGLView with
a 4.1 Core Profile context, rendering through
`gles3_renderer.c` (the renderer auto-switches its GLSL version
preamble to `410` on `__APPLE__`). Runs on Intel and Apple Silicon
Macs.

Companion to:
- `../demo-windows/` — Windows host
- `../demo-android/` — Android host
- `../demo-linux-drm/` — Linux kiosk host
- `../demo-linux-x11/` — Linux desktop host
- **`../demo-macos/`** — this one

`main.ui` + `main.style` are shared with `../demo-windows/`;
`build.sh` stages them next to the binary.

## Prerequisites

- macOS 10.12 (Sierra) or newer. Older might work;
  `NSOpenGLProfileVersion4_1Core` has been available since 10.9
  and the Cocoa APIs we use predate Sierra, but untested.
- Apple's Command Line Tools:
  ```
  xcode-select --install
  ```
  Provides clang + system frameworks. Full Xcode is not required.

## Build + run

```
./build.sh              # build + run (default)
./build.sh build        # build only
./build.sh run          # run only
./build.sh clean        # wipe build output + staged assets
```

Output: `./guidemo` (Mach-O, single binary linked against
system frameworks) plus `./assets/` staged from
`../demo-windows/` and `../gui/src/fonts/`.

The binary is built for the host architecture. To build a
universal binary, add `-arch arm64 -arch x86_64` to the clang
invocation in `build.sh`; left off by default because most
macOS dev machines target the host arch only.

## What works

- 60 fps windowed rendering; swap interval pinned to vblank
  via `NSOpenGLContextParameterSwapInterval`.
- Retina / high-DPI: `backingScaleFactor` (1.0 on non-Retina,
  2.0 on standard Retina, 3.0 on Pro Display XDR) feeds
  directly into `scene__set_scale`. Events arrive in backing-
  pixel coordinates so mouse hit-test matches rendering.
- Window resize via the chrome re-layouts the app.
- Cmd+Q / close button / kill signal: `windowShouldClose`
  sets a flag and the main loop exits cleanly.
- Keyboard: full Unicode text input through
  `charactersIgnoringModifiers`. Compose sequences
  (Option+E, E → é) work. Codepoints >0xFF are passed
  through; widgets that can't render them fall through
  to their own limits.
- Navigation keys (BACKSPACE / TAB / RETURN / ESCAPE /
  arrows / DELETE / HOME / END / PAGE UP / PAGE DOWN) map
  to the same VK codes Windows uses.
- Mouse: left/middle/right + trackpad scroll (precise
  pixel deltas normalized to tick units).
- `UI_HANDLER` callbacks auto-resolved via
  `dlsym(RTLD_DEFAULT, ...)`.
- Hot reload of `main.ui` / `main.style`.

## Known limits

- **OpenGL is deprecated** on macOS. Still works today on
  every current release but Apple could remove it
  eventually. A Metal renderer (`metal_renderer.m`) is
  the long-term answer; not yet written.
- **No bundle wrapping.** The binary ships as a plain
  Mach-O executable, not as a `.app` bundle. Works fine
  when launched from a terminal; double-clicking in Finder
  won't work without wrapping. A follow-up could add a
  `Contents/Info.plist` + `Contents/MacOS/guidemo`
  structure in `build.sh`.
- **No menu bar beyond default.** Cocoa gives us the
  standard app menu for free (Quit, Hide, etc.) but we
  don't populate a File / Edit / View menu structure.
- **No native file-open/save dialogs** for content; the
  demo doesn't need them.
- **No per-monitor DPI re-pick.** If the window moves
  between displays of different backing-scale factors,
  scale doesn't update until restart. `NSWindowDidChangeBackingPropertiesNotification`
  is the fix.

## Architecture

```
demo-macos/main.c
  ↓ UI_HANDLER-marked fns
gui/src/platforms/macos/platform_macos.m    (Objective-C)
  ↓ Cocoa + NSOpenGLContext + NSView event routing
gui/src/renderers/gles3_renderer.c          (C, Apple branch for GLSL 410)
  ↓ GL 4.1 Core draw calls via <OpenGL/gl3.h>
gui/src/platforms/macos/fs_macos.c          (POSIX fs, mirror of fs_linux.c)
```

Per-tick sequence:
1. `[NSApp nextEventMatchingMask:NSEventMaskAny
   untilDate:[NSDate distantPast] ...]` drains the event
   queue non-blockingly; `sendEvent:` routes each event to
   the view / window delegate.
2. `scene__begin_frame_time` / `resolve_styles` / `animator__tick`
   / `layout` / `renderer__begin_frame` / `emit_draws` /
   `renderer__end_frame` — the usual pipeline.
3. `[[g_view openGLContext] flushBuffer]` presents. Apple's
   equivalent of `SwapBuffers` / `eglSwapBuffers`.

`platform_macos.m` is compiled with `-fobjc-arc` (Automatic
Reference Counting). The window / view / delegate are
held via strong globals so ARC keeps them alive for the
process lifetime; `platform__shutdown` releases them by
setting the globals to `nil`.
