#!/usr/bin/env bash
# =============================================================================
# build.sh - Linux DRM/KMS build for this repo.
# =============================================================================
#
# Direct clang invocation, no CMake. Mirrors the structure of
# demo-android/build.sh (which also drives the NDK directly) -- same
# per-source compile + link into a single binary, same asset staging
# step, same default: build + run.
#
# OUTPUT: $SCRIPT_DIR/guidemo   (ELF, dynamically linked)
#
# -----------------------------------------------------------------------------
# USAGE
# -----------------------------------------------------------------------------
#
#   ./build.sh                    # DEFAULT: build + run
#   ./build.sh build              # build only, no run
#   ./build.sh run                # run only (no rebuild; must have built first)
#   ./build.sh clean              # wipe build outputs
#   ./build.sh deps               # echo apt-get / dnf / pacman install lines
#
# -----------------------------------------------------------------------------
# DEPENDENCIES
# -----------------------------------------------------------------------------
#
# Ubuntu / Debian:
#   sudo apt-get install clang libdrm-dev libgbm-dev libegl-dev libgles-dev
# Fedora:
#   sudo dnf install clang libdrm-devel mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel
# Arch:
#   sudo pacman -S clang libdrm mesa
#
# Plus runtime groups on the user who'll run the binary:
#   sudo usermod -aG video,input $USER     # log out / log back in after
#
# Running:
#   Switch to a text console (Ctrl+Alt+F3), log in, cd to this directory,
#   ./build.sh run. If X11 / Wayland is running on tty1 and your session is
#   there, drmModeSetCrtc will return EBUSY -- switch to a free tty or
#   stop the display manager (sudo systemctl stop gdm / sddm / lightdm).
#

set -euo pipefail

APPNAME=${APPNAME:-guidemo}
TARGET=${1:-run}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="$SCRIPT_DIR/build"
BIN_OUT="$SCRIPT_DIR/$APPNAME"

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

info()  { printf '\033[36m[build]\033[0m %s\n' "$*"; }
ok()    { printf '\033[32m[build]\033[0m %s\n' "$*"; }
err()   { printf '\033[31m[build]\033[0m %s\n' "$*" >&2; }
fatal() { err "$*"; exit 1; }

# -----------------------------------------------------------------------------
# Compiler pick. Prefer clang (matches the project toolchain convention);
# fall back to gcc. CC env override wins.
# -----------------------------------------------------------------------------

if [[ -n "${CC:-}" ]]; then
    :
elif command -v clang >/dev/null; then
    CC=clang
elif command -v gcc >/dev/null; then
    CC=gcc
else
    fatal "no C compiler found. Install clang or gcc."
fi

# -----------------------------------------------------------------------------
# pkg-config lookups for libdrm / gbm / egl / gles
# -----------------------------------------------------------------------------
#
# Using pkg-config keeps us honest across distros -- Debian stores
# libdrm's headers under /usr/include/libdrm, Arch under /usr/include
# directly, Fedora somewhere in between. The --cflags / --libs
# queries return the right -I and -l flags regardless.
#
# We collect ALL missing pieces before bailing out so the user gets
# one consolidated `sudo apt-get install ...` line to run instead of
# the whack-a-mole "missing X → install → rerun → missing Y" loop.
# Mapping from pkg-config id to Debian/Ubuntu package name lives in
# the `apt_for` function; Fedora / Arch variants are echoed by
# `./build.sh deps`.
#

apt_for() {
    case "$1" in
        pkg-config) echo "pkg-config" ;;
        libdrm)     echo "libdrm-dev" ;;
        gbm)        echo "libgbm-dev" ;;
        egl)        echo "libegl-dev" ;;
        glesv2)     echo "libgles-dev" ;;
        x11)        echo "libx11-dev" ;;
        *)          echo "$1" ;;
    esac
}

missing_apt=()

if ! command -v pkg-config >/dev/null; then
    missing_apt+=("$(apt_for pkg-config)")
else
    for pkg in libdrm gbm egl glesv2; do
        if ! pkg-config --exists "$pkg"; then
            missing_apt+=("$(apt_for "$pkg")")
        fi
    done
fi

