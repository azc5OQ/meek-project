#!/usr/bin/env bash
# =============================================================================
# build.sh - Linux X11 build for this repo.
# =============================================================================
#
# Direct clang invocation, no CMake. Mirrors demo-linux-drm/build.sh but
# links X11 + EGL + GLESv2 instead of libdrm + libgbm. Same source list
# modulo platform backend, same asset staging.
#
# OUTPUT: $SCRIPT_DIR/guidemo   (ELF, dynamically linked)
#
# -----------------------------------------------------------------------------
# USAGE
# -----------------------------------------------------------------------------
#
#   ./build.sh                    # DEFAULT: build + run
#   ./build.sh build              # build only
#   ./build.sh run                # run only
#   ./build.sh clean              # wipe build outputs
#   ./build.sh deps               # print apt-get / dnf / pacman install lines
#
# -----------------------------------------------------------------------------
# DEPENDENCIES
# -----------------------------------------------------------------------------
#
# Ubuntu / Debian:
#   sudo apt-get install clang libx11-dev libegl-dev libgles-dev pkg-config
# Fedora:
#   sudo dnf install clang libX11-devel mesa-libEGL-devel mesa-libGLES-devel pkgconf-pkg-config
# Arch:
#   sudo pacman -S clang libx11 mesa pkgconf
#
# Run from an X session (the thing you're already in when you open a
# terminal on a graphical Linux desktop). Wayland-on-XWayland works
# too -- the X protocol is tunneled through. Wayland-native without
# XWayland does NOT work; a Wayland backend is pending.
#

set -euo pipefail

APPNAME=${APPNAME:-guidemo}
TARGET=${1:-run}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="$SCRIPT_DIR/build"
BIN_OUT="$SCRIPT_DIR/$APPNAME"

info()  { printf '\033[36m[build]\033[0m %s\n' "$*"; }
ok()    { printf '\033[32m[build]\033[0m %s\n' "$*"; }
err()   { printf '\033[31m[build]\033[0m %s\n' "$*" >&2; }
fatal() { err "$*"; exit 1; }

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
# pkg-config for x11 + egl + glesv2. libX11 has no hyphen in pkg-config's
# name on any distro; egl / glesv2 are consistent across Mesa + NVIDIA.
# -----------------------------------------------------------------------------
#
# Collect every missing dep first and print one consolidated
# `sudo apt-get install ...` line. Previously each required pkg-config
# name bailed out via `fatal` on the first miss, so a user without any
# of them had to apt-install, rerun, repeat for every iteration.
#

apt_for() {
    case "$1" in
        pkg-config) echo "pkg-config" ;;
        x11)        echo "libx11-dev" ;;
        egl)        echo "libegl-dev" ;;
        glesv2)     echo "libgles-dev" ;;
        *)          echo "$1" ;;
    esac
}

missing_apt=()

if ! command -v pkg-config >/dev/null; then
    missing_apt+=("$(apt_for pkg-config)")
else
    for pkg in x11 egl glesv2; do
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

X11_CFLAGS="$(pkg-config --cflags x11 egl glesv2)"
X11_LIBS="$(pkg-config --libs   x11 egl glesv2)"

# -----------------------------------------------------------------------------
# Sources. Same as demo-linux-drm minus platform_linux_drm, plus
# platform_linux_x11.
# -----------------------------------------------------------------------------

SOURCES=(
    "$ROOT/gui/src/scene.c"
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
    "$ROOT/gui/src/platforms/linux/platform_linux_x11.c"
    "$ROOT/gui/src/clib/memory_manager.c"
    "$ROOT/gui/src/clib/stdlib.c"
    "$ROOT/gui/src/hot_reload.c"
    "$SCRIPT_DIR/main.c"
)

