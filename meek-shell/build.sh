#!/usr/bin/env bash
# =============================================================================
# build.sh - Linux-native build for meek-shell.
# =============================================================================
#
# Direct clang invocation, no CMake. Mirrors
# meek-ui/demo-linux-wayland/build.sh closely: same wayland-scanner
# step, same EGL + GLES3 linkage, same meek-ui source list. Adds
# meek-shell-specific asset staging for views/*.ui + views/*.style.
#
# PLATFORM SELECTOR
# -----------------------------------------------------------------------------
# The shell ships in two output modes, selectable at build time via
# the PLATFORM environment variable:
#
#   PLATFORM=wayland_client  (default) -- meek-shell is a Wayland
#                                         client. Connects to whatever
#                                         compositor is running (sway,
#                                         meek-compositor, weston,
#                                         etc.). Output binary:
#                                         $SCRIPT_DIR/meek_shell
#
#   PLATFORM=drm             -- meek-shell bypasses Wayland entirely
#                               and owns /dev/dri/card0. Requires the
#                               host compositor to be stopped. Uses
#                               meek-ui's platform_linux_drm.c (same
#                               one the demo-linux-drm target uses).
#                               Output binary:
#                                         $SCRIPT_DIR/meek_shell_drm
#
# The two modes produce separate binaries because they pull in
# disjoint dependency sets (wayland-client vs libdrm + libgbm). Both
# can coexist side-by-side in the tree. Shell logic + UI assets +
# meek-ui sources are identical between them; only the bottom-layer
# platform file swaps.
#
# -----------------------------------------------------------------------------
# USAGE
# -----------------------------------------------------------------------------
#
#   ./build.sh                    # DEFAULT: build + run (wayland_client)
#   ./build.sh build              # build only, no run
#   ./build.sh run                # run only
#   ./build.sh clean              # wipe build outputs
#   ./build.sh deps               # echo install lines per distro
#
#   PLATFORM=drm ./build.sh build # build the DRM variant
#   PLATFORM=drm ./build.sh run   # refuses (DRM mode needs sway stopped;
#                                 # use scripts/with-drm.sh instead)
#
# -----------------------------------------------------------------------------
# DEPENDENCIES
# -----------------------------------------------------------------------------
#
# postmarketOS / Alpine (wayland_client):
#   sudo apk add clang pkgconf wayland-dev wayland-protocols mesa-dev
# postmarketOS / Alpine (drm, additional):
#   sudo apk add libdrm-dev mesa-gbm
# Ubuntu / Debian (wayland_client):
#   sudo apt-get install clang libwayland-dev libegl-dev libgles-dev \
#                        wayland-protocols pkg-config
# Ubuntu / Debian (drm, additional):
#   sudo apt-get install libdrm-dev libgbm-dev
#

set -euo pipefail

APPNAME=${APPNAME:-meek_shell}
TARGET=${1:-run}

#
# PLATFORM: picks the meek-ui platform backend this build links
# against. Defaults to wayland_client (ordinary shell-under-
# compositor behavior). Set PLATFORM=drm to produce the direct-KMS
# variant.
#
PLATFORM=${PLATFORM:-wayland_client}
case "$PLATFORM" in
    wayland_client|drm) ;;
    *) echo "[build] invalid PLATFORM='$PLATFORM' (valid: wayland_client, drm)" >&2; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MEEK_UI_SRC="$(cd "$SCRIPT_DIR/../meek-ui/gui/src" 2>/dev/null && pwd || true)"

BUILD_DIR="$SCRIPT_DIR/build"
GEN_DIR="$BUILD_DIR/protocols"

#
# Binary name differs per platform so the two variants can coexist.
# APPNAME still controls the C-visible name inside the binary (for
# log prefixes, socket paths, etc.); only the file name on disk
# gets the suffix.
#
case "$PLATFORM" in
    wayland_client) BIN_OUT="$SCRIPT_DIR/$APPNAME" ;;
    drm)            BIN_OUT="$SCRIPT_DIR/${APPNAME}_drm" ;;
esac

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

apt_for() {
    case "$1" in
        pkg-config)         echo "pkg-config" ;;
        wayland-client)     echo "libwayland-dev" ;;
        wayland-egl)        echo "libwayland-dev" ;;
        wayland-protocols)  echo "wayland-protocols" ;;
        egl)                echo "libegl-dev" ;;
        glesv2)             echo "libgles-dev" ;;
        libdrm)             echo "libdrm-dev" ;;
        gbm)                echo "libgbm-dev" ;;
        *)                  echo "$1" ;;
    esac
}