if [[ ${#missing_apt[@]} -gt 0 ]]; then
    err "missing build dependencies: ${missing_apt[*]}"
    err "install all at once with:"
    err "  sudo apt-get install ${missing_apt[*]}"
    err "(for Fedora / Arch equivalents see: ./build.sh deps)"
    exit 1
fi

DRM_CFLAGS="$(pkg-config --cflags libdrm gbm egl glesv2)"
DRM_LIBS="$(pkg-config --libs   libdrm gbm egl glesv2)"

# -----------------------------------------------------------------------------
# Sources -- mirrors demo-android/build.sh's list, minus the Android-only
# bits (native_app_glue, fs_android) and plus the Linux-specific bits
# (fs_linux, platform_linux_drm).
# -----------------------------------------------------------------------------

SOURCES=(
    "$ROOT/gui/src/scene.c"
    "$ROOT/gui/src/scene_style.c"
    "$ROOT/gui/src/scene_layout.c"
    "$ROOT/gui/src/scene_render.c"
    "$ROOT/gui/src/scene_input.c"
    "$ROOT/gui/src/platforms/linux/fs_linux.c"
    "$ROOT/gui/src/font.c"
    "$ROOT/gui/src/scroll.c"
    "$ROOT/gui/src/animator.c"
    "$ROOT/gui/src/parser_xml.c"
    "$ROOT/gui/src/parser_style.c"
    "$ROOT/gui/src/widget_registry.c"
    "$ROOT/gui/src/widgets/widget_window.c"
    "$ROOT/gui/src/widgets/widget_column.c"
    "$ROOT/gui/src/widgets/widget_row.c"
    "$ROOT/gui/src/widgets/widget_button.c"
    "$ROOT/gui/src/widgets/widget_slider.c"
    "$ROOT/gui/src/widgets/widget_div.c"
    "$ROOT/gui/src/widgets/widget_text.c"
    "$ROOT/gui/src/widgets/widget_input.c"
    "$ROOT/gui/src/widgets/widget_checkbox.c"
    "$ROOT/gui/src/widgets/widget_radio.c"
    "$ROOT/gui/src/widgets/widget_select.c"
    "$ROOT/gui/src/widgets/widget_option.c"
    "$ROOT/gui/src/widgets/widget_image.c"
    "$ROOT/gui/src/widgets/widget_collection.c"
    "$ROOT/gui/src/widgets/widget_colorpicker.c"
    "$ROOT/gui/src/widgets/widget_popup.c"
    "$ROOT/gui/src/widgets/widget_textarea.c"
    "$ROOT/gui/src/widgets/widget_canvas.c"
    "$ROOT/gui/src/widgets/widget_keyboard.c"
    "$ROOT/gui/src/third_party/log.c"
    "$ROOT/gui/src/renderers/gles3_renderer.c"
    "$ROOT/gui/src/platforms/linux/platform_linux_drm.c"
    "$ROOT/gui/src/clib/memory_manager.c"
    "$ROOT/gui/src/clib/stdlib.c"
    "$ROOT/gui/src/hot_reload.c"
    "$SCRIPT_DIR/main.c"
)

# -----------------------------------------------------------------------------
# Build-time defines header (same rationale as demo-android/build.sh:
# embedded-quote -D flags are painful to pass through shell layers, so
# we use -include instead).
# -----------------------------------------------------------------------------

gen_defines() {
    mkdir -p "$BUILD_DIR"
    local defs="$BUILD_DIR/linux_drm_build_defines.h"
    #
    # DEMO_SOURCE_DIR is the assets dir next to the binary. Build.sh
    # stages main.ui / main.style / wallpapers / fonts into
    # $SCRIPT_DIR/assets/ and we pass that absolute path in here so
    # fs_linux.c's open() resolves "<dir>/main.style" directly.
    #
    # GUI_FONTS_SOURCE_DIR is read by font.c on init to auto-scan
    # for *.ttf. Pointing it at the staged fonts/ subdir gives us
    # the same family-by-name resolution the other platforms use.
    #
    cat > "$defs" <<EOF
/* Auto-generated by build.sh -- do not edit. */
#ifndef LINUX_DRM_BUILD_DEFINES_H
#define LINUX_DRM_BUILD_DEFINES_H
#define APPNAME "$APPNAME"
#define DEMO_SOURCE_DIR "$SCRIPT_DIR/assets"
#define GUI_FONTS_SOURCE_DIR "$SCRIPT_DIR/assets/fonts"
#endif
EOF
}

# -----------------------------------------------------------------------------
# Asset staging. Copies main.ui / main.style / wallpapers / fonts next
# to the binary. Mirrors demo-android's APK asset staging but to a
# filesystem dir.
# -----------------------------------------------------------------------------

stage_assets() {
    local dst="$SCRIPT_DIR/assets"
    mkdir -p "$dst"
    mkdir -p "$dst/fonts"
    mkdir -p "$dst/wallpapers"

    #
    # .ui / .style / loose images from demo-windows. Copy only if
    # newer -- preserves any local overrides if the user has
    # dropped a modified main.ui into demo-linux-drm/assets/.
    #
    for f in "$ROOT/demo-windows"/*.ui "$ROOT/demo-windows"/*.style "$ROOT/demo-windows"/*.png "$ROOT/demo-windows"/*.jpg "$ROOT/demo-windows"/*.jpeg; do
        if [ -f "$f" ]; then
            bn=$(basename "$f")
            #
            # Skip filenames with non-ASCII bytes. A leftover from
            # the demo-windows sources; not referenced by the UI.
            #
            if printf '%s' "$bn" | LC_ALL=C grep -q '[^ -~]'; then
                continue
            fi
            cp -u "$f" "$dst/"
        fi
    done

    if [ -d "$ROOT/demo-windows/wallpapers" ]; then
        for f in "$ROOT/demo-windows/wallpapers"/*.png "$ROOT/demo-windows/wallpapers"/*.jpg "$ROOT/demo-windows/wallpapers"/*.jpeg; do
            if [ -f "$f" ]; then
                bn=$(basename "$f")
                if printf '%s' "$bn" | LC_ALL=C grep -q '[^ -~]'; then
                    continue
                fi
                cp -u "$f" "$dst/wallpapers/"
            fi
        done
    fi

    if [ -d "$ROOT/gui/src/fonts" ]; then
        for f in "$ROOT/gui/src/fonts"/*.ttf; do
            if [ -f "$f" ]; then
                cp -u "$f" "$dst/fonts/"
            fi
        done
    fi

    ok "staged assets -> $dst"
}

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------

target_build() {
    gen_defines
    stage_assets

    local defs="$BUILD_DIR/linux_drm_build_defines.h"

    local cflags=(
        -O2 -g
        -fvisibility=hidden
        -Wall -Wno-unused-function
        -DGUI_TRACK_ALLOCATIONS
        -include "$defs"
        -I"$ROOT/gui/src"
    )
    # pkg-config returns multi-token strings; let the shell split.
    # shellcheck disable=SC2206
    cflags+=($DRM_CFLAGS)

    local ldflags=(
        -lm -ldl -lpthread
    )
    # shellcheck disable=SC2206
    ldflags+=($DRM_LIBS)

    info "compiling $APPNAME"
    "$CC" "${cflags[@]}" -o "$BIN_OUT" "${SOURCES[@]}" "${ldflags[@]}"
    ok "built $BIN_OUT"
    local size
    size=$(stat -c%s "$BIN_OUT" 2>/dev/null || stat -f%z "$BIN_OUT" 2>/dev/null || echo '?')
    info "size: $size bytes"
}

target_run() {
    target_build
    #
    # The binary needs group `video` (for /dev/dri/card*) and `input`
    # (for /dev/input/event*). If the user isn't in those groups and
    # hasn't run as root, the DRM open will fail with EACCES or the
    # input devices will silently enumerate zero and the app will be
    # non-interactive. Either outcome is logged; we just launch and
    # let the logs explain.
    #
    info "running $BIN_OUT"
    "$BIN_OUT"
}

target_clean() {
    for p in "$BUILD_DIR" "$BIN_OUT" "$SCRIPT_DIR/assets"; do
        if [[ -e "$p" ]]; then
            info "removing $p"
            rm -rf "$p"
        fi
    done
    ok 'clean done'
}

target_deps() {
    cat <<'EOF'
Install one set depending on your distro:

  # Ubuntu / Debian:
  sudo apt-get install clang libdrm-dev libgbm-dev libegl-dev libgles-dev pkg-config

  # Fedora:
  sudo dnf install clang libdrm-devel mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel pkgconf-pkg-config

  # Arch:
  sudo pacman -S clang libdrm mesa pkgconf

Then add your user to the video + input groups (required for DRM + evdev):
  sudo usermod -aG video,input $USER
  # log out and log back in for the group membership to take effect

Run from a text console (Ctrl+Alt+F3, log in, ./build.sh) or stop your
display manager first (sudo systemctl stop gdm / sddm / lightdm). The
program needs to be DRM master, which means no X11 / Wayland session
may hold the card.
EOF
}

# -----------------------------------------------------------------------------
# Dispatch
# -----------------------------------------------------------------------------

case "$TARGET" in
    run|'')   target_run ;;
    build)    target_build ;;
    clean)    target_clean ;;
    deps)     target_deps ;;
    *)        err "Unknown target: $TARGET"; echo "Valid: run (default), build, clean, deps"; exit 2 ;;
esac
