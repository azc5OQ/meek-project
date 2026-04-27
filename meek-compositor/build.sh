#!/usr/bin/env bash
# =============================================================================
# build.sh - Linux-native build for meek-compositor.
# =============================================================================
#
# Direct clang invocation, no CMake. Mirrors the structure of
# meek-ui/demo-linux-drm/build.sh so familiar muscle memory applies:
# same targets (build/run/clean/deps), same pkg-config-before-compile
# discipline, same one-shot missing-deps message.
#
# OUTPUT: $SCRIPT_DIR/meek_compositor   (ELF, dynamically linked)
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
#   sudo apt-get install clang pkg-config libwayland-dev
# Fedora:
#   sudo dnf install clang pkgconf-pkg-config wayland-devel
# Arch:
#   sudo pacman -S clang pkgconf wayland
#
# libwayland-dev provides libwayland-server + wayland-scanner + the
# protocol .xml files. We don't invoke wayland-scanner yet (no
# protocols registered in this pass) but it will be needed for
# xdg-shell / linux-dmabuf / wl_seat in the next pass.
#
# Running:
#   The compositor opens a Wayland socket at $XDG_RUNTIME_DIR/
#   wayland-N (N picked automatically). No clients will bind to
#   anything meaningful yet -- this pass only proves the socket +
#   event loop work.
#
#   Smoke-test with any wayland client on the same box:
#     WAYLAND_DISPLAY=wayland-1 weston-info        # should connect cleanly
#     WAYLAND_DISPLAY=wayland-1 wayland-scanner    # alt
#

set -euo pipefail

APPNAME=${APPNAME:-meek_compositor}
TARGET=${1:-run}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MEEK_UI_SRC="$(cd "$SCRIPT_DIR/../meek-ui/gui/src" 2>/dev/null && pwd || true)"

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
# Compiler pick -- same policy as demo-linux-drm/build.sh.
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
# pkg-config lookups. Collect ALL missing pieces before bailing so
# the user gets one install line.
# -----------------------------------------------------------------------------

apt_for() {
    case "$1" in
        pkg-config)          echo "pkg-config" ;;
        wayland-server)      echo "libwayland-dev" ;;
        wayland-protocols)   echo "wayland-protocols" ;;
        egl)                 echo "libegl-dev" ;;
        glesv2)              echo "libgles-dev" ;;
        gbm)                 echo "libgbm-dev" ;;
        libdrm)              echo "libdrm-dev" ;;
        x11)                 echo "libx11-dev" ;;
        xkbcommon)           echo "libxkbcommon-dev" ;;
        *)                   echo "$1" ;;
    esac
}

#
# postmarketOS / Alpine: packages come via apk, not apt. Mapping
# is different so keep both tables. `pkg-config` is part of
# `pkgconf` on Alpine; libwayland-server dev headers are split out
# into `wayland-dev`. Mesa's EGL / GLES / GBM live in separate
# -dev packages on Alpine too.
#
apk_for() {
    case "$1" in
        pkg-config)          echo "pkgconf" ;;
        wayland-server)      echo "wayland-dev" ;;
        wayland-protocols)   echo "wayland-protocols" ;;
        egl)                 echo "mesa-dev" ;;
        glesv2)              echo "mesa-dev" ;;
        gbm)                 echo "mesa-dev" ;;
        libdrm)              echo "libdrm-dev" ;;
        x11)                 echo "libx11-dev" ;;
        xkbcommon)           echo "libxkbcommon-dev" ;;
        *)                   echo "$1" ;;
    esac
}

missing_apt=()

if ! command -v pkg-config >/dev/null; then
    missing_apt+=("$(apt_for pkg-config)")
else
    #
    # wayland-server is the core runtime; wayland-protocols is the
    # separate package that ships the XML files the scanner consumes
    # (xdg-shell.xml, linux-dmabuf-v1.xml, ...). On Debian they're
    # split between libwayland-dev and wayland-protocols; on Alpine
    # they're wayland-dev and wayland-protocols.
    #
    for pkg in wayland-server wayland-protocols egl glesv2 gbm libdrm x11 libinput libudev xkbcommon; do
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

WL_CFLAGS="$(pkg-config --cflags wayland-server)"
WL_LIBS="$(pkg-config --libs   wayland-server)"

