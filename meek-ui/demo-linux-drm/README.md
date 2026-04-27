# demo-linux-drm

Linux DRM/KMS kiosk host for the gui library. Draws straight to the
display via `/dev/dri/card0`, bypassing X11 and Wayland. Intended for
embedded UIs, kiosks, digital signage, and the "ssh into a headless
box and want a framebuffer UI" scenario.

Companion to:
- `../demo-windows/` — Windows host (CMake + Clang)
- `../demo-android/` — Android host (NDK clang)
- `../demo-linux-drm/` — this one
- `../demo-linux-x11/` (TBD) — same library, X11 windowed host

The shared `main.ui` + `main.style` live in `../demo-windows/`; this
directory only owns the host entry `main.c`, the build script, and
the staged assets copied alongside the binary.

## Prerequisites

### Distro packages

Ubuntu / Debian:
```
sudo apt-get install clang libdrm-dev libgbm-dev libegl-dev libgles-dev pkg-config
```

Fedora:
```
sudo dnf install clang libdrm-devel mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel pkgconf-pkg-config
```

Arch:
```
sudo pacman -S clang libdrm mesa pkgconf
```

`./build.sh deps` prints these same commands if you need a reminder.

### Group membership

The program needs read access to `/dev/dri/card*` and
`/dev/input/event*`:

```
sudo usermod -aG video,input $USER
# log out and log back in for the group change to take effect
```

Alternatively run the binary as root (`sudo ./guidemo`), but group
membership is the cleaner approach.

### Free the display

DRM master is exclusive — only one process can own the card at a
time. On a typical desktop, X11 / Wayland (via GDM / SDDM /
LightDM) is already DRM master on `tty1`. Either:

- **Switch tty**: `Ctrl+Alt+F3` to reach a free text console, log
  in, and run from there.
- **Stop the display manager**: `sudo systemctl stop gdm` (or
  `sddm` / `lightdm`). Restart it when done.
- **Boot into multi-user.target** on a headless box — no DM starts,
  the tty is always free.

If DRM master is held, `drmModeSetCrtc` returns `EBUSY` at startup
and the binary logs the error and exits.

### Not supported: WSL2

WSL2 doesn't work — and it's not a matter of permissions or
package availability. WSL2's kernel deliberately doesn't expose
a DRM device. The Microsoft GPU integration surfaces `/dev/dxg`
(DirectX compute) but **no `/dev/dri/card*`**, so this backend
has nothing to open. The X11 backend (`../demo-linux-x11/`)
does work on WSL2 via WSLg; use that instead.

## Build + run

```
./build.sh            # build + run (default)
./build.sh build      # build only
./build.sh run        # run only (requires prior build)
./build.sh clean      # wipe build output + staged assets
./build.sh deps       # print dependency install lines
```

Output: `./guidemo` (dynamically-linked ELF next to this file) plus
a staged `./assets/` directory mirroring the layout
`demo-windows/` ships (`main.ui`, `main.style`, `wallpapers/*.jpg`,
`fonts/*.ttf`).

## What works

- 60 fps rendering via GBM + EGL + GLES 3 through the
  `gles3_renderer.c` backend (same backend Android uses).
- Page-flip with vsync via `drmModePageFlip` + vblank-driven poll.
- Raw evdev input: keyboard, mouse (relative + wheel), absolute
  touchscreen (slot 0 only).
- Hot reload of `main.ui` / `main.style` through `hot_reload.c` —
  edit the files under `./assets/` and the app picks up changes
  within a frame.
- The full widget set: buttons, sliders, text, inputs, checkboxes,
  radios, selects (dropdown), images, color picker, popups, canvas,
  on-screen keyboard.
- Theme toggle, display/visibility toggles, canvas painting — all
  the same handlers as the Windows / Android demos, auto-resolved
  via `dlsym(RTLD_DEFAULT, ...)` on `UI_HANDLER`-marked symbols.
- DPI-aware scale factor picked from the connector's `mmWidth` +
  `hdisplay` at startup.

## Known limits

- **Single pointer.** Multi-touch beyond the first finger is
  ignored, same as Android. Two-finger pan / pinch zoom not wired.
- **Minimal US-QWERTY keymap.** The evdev-to-char translator is a
  hand-rolled ASCII table — no xkbcommon integration, no dead keys,
  no non-US layouts. Enough for numeric input + English text demos,
  not a production text editor.
- **First connected connector wins.** Multi-monitor setups render
  only on the first connected display. No way to pick a different
  one short of editing `_platform_linux_drm_internal__try_card`.
- **`/dev/dri/card0` first.** Machines with multiple GPUs pick the
  first one with a connected connector; there's no env override.
- **DRM master required.** No "headless render into a dmabuf" path
  yet. This is a display backend; render-only usage (CI screenshots,
  GPU compute) would need a different init path.

## Architecture notes

Entry point is plain `int main(int, char**)`. `platform.h` renames
it to `app_main` so `platform_linux_drm.c`'s own `main` (soon —
right now `main` lives in `main.c` because DRM doesn't need an
android-style trampoline) can forward to it. Today the Linux DRM
host uses the host's `main` directly because there's no platform-
specific entry-point shape (unlike Android's `android_main(struct
android_app*)`).

Frame loop per `platform__tick`:

1. `poll_input` drains all `/dev/input/event*` fds non-blockingly
   and dispatches translated events into `scene__on_*`.
2. `scene__begin_frame_time` / `resolve_styles` / `animator__tick`
   / `scene__layout` / `renderer__begin_frame` / `scene__emit_draws`
   / `renderer__end_frame` — the same pipeline every platform runs.
3. `eglSwapBuffers` hands the rendered back buffer to GBM.
4. `gbm_surface_lock_front_buffer` pulls the BO out.
5. `drmModeAddFB2` (cached per-BO via user-data) turns the BO into
   a DRM framebuffer id.
6. `drmModePageFlip(DRM_MODE_PAGE_FLIP_EVENT)` schedules the flip
   for the next vblank.
7. `poll` + `drmHandleEvent` waits for the flip-done event (this
   is where the frame rate gets pinned to vsync).
8. `gbm_surface_release_buffer` returns the previous-frame BO to
   the GBM free list.

Shutdown restores the CRTC state captured at init (`drmModeGetCrtc`
result stashed on `saved_crtc`), so the VT's framebuffer comes back
readable when the program exits.