gen_defines() {
    mkdir -p "$BUILD_DIR"
    local defs="$BUILD_DIR/linux_x11_build_defines.h"
    cat > "$defs" <<EOF
/* Auto-generated by build.sh -- do not edit. */
#ifndef LINUX_X11_BUILD_DEFINES_H
#define LINUX_X11_BUILD_DEFINES_H
#define APPNAME "$APPNAME"
#define DEMO_SOURCE_DIR "$SCRIPT_DIR/assets"
#define GUI_FONTS_SOURCE_DIR "$SCRIPT_DIR/assets/fonts"
#endif
EOF
}

stage_assets() {
    local dst="$SCRIPT_DIR/assets"
    mkdir -p "$dst"
    mkdir -p "$dst/fonts"
    mkdir -p "$dst/wallpapers"

    for f in "$ROOT/demo-windows"/*.ui "$ROOT/demo-windows"/*.style "$ROOT/demo-windows"/*.png "$ROOT/demo-windows"/*.jpg "$ROOT/demo-windows"/*.jpeg; do
        if [ -f "$f" ]; then
            bn=$(basename "$f")
            if printf '%s' "$bn" | LC_ALL=C grep -q '[^ -~]'; then continue; fi
            cp -u "$f" "$dst/"
        fi
    done

    if [ -d "$ROOT/demo-windows/wallpapers" ]; then
        for f in "$ROOT/demo-windows/wallpapers"/*.png "$ROOT/demo-windows/wallpapers"/*.jpg "$ROOT/demo-windows/wallpapers"/*.jpeg; do
            if [ -f "$f" ]; then
                bn=$(basename "$f")
                if printf '%s' "$bn" | LC_ALL=C grep -q '[^ -~]'; then continue; fi
                cp -u "$f" "$dst/wallpapers/"
            fi
        done
    fi

    if [ -d "$ROOT/gui/src/fonts" ]; then
        for f in "$ROOT/gui/src/fonts"/*.ttf; do
            if [ -f "$f" ]; then cp -u "$f" "$dst/fonts/"; fi
        done
    fi

    ok "staged assets -> $dst"
}

target_build() {
    gen_defines
    stage_assets

    local defs="$BUILD_DIR/linux_x11_build_defines.h"

    local cflags=(
        -O2 -g
        -fvisibility=hidden
        -Wall -Wno-unused-function
        -DGUI_TRACK_ALLOCATIONS
        -include "$defs"
        -I"$ROOT/gui/src"
    )
    # shellcheck disable=SC2206
    cflags+=($X11_CFLAGS)

    local ldflags=( -lm -ldl -lpthread )
    # shellcheck disable=SC2206
    ldflags+=($X11_LIBS)

    info "compiling $APPNAME"
    "$CC" "${cflags[@]}" -o "$BIN_OUT" "${SOURCES[@]}" "${ldflags[@]}"
    ok "built $BIN_OUT"
    local size
    size=$(stat -c%s "$BIN_OUT" 2>/dev/null || stat -f%z "$BIN_OUT" 2>/dev/null || echo '?')
    info "size: $size bytes"
}

target_run() {
    target_build
    if [[ -z "${DISPLAY:-}" ]]; then
        err "DISPLAY is not set. Run from an X session (or export DISPLAY=:0 over ssh -X)."
        exit 1
    fi
    info "running $BIN_OUT on $DISPLAY"
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
  sudo apt-get install clang libx11-dev libegl-dev libgles-dev pkg-config

  # Fedora:
  sudo dnf install clang libX11-devel mesa-libEGL-devel mesa-libGLES-devel pkgconf-pkg-config

  # Arch:
  sudo pacman -S clang libx11 mesa pkgconf

Then run from any X session. On Wayland, the binary will run via
XWayland (the X11 compatibility layer). A native Wayland backend is
pending.
EOF
}

case "$TARGET" in
    run|'')   target_run ;;
    build)    target_build ;;
    clean)    target_clean ;;
    deps)     target_deps ;;
    *)        err "Unknown target: $TARGET"; echo "Valid: run (default), build, clean, deps"; exit 2 ;;
esac