apk_for() {
    case "$1" in
        pkg-config)         echo "pkgconf" ;;
        wayland-client)     echo "wayland-dev" ;;
        wayland-egl)        echo "wayland-dev" ;;
        wayland-protocols)  echo "wayland-protocols" ;;
        egl)                echo "mesa-dev" ;;
        glesv2)             echo "mesa-dev" ;;
        libdrm)             echo "libdrm-dev" ;;
        gbm)                echo "mesa-gbm" ;;
        *)                  echo "$1" ;;
    esac
}

#
# Required pkg-config packages differ per PLATFORM. Both modes need
# EGL + GLES; wayland_client adds the Wayland client + scanner
# toolchain; drm adds libdrm + gbm instead.
#
case "$PLATFORM" in
    wayland_client)
        #
        # xkbcommon: meek-ui's wayland-client backend uses it to
        # decode wl_keyboard.keymap + translate key events to
        # unicode + modifier state. Required for any client that
        # wants real keyboard input (demo-settings' text field,
        # shell shortcuts, etc.).
        #
        REQUIRED_PKGS=(wayland-client wayland-egl wayland-protocols egl glesv2 xkbcommon)
        ;;
    drm)
        REQUIRED_PKGS=(egl glesv2 libdrm gbm xkbcommon)
        ;;
esac

missing_apt=()

if ! command -v pkg-config >/dev/null; then
    missing_apt+=("$(apt_for pkg-config)")
else
    for pkg in "${REQUIRED_PKGS[@]}"; do
        if ! pkg-config --exists "$pkg"; then
            missing_apt+=("$(apt_for "$pkg")")
        fi
    done
fi