#
# EGL / GLES / GBM -- pulled in at pass A3 for client-buffer import.
# `glesv2` is the pkg-config name used by Mesa for GLES2+; it ships
# the GLES3 headers too (the header is #include <GLES3/gl3.h>).
#
EGL_CFLAGS="$(pkg-config --cflags egl glesv2 gbm libdrm x11)"
EGL_LIBS="$(pkg-config --libs   egl glesv2 gbm libdrm x11)"

#
# libinput + libudev -- pass C7 for /dev/input/* touch capture. On
# Alpine these are pulled in by libinput-dev (libudev comes along
# with systemd/eudev).
#
INPUT_CFLAGS="$(pkg-config --cflags libinput libudev)"
INPUT_LIBS="$(pkg-config --libs   libinput libudev)"

#
# xkbcommon -- pass C7/E2 for wl_keyboard support. We build a
# server-side xkb keymap (US QWERTY) once at startup and ship it
# to every client that binds wl_keyboard. See seat.c for the init
# sequence and memfd-based keymap transfer.
#
XKB_CFLAGS="$(pkg-config --cflags xkbcommon)"
XKB_LIBS="$(pkg-config --libs   xkbcommon)"

#
# wayland-scanner binary. Prefer the pkg-config variable (points at
# the exact scanner for the wayland-server version we're linking
# against); fall back to whatever's on PATH.
#
WAYLAND_SCANNER_BIN="$(pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || true)"
if [[ -z "$WAYLAND_SCANNER_BIN" ]]; then
    WAYLAND_SCANNER_BIN="$(command -v wayland-scanner 2>/dev/null || true)"
fi
if [[ -z "$WAYLAND_SCANNER_BIN" ]]; then
    fatal "wayland-scanner not found. Install wayland-dev / libwayland-dev."
fi

#
# wayland-protocols pkgdatadir: usually /usr/share/wayland-protocols.
#
WL_PROTOCOLS_DATADIR="$(pkg-config --variable=pkgdatadir wayland-protocols)"
if [[ -z "$WL_PROTOCOLS_DATADIR" ]]; then
    fatal "could not resolve wayland-protocols pkgdatadir via pkg-config"
fi

# -----------------------------------------------------------------------------
# Meek-ui types.h check. We don't compile meek-ui sources in this
# pass, but main.c includes "types.h" from meek-ui/gui/src to keep
# our primitive-type vocabulary aligned from day one.
# -----------------------------------------------------------------------------

if [[ -z "$MEEK_UI_SRC" || ! -f "$MEEK_UI_SRC/types.h" ]]; then
    err "expected meek-ui at: $SCRIPT_DIR/../meek-ui/gui/src"
    err "  that dir (or its types.h) was not found."
    err "  layout should be:"
    err "    <parent>/meek-ui/gui/src/types.h"
    err "    <parent>/meek-compositor/build.sh"
    fatal "meek-ui checkout missing"
fi

# -----------------------------------------------------------------------------
# Sources.
#
# Shared meek-ui utility sources pulled into meek-compositor's
# binary. Per project guideline, prefer these over raw libc:
#   * third_party/log.c       -> log_info / log_warn / log_error
#                                instead of fprintf.
#   * clib/memory_manager.c   -> GUI_MALLOC / GUI_FREE instead of
#                                malloc/free; tracked for leaks when
#                                GUI_TRACK_ALLOCATIONS is on.
#   * clib/stdlib.c           -> stdlib__* wrappers.
# Keep this list in sync with CMakeLists.txt's _MEEK_UI_SHARED_SOURCES.
#
# The src/clib/stdlib.c pre-staged under src/ is superseded by the
# canonical meek-ui copy and no longer compiled.
# -----------------------------------------------------------------------------

MEEK_UI_SHARED_SOURCES=(
    "$MEEK_UI_SRC/third_party/log.c"
    "$MEEK_UI_SRC/clib/memory_manager.c"
    "$MEEK_UI_SRC/clib/stdlib.c"
)

#
# Protocol bindings generated by wayland-scanner live under
# build/protocols/. The lists below must match CMakeLists.txt's
# wl_scanner_generate() calls. Keep in sync when new protocols
# are added.
#
GEN_DIR="$BUILD_DIR/protocols"

