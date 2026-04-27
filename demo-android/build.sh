#!/usr/bin/env bash
# =============================================================================
# build.sh - Android build for this repo, Linux / macOS.
# =============================================================================
#
# Mirror of build.ps1 for POSIX hosts. Same responsibilities, same native-
# only tooling (no MSYS2 equivalent needed -- POSIX is the "native" here).
# Calls the NDK clang wrappers, aapt, aapt add, zipalign, apksigner, and
# keytool directly. Supports Linux and macOS; picks the NDK prebuilt dir
# based on uname.
#
# -----------------------------------------------------------------------------
# USAGE
# -----------------------------------------------------------------------------
#
#   ./build.sh                    # DEFAULT: build + install + launch
#                                 # (requires a connected device)
#   ./build.sh build              # build the APK on disk, no device needed
#   ./build.sh push               # build + adb install -r (no launch)
#   ./build.sh testsdk            # print discovered SDK / NDK / JDK paths
#   ./build.sh clean              # wipe build outputs
#   ./build.sh manifest           # generate AndroidManifest.xml only
#   ./build.sh uninstall          # adb uninstall
#
# Overrides -- set any of these as env vars:
#
#   APPNAME=hello LABEL="Hello" PACKAGENAME=org.example.hello ./build.sh
#   ANDROIDVERSION=28 ./build.sh
#   SDK=/path/to/Sdk NDK=/path/to/ndk/25.2.x ./build.sh
#

set -euo pipefail

# -----------------------------------------------------------------------------
# Configuration (env-overridable)
# -----------------------------------------------------------------------------

APPNAME=${APPNAME:-guidemo}
LABEL=${LABEL:-guidemo}
PACKAGENAME=${PACKAGENAME:-org.novyworkbench.guidemo}
ANDROIDVERSION=${ANDROIDVERSION:-30}
ANDROIDTARGET=${ANDROIDTARGET:-$ANDROIDVERSION}
STOREPASS=${STOREPASS:-password}
ALIASNAME=${ALIASNAME:-standkey}

SDK=${SDK:-${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}}
NDK=${NDK:-${ANDROID_NDK_HOME:-}}
BUILD_TOOLS=${BUILD_TOOLS:-}
JDK=${JDK:-${JAVA_HOME:-}}

# Default target: 'run' (build + install + launch, flutter-style). Use
# 'build' explicitly if you just want the APK on disk, no device needed.
TARGET=${1:-run}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="$SCRIPT_DIR/build"
MAKECAPK="$BUILD_DIR/makecapk"
MANIFEST_OUT="$BUILD_DIR/AndroidManifest.xml"
MANIFEST_TPL="$SCRIPT_DIR/AndroidManifest.xml.template"
KEYSTORE_FILE="$SCRIPT_DIR/my-release-key.keystore"
APK_FINAL="$SCRIPT_DIR/$APPNAME.apk"

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

info()  { printf '\033[36m[build]\033[0m %s\n' "$*"; }
ok()    { printf '\033[32m[build]\033[0m %s\n' "$*"; }
err()   { printf '\033[31m[build]\033[0m %s\n' "$*" >&2; }
fatal() { err "$*"; exit 1; }

first_existing() {
    for p in "$@"; do
        [[ -n "$p" && -d "$p" ]] && { printf '%s' "$p"; return 0; }
    done
    return 1
}

latest_subdir() {
    local parent="$1"
    [[ -d "$parent" ]] || return 1
    # Sort version-like names properly. Works for NDK (25.2.x), build-tools
    # (34.0.0), and ndk side-by-side installs.
    local picked
    picked=$(ls -1 "$parent" 2>/dev/null | sort -V | tail -1 || true)
    [[ -n "$picked" ]] || return 1
    printf '%s' "$parent/$picked"
}

# -----------------------------------------------------------------------------
# Host OS detection -- drives the NDK prebuilt subdir
# -----------------------------------------------------------------------------

case "$(uname -s)" in
    Linux)   OS_NAME="linux-x86_64" ;;
    Darwin)
        # NDK r23+ ships darwin-x86_64 only; Apple Silicon runs it via
        # Rosetta. Newer NDK releases (r26+) may ship darwin-arm64 too; we
        # prefer -arm64 when present, fall back to -x86_64.
        if [[ "$(uname -m)" == "arm64" ]]; then
            OS_NAME_CANDIDATES=("darwin-arm64" "darwin-x86_64")
        else
            OS_NAME_CANDIDATES=("darwin-x86_64")
        fi
        OS_NAME="${OS_NAME_CANDIDATES[0]}"
        ;;
    *)       fatal "Unsupported host OS: $(uname -s)" ;;