if [[ ${#missing_apt[@]} -gt 0 ]]; then
    err "missing build dependencies for PLATFORM=$PLATFORM: ${missing_apt[*]}"
    err "install all at once with:"
    err "  sudo apt-get install ${missing_apt[*]}"
    err "(for Fedora / Arch / pmOS equivalents see: ./build.sh deps)"
    exit 1
fi

PKG_CFLAGS="$(pkg-config --cflags "${REQUIRED_PKGS[@]}")"
PKG_LIBS="$(pkg-config --libs   "${REQUIRED_PKGS[@]}")"

#
# The Wayland-scanner toolchain is only relevant under
# PLATFORM=wayland_client (the DRM variant doesn't speak Wayland).
# Probe + resolve it only when needed.
#
WAYLAND_SCANNER_BIN=""
WL_PROTOCOLS_DATADIR=""
if [[ "$PLATFORM" == "wayland_client" ]]; then
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
fi

# -----------------------------------------------------------------------------
# Meek-ui checkout check.
# -----------------------------------------------------------------------------

if [[ -z "$MEEK_UI_SRC" || ! -f "$MEEK_UI_SRC/types.h" ]]; then
    err "expected meek-ui at: $SCRIPT_DIR/../meek-ui/gui/src"
    err "  layout should be:"
    err "    <parent>/meek-ui/gui/src/types.h"
    err "    <parent>/meek-shell/build.sh"
    fatal "meek-ui checkout missing"
fi

# -----------------------------------------------------------------------------
# Scanner step (xdg-shell; meek_shell_v1 added at D3).
# -----------------------------------------------------------------------------

XDG_XML="$WL_PROTOCOLS_DATADIR/stable/xdg-shell/xdg-shell.xml"
XDG_HEADER="$GEN_DIR/xdg-shell-client-protocol.h"
XDG_SRC="$GEN_DIR/xdg-shell-protocol.c"

#
# meek_shell_v1 lives in the sibling meek-compositor repo. Both
# sides read the same XML so client and server bindings stay in
# lockstep.
#
MSV1_XML="$SCRIPT_DIR/../meek-compositor/protocols/meek-shell-v1.xml"
MSV1_HEADER="$GEN_DIR/meek-shell-v1-client-protocol.h"
MSV1_SRC="$GEN_DIR/meek-shell-v1-protocol.c"

#
# idle-inhibit-unstable-v1: lets us tell the compositor "don't blank
# the screen while my surface is visible". Needed because sxmo (and
# most mobile compositors) auto-blank on ~10s of input idleness,
# which makes any always-on shell UI useless without inhibit.
# Consumed by meek-ui's platform_linux_wayland_client.c.
#
IDLE_INHIBIT_XML="$WL_PROTOCOLS_DATADIR/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml"
IDLE_INHIBIT_HEADER="$GEN_DIR/idle-inhibit-unstable-v1-client-protocol.h"
IDLE_INHIBIT_SRC="$GEN_DIR/idle-inhibit-unstable-v1-protocol.c"

target_scanner() {
    #
    # Only runs for PLATFORM=wayland_client. The DRM variant has no
    # Wayland wire to speak, so the generated protocol bindings are
    # irrelevant (and their generator tool may not even be present
    # on a minimal DRM-only build host).
    #
    if [[ "$PLATFORM" != "wayland_client" ]]; then
        return
    fi

    [[ -f "$XDG_XML" ]]           || fatal "xdg-shell.xml not found at $XDG_XML"
    [[ -f "$MSV1_XML" ]]          || fatal "meek-shell-v1.xml not found at $MSV1_XML (expected sibling meek-compositor checkout)"
    [[ -f "$IDLE_INHIBIT_XML" ]]  || fatal "idle-inhibit-unstable-v1.xml not found at $IDLE_INHIBIT_XML"
    mkdir -p "$GEN_DIR"

    if [[ ! -f "$XDG_HEADER" || "$XDG_XML" -nt "$XDG_HEADER" ]]; then
        info "wayland-scanner client-header xdg-shell"
        "$WAYLAND_SCANNER_BIN" client-header "$XDG_XML" "$XDG_HEADER"
    fi
    if [[ ! -f "$XDG_SRC" || "$XDG_XML" -nt "$XDG_SRC" ]]; then
        info "wayland-scanner private-code xdg-shell"
        "$WAYLAND_SCANNER_BIN" private-code "$XDG_XML" "$XDG_SRC"
    fi

    if [[ ! -f "$MSV1_HEADER" || "$MSV1_XML" -nt "$MSV1_HEADER" ]]; then
        info "wayland-scanner client-header meek-shell-v1"
        "$WAYLAND_SCANNER_BIN" client-header "$MSV1_XML" "$MSV1_HEADER"
    fi
    if [[ ! -f "$MSV1_SRC" || "$MSV1_XML" -nt "$MSV1_SRC" ]]; then
        info "wayland-scanner private-code meek-shell-v1"
        "$WAYLAND_SCANNER_BIN" private-code "$MSV1_XML" "$MSV1_SRC"
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
# Sources. Keep in sync with CMakeLists.txt's _MEEK_UI_SOURCES.
# -----------------------------------------------------------------------------

#
# Sources common to both PLATFORM variants (meek-ui library + shell
# main). Kept stable across builds so the only delta between
# wayland_client and drm is the platform backend + the Wayland-
# specific extras.
#
SOURCES=(
    "$MEEK_UI_SRC/scene.c"
    "$MEEK_UI_SRC/scene_style.c"
    "$MEEK_UI_SRC/scene_layout.c"
    "$MEEK_UI_SRC/scene_render.c"
    "$MEEK_UI_SRC/scene_input.c"
    "$MEEK_UI_SRC/platforms/linux/fs_linux.c"
    "$MEEK_UI_SRC/font.c"
    "$MEEK_UI_SRC/scroll.c"
    "$MEEK_UI_SRC/animator.c"
    "$MEEK_UI_SRC/parser_xml.c"
    "$MEEK_UI_SRC/parser_style.c"
    "$MEEK_UI_SRC/widget_registry.c"
    "$MEEK_UI_SRC/widgets/widget_window.c"
    "$MEEK_UI_SRC/widgets/widget_column.c"
    "$MEEK_UI_SRC/widgets/widget_row.c"
    "$MEEK_UI_SRC/widgets/widget_button.c"
    "$MEEK_UI_SRC/widgets/widget_slider.c"
    "$MEEK_UI_SRC/widgets/widget_div.c"
    "$MEEK_UI_SRC/widgets/widget_text.c"
    "$MEEK_UI_SRC/widgets/widget_input.c"
    "$MEEK_UI_SRC/widgets/widget_checkbox.c"
    "$MEEK_UI_SRC/widgets/widget_radio.c"
    "$MEEK_UI_SRC/widgets/widget_select.c"
    "$MEEK_UI_SRC/widgets/widget_option.c"
    "$MEEK_UI_SRC/widgets/widget_image.c"
    "$MEEK_UI_SRC/widgets/widget_collection.c"
    "$MEEK_UI_SRC/widgets/widget_colorpicker.c"
    "$MEEK_UI_SRC/widgets/widget_popup.c"
    "$MEEK_UI_SRC/widgets/widget_textarea.c"
    "$MEEK_UI_SRC/widgets/widget_canvas.c"
    "$MEEK_UI_SRC/widgets/widget_process_window.c"
    "$MEEK_UI_SRC/widgets/widget_keyboard.c"
    "$MEEK_UI_SRC/third_party/log.c"
    "$MEEK_UI_SRC/renderers/gles3_renderer.c"
    "$MEEK_UI_SRC/clib/memory_manager.c"
    "$MEEK_UI_SRC/clib/stdlib.c"
    "$MEEK_UI_SRC/hot_reload.c"
    "$SCRIPT_DIR/src/main.c"
)

#
# PLATFORM-specific additions:
#   wayland_client -> platform_linux_wayland_client.c + meek_shell_v1
#                     client glue + three generated scanner outputs
#                     for xdg-shell / meek-shell-v1 / idle-inhibit.
#   drm            -> platform_linux_drm.c, nothing else. No
#                     compositor-side protocol to speak; direct KMS
#                     is self-contained.
#
case "$PLATFORM" in
    wayland_client)
        SOURCES+=(
            "$MEEK_UI_SRC/platforms/linux/platform_linux_wayland_client.c"
            "$SCRIPT_DIR/src/meek_shell_v1_client.c"
            "$SCRIPT_DIR/src/toplevel_registry.c"
            "$SCRIPT_DIR/src/gesture_recognizer.c"
            "$SCRIPT_DIR/src/settings.c"
            "$SCRIPT_DIR/src/app_registry.c"
            "$SCRIPT_DIR/src/icon_resolver.c"
            "$SCRIPT_DIR/src/icon_color_sampler.c"
            "$SCRIPT_DIR/src/card_drag.c"
            "$XDG_SRC"
            "$MSV1_SRC"
            "$IDLE_INHIBIT_SRC"
        )
        ;;
    drm)
        SOURCES+=(
            "$MEEK_UI_SRC/platforms/linux/platform_linux_drm.c"
        )
        ;;
esac

gen_defines() {
    mkdir -p "$BUILD_DIR"
    local defs="$BUILD_DIR/shell_build_defines.h"
    #
    # Platform-identifying macro so shell code can branch on what
    # kind of binary it is (e.g. "don't try to talk meek_shell_v1
    # under PLATFORM=drm"). PLATFORM_NAME is an uppercase token the
    # preprocessor can compare; PLATFORM_STR is a convenience
    # string for log messages.
    #
    local platform_token
    case "$PLATFORM" in
        wayland_client) platform_token="WAYLAND_CLIENT" ;;
        drm)            platform_token="DRM" ;;
    esac
    cat > "$defs" <<EOF
/* Auto-generated by build.sh -- do not edit. */
#ifndef MEEK_SHELL_BUILD_DEFINES_H
#define MEEK_SHELL_BUILD_DEFINES_H
#define APPNAME "$APPNAME"
#define SHELL_ASSET_DIR "$SCRIPT_DIR/assets"
#define GUI_FONTS_SOURCE_DIR "$SCRIPT_DIR/assets/fonts"
#define MEEK_SHELL_PLATFORM_$platform_token 1
#define MEEK_SHELL_PLATFORM_STR "$PLATFORM"
#endif
EOF
}