# Each entry: "<xml path under $WL_PROTOCOLS_DATADIR>:<stem>" or
# (for our custom protocols) "abs:<absolute path>:<stem>" so
# target_scanner can tell "system protocol" from "repo-local
# protocol" apart.
#
# System-protocol entries can list multiple candidate paths separated
# by '|'. target_scanner tries each in order and uses the first that
# exists. This absorbs the wayland-protocols upstream convention where
# protocols migrate from unstable/ -> staging/ -> stable/ as they
# mature. Without multiple candidates the build breaks on every distro
# that ships a different wayland-protocols version.
#
# Example: linux-dmabuf-v1 is in stable/ on wayland-protocols >= ~1.30,
# staging/ on some transitional versions, and unstable/ (with a
# different filename!) on older distros.
#
WL_PROTOCOLS=(
    "stable/xdg-shell/xdg-shell.xml:xdg-shell"
    "stable/linux-dmabuf/linux-dmabuf-v1.xml|staging/linux-dmabuf/linux-dmabuf-v1.xml|unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml:linux-dmabuf-v1"
    "unstable/text-input/text-input-unstable-v3.xml:text-input-unstable-v3"
    "stable/viewporter/viewporter.xml:viewporter"
    "staging/fractional-scale/fractional-scale-v1.xml:fractional-scale-v1"
    "abs:$SCRIPT_DIR/protocols/meek-shell-v1.xml:meek-shell-v1"
)

# Populated by target_build() after scanner runs.
WL_PROTOCOL_SOURCES=()

SOURCES_BASE=(
    "$SCRIPT_DIR/src/main.c"
    "$SCRIPT_DIR/src/globals.c"
    "$SCRIPT_DIR/src/surface.c"
    "$SCRIPT_DIR/src/seat.c"
    "$SCRIPT_DIR/src/xdg_shell.c"
    "$SCRIPT_DIR/src/egl_ctx.c"
    "$SCRIPT_DIR/src/linux_dmabuf.c"
    "$SCRIPT_DIR/src/output_x11.c"
    "$SCRIPT_DIR/src/output_drm.c"
    "$SCRIPT_DIR/src/input.c"
    "$SCRIPT_DIR/src/meek_shell_v1.c"
    "$SCRIPT_DIR/src/text_input_v3.c"
    "$SCRIPT_DIR/src/viewporter.c"
    "$SCRIPT_DIR/src/fractional_scale.c"
    "${MEEK_UI_SHARED_SOURCES[@]}"
)

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------

#
# Run wayland-scanner on the protocols listed in WL_PROTOCOLS.
# Output goes to $GEN_DIR; generated source list accumulates in
# WL_PROTOCOL_SOURCES.
#
target_scanner() {
    mkdir -p "$GEN_DIR"
    WL_PROTOCOL_SOURCES=()

    local entry xml stem hdr src
    for entry in "${WL_PROTOCOLS[@]}"; do
        if [[ "$entry" == abs:* ]]; then
            # abs:<absolute path>:<stem>
            local rest="${entry#abs:}"
            xml="${rest%:*}"
            stem="${rest##*:}"
        else
            # <xml rel path(s) under pkgdatadir>:<stem>  -- path list
            # is '|'-separated; target tries each in order. See
            # WL_PROTOCOLS comment for the migration rationale.
            local xml_rels="${entry%:*}"
            stem="${entry##*:}"
            xml=""
            local cand
            local IFS_BACKUP="$IFS"
            IFS='|'
            for cand in $xml_rels; do
                if [[ -f "$WL_PROTOCOLS_DATADIR/$cand" ]]; then
                    xml="$WL_PROTOCOLS_DATADIR/$cand"
                    break
                fi
            done
            IFS="$IFS_BACKUP"
        fi

        if [[ -z "$xml" || ! -f "$xml" ]]; then
            fatal "wayland protocol XML not found (tried: ${xml_rels:-$xml}) under $WL_PROTOCOLS_DATADIR"
        fi

        hdr="$GEN_DIR/${stem}-protocol.h"
        src="$GEN_DIR/${stem}-protocol.c"

        #
        # Only regenerate when the source XML is newer than the
        # output, so incremental rebuilds don't thrash.
        #
        if [[ ! -f "$hdr" || "$xml" -nt "$hdr" ]]; then
            info "wayland-scanner server-header $stem"
            "$WAYLAND_SCANNER_BIN" server-header "$xml" "$hdr"
        fi
        if [[ ! -f "$src" || "$xml" -nt "$src" ]]; then
            info "wayland-scanner private-code $stem"
            "$WAYLAND_SCANNER_BIN" private-code "$xml" "$src"
        fi

        WL_PROTOCOL_SOURCES+=("$src")
    done
}

