#!/usr/bin/env bash
# =============================================================================
# build.sh - Linux Wayland-client build for this repo.
# =============================================================================
#
# Direct clang invocation, no CMake. Mirrors demo-linux-x11/build.sh
# but links wayland-client + wayland-egl instead of libX11. Same
# source list modulo platform backend + one extra scanner pass for
# xdg-shell bindings.
#
# OUTPUT: $SCRIPT_DIR/guidemo   (ELF, dynamically linked)
#
# SCANNER NOTE:
#   xdg-shell is a standardized Wayland extension for application
#   windows (title, min/max, fullscreen). Its C bindings aren't in
#   libwayland-client; we run wayland-scanner on xdg-shell.xml from
#   the wayland-protocols package to produce .h + .c locally, then
#   compile them alongside our own sources.
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
#   sudo apt-get install clang libwayland-dev libegl-dev libgles-dev \
#                        wayland-protocols pkg-config
# Fedora:
#   sudo dnf install clang wayland-devel mesa-libEGL-devel \
#                    mesa-libGLES-devel wayland-protocols-devel \
#                    pkgconf-pkg-config
# Arch:
#   sudo pacman -S clang wayland wayland-protocols mesa pkgconf
# postmarketOS / Alpine:
#   sudo apk add clang pkgconf wayland-dev wayland-protocols mesa-dev
#
# Run from a Wayland session (sway / hyprland / gnome / kde / weston).
# Pure X11 sessions won't work -- the compositor has to support the
# Wayland protocol. For dev on an X machine, run `weston --backend=x11`
# in a terminal first; it creates a nested Wayland compositor you
# can then connect to with WAYLAND_DISPLAY=wayland-1 ./guidemo.
#

set -euo pipefail

APPNAME=${APPNAME:-demo_settings}
TARGET=${1:-run}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# demo-settings lives at meek-shell/demo-settings; meek-ui is a
# sibling of meek-shell, so go up two levels then into meek-ui.
ROOT="$(cd "$SCRIPT_DIR/../../meek-ui" && pwd)"

BUILD_DIR="$SCRIPT_DIR/build"
GEN_DIR="$BUILD_DIR/protocols"
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
# pkg-config: wayland-client + wayland-egl + egl + glesv2 + wayland-protocols.
# -----------------------------------------------------------------------------

apt_for() {
    case "$1" in
        pkg-config)         echo "pkg-config" ;;
        wayland-client)     echo "libwayland-dev" ;;
        wayland-egl)        echo "libwayland-dev" ;;
        wayland-protocols)  echo "wayland-protocols" ;;
        egl)                echo "libegl-dev" ;;
        glesv2)             echo "libgles-dev" ;;
        *)                  echo "$1" ;;
    esac
}

missing_apt=()

if ! command -v pkg-config >/dev/null; then
    missing_apt+=("$(apt_for pkg-config)")
else
    #
    # xkbcommon: meek-ui's platform_linux_wayland_client.c parses
    # the compositor's wl_keyboard.keymap via xkbcommon so input
    # events translate to codepoints. Any meek-ui client linking
    # that platform backend needs the library.
    #
    for pkg in wayland-client wayland-egl wayland-protocols egl glesv2 xkbcommon; do
        if ! pkg-config --exists "$pkg"; then
            missing_apt+=("$(apt_for "$pkg")")
        fi
    done
fi