stage_assets() {
    local dst="$SCRIPT_DIR/assets"
    mkdir -p "$dst" "$dst/fonts"

    if [ -d "$SCRIPT_DIR/views" ]; then
        for f in "$SCRIPT_DIR/views"/*.ui "$SCRIPT_DIR/views"/*.style; do
            [ -f "$f" ] && cp -u "$f" "$dst/"
        done
    fi
    if [ -d "$MEEK_UI_SRC/fonts" ]; then
        for f in "$MEEK_UI_SRC/fonts"/*.ttf; do
            [ -f "$f" ] && cp -u "$f" "$dst/fonts/"
        done
    fi
    ok "staged assets -> $dst"
}

target_build() {
    target_scanner
    gen_defines
    stage_assets

    local defs="$BUILD_DIR/shell_build_defines.h"

    local cflags=(
        -O2 -g
        -fvisibility=hidden
        -Wall -Wno-unused-function -Wno-unused-parameter
        -DGUI_TRACK_ALLOCATIONS
        -include "$defs"
        -I"$SCRIPT_DIR/src"
        -I"$MEEK_UI_SRC"
    )
    #
    # Only add the scanner-generated header dir to the include path
    # when we actually produced scanner outputs (wayland_client
    # mode). The directory won't exist under PLATFORM=drm and
    # nothing in the source set will try to include from it.
    #
    if [[ "$PLATFORM" == "wayland_client" ]]; then
        cflags+=(-I"$GEN_DIR")
    fi
    # shellcheck disable=SC2206
    cflags+=($PKG_CFLAGS)

    #
    # -rdynamic (== -Wl,--export-dynamic) adds all default-
    # visibility symbols to the dynamic symbol table so
    # dlsym(RTLD_DEFAULT, "on_foo_tap") can resolve UI_HANDLERs
    # defined in main.c. Without this, scene's symbol resolver
    # comes back empty for every on_click handler the .ui mentions.
    # Required on Linux (and harmless on systems where it's a
    # no-op).
    #
    local ldflags=( -rdynamic -lm -ldl -lpthread )
    # shellcheck disable=SC2206
    ldflags+=($PKG_LIBS)

    info "compiling $(basename "$BIN_OUT") (PLATFORM=$PLATFORM)"
    "$CC" "${cflags[@]}" -o "$BIN_OUT" "${SOURCES[@]}" "${ldflags[@]}"
    ok "built $BIN_OUT"
    local size
    size=$(stat -c%s "$BIN_OUT" 2>/dev/null || stat -f%z "$BIN_OUT" 2>/dev/null || echo '?')
    info "size: $size bytes"
}

target_run() {
    target_build
    if [[ "$PLATFORM" == "drm" ]]; then
        #
        # DRM mode needs to become DRM master, which means no other
        # compositor may be running on the same device. Blindly
        # launching here from a live sxmo/sway session would fail
        # with EBUSY and potentially leave the session in a
        # confused state. Point the user at the handoff helper
        # instead so stop/run/restart is orchestrated safely.
        #
        err "PLATFORM=drm build completed but cannot be auto-run from build.sh."
        err "use the handoff helper to stop the host compositor first:"
        err "  $SCRIPT_DIR/scripts/with-drm.sh"
        err "(or manually: stop sway, then run $BIN_OUT with DRM master-capable perms)"
        exit 0
    fi
    if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
        err "XDG_RUNTIME_DIR not set. Wayland client needs it to find the socket."
        err "  try: export XDG_RUNTIME_DIR=/run/user/\$(id -u)"
        exit 1
    fi
    if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
        info "WAYLAND_DISPLAY unset; libwayland-client defaults to 'wayland-0'."
    fi
    info "running $BIN_OUT"
    "$BIN_OUT"
}

target_clean() {
    #
    # Clean wipes BOTH platform variants' binaries regardless of
    # which PLATFORM the current invocation targeted -- otherwise a
    # ./build.sh clean under one platform would leave the other
    # platform's stale binary on disk.
    #
    for p in \
        "$BUILD_DIR" \
        "$SCRIPT_DIR/$APPNAME" \
        "$SCRIPT_DIR/${APPNAME}_drm" \
        "$SCRIPT_DIR/assets"; do
        if [[ -e "$p" ]]; then
            info "removing $p"
            rm -rf "$p"
        fi
    done
    ok 'clean done'
}

target_deps() {
    cat <<'EOF'
Dependencies split by PLATFORM. wayland_client covers the default
client build; drm covers the direct-KMS variant. Install whichever
platform(s) you plan to build.

  === PLATFORM=wayland_client (default) ===

  # postmarketOS / Alpine:
  sudo apk add clang pkgconf wayland-dev wayland-protocols mesa-dev

  # Ubuntu / Debian:
  sudo apt-get install clang libwayland-dev libegl-dev libgles-dev \
                       wayland-protocols pkg-config

  # Fedora:
  sudo dnf install clang wayland-devel mesa-libEGL-devel \
                   mesa-libGLES-devel wayland-protocols-devel \
                   pkgconf-pkg-config

  # Arch:
  sudo pacman -S clang wayland wayland-protocols mesa pkgconf

  === PLATFORM=drm (in addition to the platform-egl/gles base) ===

  # postmarketOS / Alpine:
  sudo apk add libdrm-dev mesa-gbm

  # Ubuntu / Debian:
  sudo apt-get install libdrm-dev libgbm-dev

  # Fedora:
  sudo dnf install libdrm-devel mesa-libgbm-devel

  # Arch:
  sudo pacman -S libdrm   # (gbm ships inside mesa on Arch)
EOF
}

case "$TARGET" in
    run|'')   target_run ;;
    build)    target_build ;;
    clean)    target_clean ;;
    deps)     target_deps ;;
    *)        err "Unknown target: $TARGET"; echo "Valid: run (default), build, clean, deps"; exit 2 ;;
esac
