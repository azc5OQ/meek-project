# demo-linux-x11

Linux X11 desktop host for the gui library. Runs as a normal X11
client — opens a window, sets up EGL on the X display, renders
through `gles3_renderer.c` (same renderer Android and DRM use),
reads input from XEvent. No DRM master required, no tty switch,
coexists with your display manager.

Companion to:
- `../demo-windows/` — Windows host
- `../demo-android/` — Android host
- `../demo-linux-drm/` — Linux kiosk host (direct-to-GPU)
- `../demo-linux-x11/` — **this one** — Linux desktop host
- `../demo-macos/` (TBD) — macOS Cocoa host

The shared `main.ui` + `main.style` live in `../demo-windows/`;
`build.sh` stages them next to the binary.

## Prerequisites

```
# Ubuntu / Debian:
sudo apt-get install clang libx11-dev libegl-dev libgles-dev pkg-config

# Fedora:
sudo dnf install clang libX11-devel mesa-libEGL-devel mesa-libGLES-devel pkgconf-pkg-config

# Arch:
sudo pacman -S clang libx11 mesa pkgconf
```

No group membership needed (X11 handles permissions via the
display cookie). `DISPLAY` just needs to be set — always true in
a graphical session, and `ssh -X` forwarding works too.

### WSL2 (Windows 11)

This is the **recommended** way to try the toolkit from a Windows
11 machine without reaching for a separate Linux box.

WSL2 on Windows 11 ships **WSLg** — a Microsoft-maintained tiny
Wayland compositor + XWayland bridge that ships enabled by
default. You do **not** need gnome / xfce4 / KDE / any other
desktop environment; WSLg handles the window on its own. Running
a full DE on top of WSL2 actually tends to crash because there's
no systemd session / dbus / seat infrastructure the way a
full desktop expects.

The happy path:

```bash
# Inside your WSL2 shell (Ubuntu/Debian):
sudo apt-get install clang libx11-dev libegl-dev libgles-dev pkg-config
git clone <this repo>
cd meh/demo-linux-x11
./build.sh
```

The binary builds, the window pops up on your Windows 11
desktop as a native window, input routes through, and hot
reload of `main.ui` / `main.style` works. `DISPLAY=:0` is
already set by WSLg; you don't need to configure anything.

**Windows 10 WSL2** (no WSLg): install VcXsrv or X410 on the
Windows side, `export DISPLAY=$(ip route | awk '/default/ {print $3}'):0.0`
in the WSL shell, disable VcXsrv's access control, and it works
the same way. Upgrading to Windows 11 is the simpler option.

### Why not DRM on WSL2?

Common confusion: "DRM is the lowest-level backend, so it
should work everywhere including WSL2." Not the case. DRM
requires `/dev/dri/card*` from the kernel, and WSL2's kernel
doesn't expose a display device (only `/dev/dxg` for GPU
compute via Microsoft's DxgKrnl translation). X11 on WSL2 works
because WSLg provides the X server — there's no such bridge for
DRM modeset.

## Build + run

```
./build.sh              # build + run (default)
./build.sh build        # build only
./build.sh run          # run only
./build.sh clean        # wipe build output + staged assets
./build.sh deps         # print distro-specific install lines
```

Output: `./guidemo` (dynamically linked ELF) plus `./assets/`
(copied from `../demo-windows/` + `../gui/src/fonts/`).

## What works

- 60 fps windowed rendering at whatever size the WM gives us;
  resize via the WM resizes the window and the app re-lays out.
- `Ctrl+Q` / close button / kill signal: WM_DELETE_WINDOW fires
  and the app exits cleanly.
- Keyboard: full text entry through XIM (so dead-key compose like
  `´ + e = é` works) for characters in the ASCII + Latin-1 range
  the existing scene input widgets accept. Codepoints above
  `0xff` come through as `?` (atlas limitation).
- Navigation keys (BACKSPACE / TAB / ENTER / ESCAPE / arrows /
  DELETE / HOME / END / PAGE UP / PAGE DOWN) map to the same VK
  codes Windows uses so scene handling is shared.
- Mouse: left/middle/right buttons, wheel up/down.
- DPI: picked from `XDisplayWidth / XDisplayWidthMM` at startup.
  Distros that correctly report the panel's physical size get
  a matching scale factor; others stay at 1.0×.
- All the same `UI_HANDLER`-marked callbacks as the other demos,
  auto-resolved via `dlsym(RTLD_DEFAULT, ...)`.
- Hot reload of `main.ui` / `main.style` via `hot_reload.c`
  polling the staged files.

## Known limits

- **No Wayland-native**. The binary runs on Wayland only via
  XWayland (the compatibility layer most Wayland compositors
  ship). A native Wayland backend is pending.
- **Unicode text above Latin-1** shows as `?`. Same atlas-paging
  limit the rest of the toolkit has.
- **No clipboard integration**. Ctrl+C/V don't copy/paste
  through X selections yet.
- **No window icon**. Could add via `_NET_WM_ICON` property but
  we skip it.
- **Single monitor DPI**. Moving the window to a different
  monitor doesn't re-pick scale; the Windows WM_DPICHANGED
  equivalent on X11 (RANDR events + per-output DPI) isn't
  wired up.

## Architecture

```
demo-linux-x11/main.c
  ↓ UI_HANDLER-marked fns
gui/src/platforms/linux/platform_linux_x11.c
  ↓ X11 window + EGL + evdev via XEvent
gui/src/renderers/gles3_renderer.c
  ↓ GL draw calls
gui/src/platforms/linux/fs_linux.c
  ↓ POSIX file I/O
```

Per-tick sequence:
1. `XPending` loop drains every queued X event: ConfigureNotify,
   MotionNotify, ButtonPress/Release, KeyPress/Release,
   FocusIn/Out, ClientMessage (WM_DELETE_WINDOW).
2. `Xutf8LookupString` converts KeyPress to UTF-8 bytes; we walk
   them as codepoints into `scene__on_char`. Non-char keys go
   through a keysym → VK table into `scene__on_key`.
3. Normal pipeline: `scene__begin_frame_time` / `resolve_styles`
   / `animator__tick` / `layout` / `renderer__begin_frame` /
   `emit_draws` / `renderer__end_frame`.
4. `eglSwapBuffers` presents to the X window surface. `eglSwapInterval(1)`
   pins frame pacing to vsync.

Same `fs_linux.c` and POSIX font scan in `font.c` the DRM backend
uses — no duplication.