esac

# -----------------------------------------------------------------------------
# SDK / NDK / build-tools / JDK discovery
# -----------------------------------------------------------------------------

if [[ -z "$SDK" ]]; then
    SDK=$(first_existing \
        "$HOME/Android/Sdk" \
        "$HOME/Library/Android/sdk" \
        "/opt/android-sdk" \
        "/usr/lib/android-sdk" \
        ) || fatal "Android SDK not found. Set SDK=... or ANDROID_HOME=..."
fi
[[ -d "$SDK" ]] || fatal "SDK path does not exist: $SDK"

if [[ -z "$NDK" ]]; then
    NDK=$(latest_subdir "$SDK/ndk") || NDK=$(first_existing "$SDK/ndk-bundle") \
        || fatal "Android NDK not found under $SDK/ndk. Install via SDK Manager or pass NDK=..."
fi

if [[ -z "$BUILD_TOOLS" ]]; then
    BUILD_TOOLS=$(latest_subdir "$SDK/build-tools") \
        || fatal "No build-tools found under $SDK/build-tools."
fi

# On macOS with an arm64 NDK, verify the candidate bin dir actually exists;
# otherwise fall back to the x86_64 dir.
if [[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]]; then
    if [[ ! -d "$NDK/toolchains/llvm/prebuilt/$OS_NAME" ]]; then
        OS_NAME="darwin-x86_64"
    fi
fi

NDK_BIN="$NDK/toolchains/llvm/prebuilt/$OS_NAME/bin"
[[ -d "$NDK_BIN" ]] || fatal "NDK bin dir missing: $NDK_BIN"

# android.jar: exact match first, else highest installed.
WANTED_JAR="$SDK/platforms/android-$ANDROIDVERSION/android.jar"
if [[ -f "$WANTED_JAR" ]]; then
    ANDROID_JAR="$WANTED_JAR"
else
    FALLBACK_DIR=$(ls -d "$SDK"/platforms/android-[0-9]* 2>/dev/null \
        | sort -t'-' -k2 -n | tail -1 || true)
    if [[ -n "$FALLBACK_DIR" && -f "$FALLBACK_DIR/android.jar" ]]; then
        ANDROID_JAR="$FALLBACK_DIR/android.jar"
        info "platform android-$ANDROIDVERSION not installed; falling back to $(basename "$FALLBACK_DIR")"
    else
        fatal "No Android platform jars found under $SDK/platforms. Install any via SDK Manager."
    fi
fi

# keytool: $JDK wins, then Android Studio jbr/jre, then PATH.
if [[ -z "$JDK" || ! -x "$JDK/bin/keytool" ]]; then
    for candidate in \
        "$HOME/.android-studio/jbr" \
        "/opt/android-studio/jbr" \
        "/Applications/Android Studio.app/Contents/jbr/Contents/Home" \
        "/Applications/Android Studio.app/Contents/jre/Contents/Home" \
        ; do
        if [[ -x "$candidate/bin/keytool" ]]; then JDK="$candidate"; break; fi
    done
fi
if [[ -n "$JDK" && -x "$JDK/bin/keytool" ]]; then
    KEYTOOL="$JDK/bin/keytool"
else
    KEYTOOL="$(command -v keytool || true)"
fi

AAPT="$BUILD_TOOLS/aapt"
ZIPALIGN="$BUILD_TOOLS/zipalign"
APKSIGNER="$BUILD_TOOLS/apksigner"
ADB="$SDK/platform-tools/adb"
[[ -x "$ADB" ]] || ADB="$(command -v adb || true)"

# -----------------------------------------------------------------------------
# NDK clang wrapper resolver
# -----------------------------------------------------------------------------
#
# Prefer exact API match; otherwise pick the highest level <= requested (so
# we stay ABI-compatible with older devices), else the lowest level > requested.
#
resolve_clang() {
    local triple="$1"
    local wanted="$2"

    local exact="$NDK_BIN/${triple}${wanted}-clang"
    if [[ -x "$exact" ]]; then
        printf '%s' "$exact"
        return 0
    fi

    local levels
    levels=$(ls "$NDK_BIN"/${triple}[0-9]*-clang 2>/dev/null | sed -E "s|.*/${triple}([0-9]+)-clang$|\1|" | sort -n)
    [[ -n "$levels" ]] || fatal "No clang wrappers under $NDK_BIN for triple '$triple'."

    local below above chosen
    below=$(echo "$levels" | awk -v w="$wanted" '$1<=w' | tail -1 || true)
    above=$(echo "$levels" | awk -v w="$wanted" '$1> w' | head -1 || true)
    if [[ -n "$below" ]]; then
        chosen="$below"
    else
        chosen="$above"
    fi
    info "NDK: no ${triple}${wanted}-clang; using ${triple}${chosen}-clang"
    printf '%s' "$NDK_BIN/${triple}${chosen}-clang"
}