if [[ ${#missing_apt[@]} -gt 0 ]]; then
    err "missing build dependencies: ${missing_apt[*]}"
    err "install all at once with:"
    err "  sudo apt-get install ${missing_apt[*]}"
    err "(for Fedora / Arch / pmOS equivalents see: ./build.sh deps)"
    exit 1
fi

WL_CFLAGS="$(pkg-config --cflags wayland-client wayland-egl egl glesv2 xkbcommon)"
WL_LIBS="$(pkg-config --libs   wayland-client wayland-egl egl glesv2 xkbcommon)"

WAYLAND_SCANNER_BIN="$(pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || true)"
if [[ -z "$WAYLAND_SCANNER_BIN" ]]; then
    WAYLAND_SCANNER_BIN="$(command -v wayland-scanner 2>/dev/null || true)"
fi
if [[ -z "$WAYLAND_SCANNER_BIN" ]]; then
    fatal "wayland-scanner not found. Install libwayland-dev / wayland-dev."
fi

WL_PROTOCOLS_DATADIR="$(pkg-config --variable=pkgdatadir wayland-protocols)"
if [[ -z "$WL_PROTOCOLS_DATADIR" ]]; then
    fatal "could not resolve wayland-protocols pkgdatadir via pkg-config"
fi

# -----------------------------------------------------------------------------
# Scanner step. Produces:
#   $GEN_DIR/xdg-shell-client-protocol.h  (from client-header)
#   $GEN_DIR/xdg-shell-protocol.c         (from private-code)
# -----------------------------------------------------------------------------

XDG_XML="$WL_PROTOCOLS_DATADIR/stable/xdg-shell/xdg-shell.xml"
XDG_HEADER="$GEN_DIR/xdg-shell-client-protocol.h"
XDG_SRC="$GEN_DIR/xdg-shell-protocol.c"

# idle-inhibit is referenced by meek-ui's platform_linux_wayland_client.c
# (to keep the screen on while the client has focus). Client-side
# bindings live alongside xdg-shell in build/protocols/.
IDLE_INHIBIT_XML="$WL_PROTOCOLS_DATADIR/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml"
IDLE_INHIBIT_HEADER="$GEN_DIR/idle-inhibit-unstable-v1-client-protocol.h"
IDLE_INHIBIT_SRC="$GEN_DIR/idle-inhibit-unstable-v1-protocol.c"

target_scanner() {
    [[ -f "$XDG_XML" ]] || fatal "xdg-shell.xml not found at $XDG_XML"
    [[ -f "$IDLE_INHIBIT_XML" ]] || fatal "idle-inhibit-unstable-v1.xml not found at $IDLE_INHIBIT_XML"
    mkdir -p "$GEN_DIR"

    if [[ ! -f "$XDG_HEADER" || "$XDG_XML" -nt "$XDG_HEADER" ]]; then
        info "wayland-scanner client-header xdg-shell"
        "$WAYLAND_SCANNER_BIN" client-header "$XDG_XML" "$XDG_HEADER"
    fi
    if [[ ! -f "$XDG_SRC" || "$XDG_XML" -nt "$XDG_SRC" ]]; then
        info "wayland-scanner private-code xdg-shell"
        "$WAYLAND_SCANNER_BIN" private-code "$XDG_XML" "$XDG_SRC"
    fi
    if [[ ! -f "$IDLE_INHIBIT_HEADER" || "$IDLE_INHIBIT_XML" -nt "$IDLE_INHIBIT_HEADER" ]]; then
        info "wayland-scanner client-header idle-inhibit-unstable-v1"
        "$WAYLAND_SCANNER_BIN" client-header "$IDLE_INHIBIT_XML" "$IDLE_INHIBIT_HEADER"
    fi
    if [[ ! -f "$IDLE_INHIBIT_SRC" || "$IDLE_INHIBIT_XML" -nt "$IDLE_INHIBIT_SRC" ]]; then
        info "wayland-scanner private-code idle-inhibit-unstable-v1"
        "$WAYLAND_SCANNER_BIN" private-code "$IDLE_INHIBIT_XML" "$IDLE_INHIBIT_SRC"
    fi
}

# -----------------------------------------------------------------------------
# Sources. Same as demo-linux-x11 minus platform_linux_x11.c, plus
# platform_linux_wayland_client.c + the scanner-generated .c.
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
    "$ROOT/gui/src/widgets/widget_process_window.c"
    "$ROOT/gui/src/widgets/widget_keyboard.c"
    "$ROOT/gui/src/third_party/log.c"
    "$ROOT/gui/src/renderers/gles3_renderer.c"
    "$ROOT/gui/src/platforms/linux/platform_linux_wayland_client.c"
    "$ROOT/gui/src/clib/memory_manager.c"
    "$ROOT/gui/src/clib/stdlib.c"
    "$ROOT/gui/src/hot_reload.c"
    "$SCRIPT_DIR/main.c"
    "$XDG_SRC"
    "$IDLE_INHIBIT_SRC"
)

gen_defines() {
    mkdir -p "$BUILD_DIR"
    local defs="$BUILD_DIR/linux_wayland_build_defines.h"
    cat > "$defs" <<EOF
/* Auto-generated by build.sh -- do not edit. */
#ifndef LINUX_WAYLAND_BUILD_DEFINES_H
#define LINUX_WAYLAND_BUILD_DEFINES_H
#define APPNAME "$APPNAME"
#define DEMO_SOURCE_DIR "$SCRIPT_DIR/assets"
#define GUI_FONTS_SOURCE_DIR "$SCRIPT_DIR/assets/fonts"
#endif
EOF
}

stage_assets() {
    #
    # demo-settings ships its OWN main.ui + main.style (unlike
    # demo-linux-wayland which reuses demo-windows assets). We only
    # need to stage fonts from meek-ui's shipped font directory --
    # UI + style are already checked into assets/ and loaded
    # directly from there.
    #
    local dst="$SCRIPT_DIR/assets"
    mkdir -p "$dst" "$dst/fonts"

    if [ -d "$ROOT/gui/src/fonts" ]; then
        for f in "$ROOT/gui/src/fonts"/*.ttf; do
            if [ -f "$f" ]; then cp -u "$f" "$dst/fonts/"; fi
        done
    fi

    ok "staged assets -> $dst (fonts only; ui + style are checked-in)"
}

target_build() {
    target_scanner
    gen_defines
    stage_assets

    local defs="$BUILD_DIR/linux_wayland_build_defines.h"

    local cflags=(
        -O2 -g
        -fvisibility=hidden
        -Wall -Wno-unused-function
        -DGUI_TRACK_ALLOCATIONS
        -include "$defs"
        -I"$ROOT/gui/src"
        -I"$GEN_DIR"
    )
    # shellcheck disable=SC2206
    cflags+=($WL_CFLAGS)

    local ldflags=( -rdynamic -lm -ldl -lpthread )
    # shellcheck disable=SC2206
    ldflags+=($WL_LIBS)

    info "compiling $APPNAME"
    "$CC" "${cflags[@]}" -o "$BIN_OUT" "${SOURCES[@]}" "${ldflags[@]}"
    ok "built $BIN_OUT"
    local size
    size=$(stat -c%s "$BIN_OUT" 2>/dev/null || stat -f%z "$BIN_OUT" 2>/dev/null || echo '?')
    info "size: $size bytes"
}

target_run() {
    target_build
    if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
        err "XDG_RUNTIME_DIR not set. Wayland client needs it to find the socket."
        err "  try: export XDG_RUNTIME_DIR=/run/user/\$(id -u)"
        exit 1
    fi
    if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
        info "WAYLAND_DISPLAY unset; libwayland-client will default to 'wayland-0'."
    fi
    info "running $BIN_OUT (WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0})"
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
  sudo apt-get install clang libwayland-dev libegl-dev libgles-dev \
                       wayland-protocols pkg-config

  # Fedora:
  sudo dnf install clang wayland-devel mesa-libEGL-devel \
                   mesa-libGLES-devel wayland-protocols-devel \
                   pkgconf-pkg-config

  # Arch:
  sudo pacman -S clang wayland wayland-protocols mesa pkgconf

  # postmarketOS / Alpine:
  sudo apk add clang pkgconf wayland-dev wayland-protocols mesa-dev

Then run from any Wayland session (sway, hyprland, gnome, kde,
weston, labwc, niri). X11-only machines can launch `weston
--backend=x11` first to get a nested Wayland compositor.
EOF
}

case "$TARGET" in
    run|'')   target_run ;;
    build)    target_build ;;
    clean)    target_clean ;;
    deps)     target_deps ;;
    *)        err "Unknown target: $TARGET"; echo "Valid: run (default), build, clean, deps"; exit 2 ;;
esac
