# Android build

First-pass port of the gui toolkit to Android, patterned directly on
[rawdrawandroid](https://github.com/cnlohr/rawdrawandroid) (which lives
at `../rawdrawandroid/`). Goal: produce a signed APK that launches on a
device, brings up an EGL + GLES 3.0 context, and renders at least the
panel + two buttons defined in `main.c`.

## Scope (first pass)

- **Builds on Android:** scene + widgets + widget registry + font module
  (compiles but loads no TTFs yet) + new GLES 3.0 renderer + new
  android_native_app_glue-based platform layer.
- **Not yet on Android:** text rendering (no atlas pipeline in gles3 yet),
  XML/CSS parsing (scene is built in code in `main.c`), hot reload, touch
  input. Stubs + exclusions in place so the rest compiles cleanly.

## Prerequisites

Any Android-development toolchain gets you the four pieces we need — SDK,
NDK, build-tools, JDK — in known locations:

- **Visual Studio / MAUI Android workload** (Windows)
- **Android Studio** (Windows, Linux, macOS)
- **Standalone SDK + NDK command-line tools** (any host)

Each build target maps to one native tool we invoke directly:

| Tool          | Comes from    | Windows path                            | POSIX path                           |
|---------------|---------------|-----------------------------------------|--------------------------------------|
| Clang         | NDK           | `...\ndk\<ver>\...\bin\*-clang.cmd`     | `.../ndk/<ver>/.../bin/*-clang`      |
| aapt          | build-tools   | `...\build-tools\<ver>\aapt.exe`        | `.../build-tools/<ver>/aapt`         |
| zipalign      | build-tools   | `...\build-tools\<ver>\zipalign.exe`    | `.../build-tools/<ver>/zipalign`     |
| apksigner     | build-tools   | `...\build-tools\<ver>\apksigner.bat`   | `.../build-tools/<ver>/apksigner`    |
| keytool       | JDK           | `...\bin\keytool.exe`                   | `.../bin/keytool`                    |
| adb           | SDK           | `...\platform-tools\adb.exe`            | `.../platform-tools/adb`             |

No MSYS2, no WSL, no Cygwin, no `make`, no `envsubst`, no `zip`/`unzip`.
Everything the build needs ships with the SDK + NDK + JDK.

## Building on Windows (native)

`build.bat` is a one-line wrapper that runs `build.ps1` via PowerShell with
`-ExecutionPolicy Bypass`.

```cmd
cd android
build.bat testsdk            rem  print discovered SDK / NDK / JDK paths
build.bat                    rem  DEFAULT: build + adb install + am start
build.bat build              rem  build APK only, no device needed
build.bat push               rem  build + adb install (no launch)
build.bat clean
build.bat -AppName myapp -PackageName com.me.myapp
```

The default target is `run` — Flutter-style one-shot "build + deploy +
launch". Plug in a device (or start an emulator), type `build.bat`, and
the app is running a few seconds later. No manual APK copy.

Auto-discovery paths:

- SDK: `%LOCALAPPDATA%\Android\Sdk`, `%ProgramFiles(x86)%\Android\android-sdk`, or `$env:ANDROID_HOME`
- NDK: `$SDK\ndk\<latest>`
- build-tools: `$SDK\build-tools\<latest>`
- keytool: `$env:JAVA_HOME`, Android Studio's JBR, or the Microsoft Build of OpenJDK under `%ProgramFiles%\Microsoft`

Override any of these with `-Sdk`, `-Ndk`, `-BuildTools`, or `-Jdk`.

Run `build.ps1` directly if you prefer:

```powershell
powershell.exe -ExecutionPolicy Bypass -File build.ps1 testsdk
```

## Building on Linux / macOS

`build.sh` is the POSIX equivalent of `build.ps1`. Same targets, same
flags, same behavior — it just calls the POSIX-named binaries (`aapt`
instead of `aapt.exe`, etc.) and picks the NDK's `linux-x86_64` /
`darwin-x86_64` / `darwin-arm64` prebuilt dir based on `uname`.

```sh
cd android
./build.sh testsdk          # verify SDK / NDK / JDK paths
./build.sh                  # DEFAULT: build + adb install + am start
./build.sh build            # build APK only, no device needed
./build.sh push             # build + adb install (no launch)
./build.sh clean

APPNAME=myapp PACKAGENAME=com.me.myapp ./build.sh
ANDROIDVERSION=28 ./build.sh
SDK=/path/to/Sdk NDK=/path/to/ndk/25.2.x ./build.sh
```

Auto-discovery paths:

- SDK: `$ANDROID_HOME`, `$ANDROID_SDK_ROOT`, `~/Android/Sdk`, `~/Library/Android/sdk`, `/opt/android-sdk`
- NDK: `$SDK/ndk/<latest>`
- build-tools: `$SDK/build-tools/<latest>`
- keytool: `$JAVA_HOME`, Android Studio's JBR, or `PATH`

## Every build is a clean build

Both scripts call `clean` automatically at the start of `build` (and
`run`). That nukes `build/`, the previous `$APPNAME.apk`, and any stale
generated manifest; `$APPNAME.apk` gets re-signed from scratch. The
keystore survives (it lives next to the scripts, not in `build/`) so
`adb install -r` keeps working across rebuilds. Rebuilding the four
`.so` files each time costs a few seconds on a modern machine — worth
it for the "edit → run → it's actually up-to-date" guarantee.

Override knobs:

```
make APPNAME=myapp LABEL="My App" PACKAGENAME=com.example.myapp
make ANDROIDVERSION=29
make ANDROIDSDK=/path/to/Android/Sdk NDK=/path/to/ndk
```

## Installing + running

```
make push              # adb install -r guidemo.apk
make run               # push + am start
make uninstall
```

## Files

- `main.c` — `android_main()` entry point. Builds a hardcoded scene
  (panel + two buttons) and runs the platform tick loop.
- `AndroidManifest.xml.template` — NativeActivity manifest with
  placeholders for package name / label / SDK version. Generated at
  build time by `make manifest`.
- `Makefile` — NDK clang invocations for the four supported ABIs
  (arm64-v8a, armeabi-v7a, x86, x86_64), APK packaging via `aapt`,
  alignment via `zipalign`, signing via `apksigner`.
- `my-release-key.keystore` — generated on first build. Debug-grade
  RSA-2048; not suitable for Play Store publication.
- `build/` — transient outputs (per-arch .so, temp.apk, final .apk).
  `make clean` wipes it.

## Known gaps / next steps

1. **Text pipeline in gles3_renderer.** The four `renderer__*_atlas`
   entry points are stubs right now; adding the textured-glyph pass
   is a mechanical port of the opengl3 backend (same atlas upload via
   `glTexImage2D(GL_R8, ..., GL_RED, GL_UNSIGNED_BYTE, ...)`, same
   per-atlas run batching). Once those exist, `font__register_from_memory`
   with TTF bytes pulled from APK assets gives us text.
2. **Asset-backed fonts/XML/CSS.** Ship `.ttf` / `.ui` / `.style` in
   `android/assets/` so `aapt` packs them into the APK. Load at runtime
   via `AAssetManager` (pointer is accessible through
   `app->activity->assetManager`).
3. **Touch input.** Wire `app->onInputEvent` to `AMotionEvent_getX/Y`
   and dispatch to `scene__on_mouse_move` / `scene__on_mouse_button`.
4. **Hot reload.** Polling content-hash watcher needs a POSIX clock;
   swap `GetTickCount64` for `clock_gettime(CLOCK_MONOTONIC)` inside
   `hot_reload.c` and it's portable.