CC_ARM64=$(resolve_clang 'aarch64-linux-android'     "$ANDROIDVERSION")
CC_ARM32=$(resolve_clang 'armv7a-linux-androideabi'  "$ANDROIDVERSION")
CC_X86=$(resolve_clang   'i686-linux-android'        "$ANDROIDVERSION")
CC_X86_64=$(resolve_clang 'x86_64-linux-android'     "$ANDROIDVERSION")

# -----------------------------------------------------------------------------
# Source list -- mirrors build.ps1's $Sources
# -----------------------------------------------------------------------------

# android_native_app_glue ships in the NDK at
# $NDK/sources/android/native_app_glue/. Use it directly -- no reason to
# vendor a copy. Tracks whatever glue patches each NDK release brings.
GLUE_DIR="$NDK/sources/android/native_app_glue"
[[ -f "$GLUE_DIR/android_native_app_glue.c" ]] \
    || fatal "android_native_app_glue.c not found under $GLUE_DIR -- unusual NDK layout."

SOURCES=(
    "$ROOT/gui/src/scene.c"
    "$ROOT/gui/src/platforms/android/fs_android.c"
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
    "$ROOT/gui/src/platforms/android/platform_android.c"
    "$ROOT/gui/src/clib/memory_manager.c"
    "$ROOT/gui/src/clib/stdlib.c"
    "$ROOT/gui/src/hot_reload.c"
    "$GLUE_DIR/android_native_app_glue.c"
    "$SCRIPT_DIR/main.c"
)

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------

target_testsdk() {
    echo
    echo "SDK         : $SDK"
    echo "NDK         : $NDK"
    echo "Build Tools : $BUILD_TOOLS"
    echo "android.jar : $ANDROID_JAR"
    echo "keytool     : ${KEYTOOL:-<not found>}"
    echo "adb         : ${ADB:-<not found>}"
    echo "OS_NAME     : $OS_NAME"
    echo
    echo "Clang ARM64 : $CC_ARM64"
    echo "Clang ARM32 : $CC_ARM32"
    echo "Clang x86   : $CC_X86"
    echo "Clang x64   : $CC_X86_64"
}

target_manifest() {
    mkdir -p "$BUILD_DIR"
    [[ -f "$MANIFEST_TPL" ]] || fatal "Template not found: $MANIFEST_TPL"
    sed \
        -e "s|\${PACKAGENAME}|$PACKAGENAME|g" \
        -e "s|\${ANDROIDVERSION}|$ANDROIDVERSION|g" \
        -e "s|\${ANDROIDTARGET}|$ANDROIDTARGET|g" \
        -e "s|\${APPNAME}|$APPNAME|g" \
        -e "s|\${LABEL}|$LABEL|g" \
        "$MANIFEST_TPL" > "$MANIFEST_OUT"
    ok "wrote $MANIFEST_OUT"
}

target_keystore() {
    [[ -f "$KEYSTORE_FILE" ]] && return 0
    [[ -n "${KEYTOOL:-}" ]] || fatal "keytool not found. Set JDK=... or install a JDK."
    info "generating debug keystore ($KEYSTORE_FILE)"
    "$KEYTOOL" -genkey -v \
        -keystore "$KEYSTORE_FILE" \
        -alias "$ALIASNAME" \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass "$STOREPASS" -keypass "$STOREPASS" \
        -dname 'CN=example.com, OU=ID, O=Example, L=Doe, ST=John, C=GB'
}