target_build() {
    mkdir -p "$BUILD_DIR"

    #
    # Generate scanner output first so the build can include the
    # headers + compile the generated .c files.
    #
    target_scanner

    local cflags=(
        -O2 -g
        -fvisibility=hidden
        -Wall -Wno-unused-function -Wno-unused-parameter
        -DAPPNAME="\"$APPNAME\""
        #
        # Turn on memory_manager's tracked-alloc path. Without this
        # GUI_MALLOC / GUI_FREE degrade to raw malloc/free with zero
        # leak reporting.
        #
        -DGUI_TRACK_ALLOCATIONS
        -I"$SCRIPT_DIR/src"
        -I"$MEEK_UI_SRC"
        #
        # Generated server-header files -- let globals.c #include
        # them via "xdg-shell-protocol.h" / "linux-dmabuf-v1-protocol.h".
        #
        -I"$GEN_DIR"
    )
    # shellcheck disable=SC2206
    cflags+=($WL_CFLAGS $EGL_CFLAGS $INPUT_CFLAGS $XKB_CFLAGS)

    local ldflags=(
        #
        # memory_manager.c uses pthread_mutex_t on Linux.
        #
        -lpthread
    )
    # shellcheck disable=SC2206
    ldflags+=($WL_LIBS $EGL_LIBS $INPUT_LIBS $XKB_LIBS)

    local sources=(
        "${SOURCES_BASE[@]}"
        "${WL_PROTOCOL_SOURCES[@]}"
    )

    info "compiling $APPNAME"
    "$CC" "${cflags[@]}" -o "$BIN_OUT" "${sources[@]}" "${ldflags[@]}"
    ok "built $BIN_OUT"
    local size
    size=$(stat -c%s "$BIN_OUT" 2>/dev/null || stat -f%z "$BIN_OUT" 2>/dev/null || echo '?')
    info "size: $size bytes"
}

target_run() {
    target_build

    #
    # The process opens a wayland-N socket in $XDG_RUNTIME_DIR.
    # If that env var is unset we log a hint rather than silently
    # fail inside libwayland-server.
    #
    if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
        err "XDG_RUNTIME_DIR is not set. libwayland-server needs it to place the socket."
        err "  try: export XDG_RUNTIME_DIR=/run/user/\$(id -u)"
    fi

    info "running $BIN_OUT"
    "$BIN_OUT"
}

target_clean() {
    for p in "$BUILD_DIR" "$BIN_OUT"; do
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

  # postmarketOS / Alpine (the primary target):
  sudo apk add clang pkgconf wayland-dev wayland-protocols \
               mesa-dev libdrm-dev libx11-dev \
               libinput-dev eudev-dev libxkbcommon-dev

  # Ubuntu / Debian:
  sudo apt-get install clang pkg-config libwayland-dev wayland-protocols \
                       libegl-dev libgles-dev libgbm-dev libdrm-dev libx11-dev \
                       libinput-dev libudev-dev libxkbcommon-dev

  # Fedora:
  sudo dnf install clang pkgconf-pkg-config wayland-devel wayland-protocols-devel \
                   mesa-libEGL-devel mesa-libGLES-devel mesa-libgbm-devel \
                   libdrm-devel libX11-devel libinput-devel systemd-devel \
                   libxkbcommon-devel

  # Arch:
  sudo pacman -S clang pkgconf wayland wayland-protocols mesa libdrm libx11 \
                 libinput libxkbcommon

These bring in:
  * libwayland-server  -- the runtime we link against
  * wayland-scanner    -- the code generator (turns XML -> C bindings)
  * wayland-protocols  -- the standard extension XMLs (xdg-shell,
                          linux-dmabuf, presentation-time, etc.)
  * mesa / libgbm      -- EGL + GLES + GBM for dmabuf import + output
  * libdrm             -- DRM/KMS for the DRM output backend
  * libinput + libudev -- /dev/input device enumeration + event reads
  * libxkbcommon       -- build the xkb keymap that wl_keyboard clients
                          receive (seat.c; ships US QWERTY by default)

The build.sh runs wayland-scanner as part of every build,
generating ${SCRIPT_DIR}/build/protocols/*.c files that get
compiled alongside main.c + globals.c.
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