build_so() {
    local clang="$1"
    local abi="$2"
    shift 2
    local extra=("$@")

    [[ -x "$clang" ]] || fatal "clang wrapper not executable: $clang"

    local outdir="$MAKECAPK/lib/$abi"
    mkdir -p "$outdir"
    local outso="$outdir/lib${APPNAME}.so"

    info "compiling $abi -> $outso"

    # Build-time defines header for APPNAME (same rationale as build.ps1:
    # embedded-quote -D defines are nearly impossible to pass through shell
    # layers intact, so we use -include instead).
    local defs="$BUILD_DIR/android_build_defines.h"
    mkdir -p "$BUILD_DIR"
    cat > "$defs" <<EOF
/* Auto-generated by build.sh -- do not edit. */
#ifndef ANDROID_BUILD_DEFINES_H
#define ANDROID_BUILD_DEFINES_H
#define APPNAME "$APPNAME"
/* DEMO_SOURCE_DIR is the absolute dev-tree path on Windows (set by
 * demo-windows/CMakeLists.txt). On Android it doesn't exist -- assets
 * are resolved through AAssetManager via bare names. Expanding to ""
 * means \`DEMO_SOURCE_DIR "/main.ui"\` in host code yields "/main.ui",
 * which fs_android strips to "main.ui" before the asset lookup.
 */
#define DEMO_SOURCE_DIR ""
#endif
EOF

    local cflags=(
        -Os -ffunction-sections -fdata-sections -fvisibility=hidden
        -Wall -fPIC
        -DANDROID -DANDROIDVERSION="$ANDROIDVERSION"
        -DGUI_TRACK_ALLOCATIONS
        -include "$defs"
        -I"$GLUE_DIR"
        -I"$ROOT/gui/src"
    )
    local ldflags=(
        -lm -lGLESv3 -lEGL -landroid -llog
        -shared -uANativeActivity_onCreate
        -Wl,--gc-sections -s
    )

    "$clang" "${cflags[@]}" "${extra[@]}" -o "$outso" "${SOURCES[@]}" "${ldflags[@]}"
    ok "built $outso"
}

target_build() {
    # See the matching comment in build.ps1: every build is from scratch
    # (fast enough on 4 ABIs) so we never serve a stale APK after a
    # manifest or template edit. The keystore sits outside $BUILD_DIR and
    # survives the clean.
    target_clean
    target_manifest
    target_keystore

    mkdir -p "$MAKECAPK/assets"

    #
    # Stage APK assets. Anything under demo-android/assets/ becomes
    # readable at runtime via AAssetManager_open -- fs.c routes that
    # through fs__set_asset_manager so parser_xml__load_ui("main.ui")
    # and parser_style__load_styles("main.style") resolve against the
    # APK. Also grabs any loose *.ui / *.style files next to main.c.
    #
    if [ -d "$SCRIPT_DIR/assets" ]; then
        cp -R "$SCRIPT_DIR/assets/." "$MAKECAPK/assets/"
        echo "[build] staged assets from $SCRIPT_DIR/assets -> $MAKECAPK/assets"
    fi
    for f in "$SCRIPT_DIR"/*.ui "$SCRIPT_DIR"/*.style; do
        if [ -f "$f" ]; then
            cp "$f" "$MAKECAPK/assets/"
            echo "[build] staged $(basename "$f") -> $MAKECAPK/assets"
        fi
    done
    #
    # Also stage .ui / .style from demo-windows/ so the Android APK
    # ships the same UI the Windows demo edits. Keeps the two targets
    # visually identical without duplicating source files. Local
    # demo-android/*.ui / *.style (if any) wins because it was copied
    # first -- same cp target path, second cp overwrites would lose.
    # We want the opposite: only copy from demo-windows if the
    # destination doesn't already exist.
    #
    for f in "$ROOT/demo-windows"/*.ui "$ROOT/demo-windows"/*.style "$ROOT/demo-windows"/*.png "$ROOT/demo-windows"/*.jpg "$ROOT/demo-windows"/*.jpeg; do
        if [ ! -f "$f" ]; then
            continue
        fi
        bn=$(basename "$f")
        #
        # aapt rejects asset filenames with bytes outside ASCII.
        # Skip silently rather than fail the build -- anything the
        # UI actually references will be named ASCII.
        #
        if printf '%s' "$bn" | LC_ALL=C grep -q '[^ -~]'; then
            echo "[build] skipped non-ASCII filename $bn (aapt can't package it)"
            continue
        fi
        if [ ! -f "$MAKECAPK/assets/$bn" ]; then
            cp "$f" "$MAKECAPK/assets/"
            echo "[build] staged $bn from demo-windows -> $MAKECAPK/assets"
        fi
    done

    #
    # Stage the wallpapers/ subdirectory preserving its layout so
    # <image src="wallpapers/foo.jpg"/> resolves against the same
    # relative path on Android as on Windows. Same ASCII filter.
    #
    if [ -d "$ROOT/demo-windows/wallpapers" ]; then
        mkdir -p "$MAKECAPK/assets/wallpapers"
        for f in "$ROOT/demo-windows/wallpapers"/*.png "$ROOT/demo-windows/wallpapers"/*.jpg "$ROOT/demo-windows/wallpapers"/*.jpeg; do
            if [ ! -f "$f" ]; then
                continue
            fi
            bn=$(basename "$f")
            if printf '%s' "$bn" | LC_ALL=C grep -q '[^ -~]'; then
                continue
            fi
            cp "$f" "$MAKECAPK/assets/wallpapers/"
        done
        echo "[build] staged wallpapers/ from demo-windows -> $MAKECAPK/assets/wallpapers"
    fi

    #
    # Stage TTF fonts from gui/src/fonts/ into assets/fonts/. The
    # Android text pipeline reads them at startup via
    # AAssetManager_openDir("fonts") + font__register_from_file for
    # each .ttf. Same bundled font set as the Windows build's
    # GUI_FONTS_SOURCE_DIR auto-scan.
    #
    if [ -d "$ROOT/gui/src/fonts" ]; then
        mkdir -p "$MAKECAPK/assets/fonts"
        for f in "$ROOT/gui/src/fonts"/*.ttf; do
            if [ -f "$f" ]; then
                cp "$f" "$MAKECAPK/assets/fonts/"
                echo "[build] staged font $(basename "$f") -> $MAKECAPK/assets/fonts"
            fi
        done
    fi

    build_so "$CC_ARM64"  arm64-v8a    -m64
    build_so "$CC_ARM32"  armeabi-v7a  -mfloat-abi=softfp -m32
    build_so "$CC_X86"    x86          -march=i686 -mssse3 -mfpmath=sse -m32
    build_so "$CC_X86_64" x86_64       -march=x86-64 -msse4.2 -mpopcnt -m64

    local temp_apk="$BUILD_DIR/temp.apk"
    rm -f "$temp_apk"

    info "aapt package (manifest + assets)"
    "$AAPT" package -f \
        -F "$temp_apk" \
        -I "$ANDROID_JAR" \
        -M "$MANIFEST_OUT" \
        -A "$MAKECAPK/assets" \
        --target-sdk-version "$ANDROIDTARGET"

    # aapt add stores archive paths relative to cwd, so cd into makecapk/.
    (
        cd "$MAKECAPK"
        for abi in arm64-v8a armeabi-v7a x86 x86_64; do
            local rel="lib/$abi/lib${APPNAME}.so"
            info "aapt add $rel"
            "$AAPT" add -v "$temp_apk" "$rel"
        done
    )

    rm -f "$APK_FINAL"
    info "zipalign"
    "$ZIPALIGN" -v 4 "$temp_apk" "$APK_FINAL"

    info "apksigner sign"
    "$APKSIGNER" sign \
        --ks "$KEYSTORE_FILE" \
        --ks-pass "pass:$STOREPASS" \
        --key-pass "pass:$STOREPASS" \
        "$APK_FINAL"

    rm -f "$temp_apk"
    ok "signed APK: $APK_FINAL"
    local size
    size=$(stat -c%s "$APK_FINAL" 2>/dev/null || stat -f%z "$APK_FINAL" 2>/dev/null || echo '?')
    info "size: $size bytes"
}

target_clean() {
    for p in "$BUILD_DIR" "$APK_FINAL" "$SCRIPT_DIR/output.map"; do
        if [[ -e "$p" ]]; then
            info "removing $p"
            rm -rf "$p"
        fi
    done
    ok 'clean done'
}

target_push() {
    # Always rebuild before pushing (mirror of build.ps1's Target-Push):
    # auto-clean means any cached APK could be stale. The rebuild cost
    # is a few seconds of clang, negligible vs. the "wait, did that
    # install the new version?" debugging tax.
    target_build
    [[ -n "${ADB:-}" ]] || fatal "adb not found."
    info "adb install -r $APK_FINAL"
    "$ADB" install -r "$APK_FINAL"
}

target_run() {
    target_push
    local activity="$PACKAGENAME/android.app.NativeActivity"
    info "adb shell am start -n $activity"
    "$ADB" shell am start -n "$activity"
}

target_uninstall() {
    [[ -n "${ADB:-}" ]] || fatal "adb not found."
    info "adb uninstall $PACKAGENAME"
    "$ADB" uninstall "$PACKAGENAME" || true
}

# -----------------------------------------------------------------------------
# Dispatch
# -----------------------------------------------------------------------------

case "$TARGET" in
    run|'')          target_run ;;     # default: build + install + launch
    build|apk)       target_build ;;   # build APK only, no device required
    push)            target_push ;;    # build + install (no launch)
    testsdk)         target_testsdk ;;
    clean)           target_clean ;;
    manifest)        target_manifest ;;
    keystore)        target_keystore ;;
    uninstall)       target_uninstall ;;
    *)               err "Unknown target: $TARGET"; echo "Valid: run (default), build, push, testsdk, clean, manifest, uninstall"; exit 2 ;;
esac
