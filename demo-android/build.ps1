# ============================================================================
# build.ps1 - Android build for this repo, native Windows (no MSYS2 / WSL).
# ============================================================================
#
# Replaces the POSIX Makefile for Windows users who already have the MAUI /
# Android Studio toolchain installed. Calls only native Windows binaries that
# ship with the SDK + NDK + JDK:
#
#   NDK clang wrappers  aarch64-linux-android30-clang.cmd, etc.
#   aapt.exe            package manifest + assets into an APK
#   aapt add            add the .so files to the APK with correct compression
#   zipalign.exe        align the ZIP at 4-byte boundaries
#   apksigner.bat       APK v2+ signing (required on Android 30+)
#   keytool.exe         first-run keystore generation
#   adb.exe             install + launch
#
# No dependency on bash, make, zip, unzip, or envsubst. The manifest template
# placeholders are resolved via plain PowerShell string replacement. The
# APK zip layout is managed by aapt (which already gets compression right for
# .so, AndroidManifest.xml, and resources.arsc).
#
# ---------------------------------------------------------------------------
# USAGE
# ---------------------------------------------------------------------------
#
#   .\build.ps1                     # DEFAULT: build + install + launch
#                                   # (requires a connected device)
#   .\build.ps1 build               # build the APK on disk, no device needed
#   .\build.ps1 push                # build + adb install -r (no launch)
#   .\build.ps1 testsdk             # print discovered SDK / NDK / JDK paths
#   .\build.ps1 clean               # wipe build outputs
#   .\build.ps1 manifest            # generate AndroidManifest.xml only
#   .\build.ps1 uninstall           # adb uninstall
#
# Override knobs (any combination; all optional):
#
#   .\build.ps1 -AppName hello -Label "Hello" -PackageName org.example.hello
#   .\build.ps1 -AndroidVersion 28
#   .\build.ps1 -Sdk "D:\AndroidSdk" -Ndk "D:\AndroidSdk\ndk\25.2.9519653"
#
# ---------------------------------------------------------------------------

param(
    #
    # Default target is 'run' -- one-shot build + adb install + am start,
    # flutter-style. Pass 'build' explicitly if you just want the APK on
    # disk (no device required).
    #
    [string]$Target = 'run',
    [string]$AppName = 'guidemo',
    [string]$Label = 'guidemo',
    [string]$PackageName = 'org.novyworkbench.guidemo',
    [int]$AndroidVersion = 30,
    [int]$AndroidTarget = 0,
    [string]$Sdk = $env:ANDROID_HOME,
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [string]$BuildTools = '',
    [string]$Jdk = $env:JAVA_HOME,
    [string]$StorePass = 'password',
    [string]$KeystoreFile = 'my-release-key.keystore',
    [string]$AliasName = 'standkey'
)

$ErrorActionPreference = 'Stop'
if ($AndroidTarget -eq 0) { $AndroidTarget = $AndroidVersion }

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Info([string]$msg)  { Write-Host "[build] $msg" -ForegroundColor Cyan }
function Ok  ([string]$msg)  { Write-Host "[build] $msg" -ForegroundColor Green }
function Err ([string]$msg)  { Write-Host "[build] $msg" -ForegroundColor Red }

function Find-FirstPath([string[]]$Candidates) {
    foreach ($c in $Candidates) {
        if (-not $c) { continue }
        $expanded = [System.Environment]::ExpandEnvironmentVariables($c)
        if (Test-Path $expanded) { return $expanded }
    }
    return $null
}

function Find-LatestSubdir([string]$Parent) {
    if (-not (Test-Path $Parent)) { return $null }
    $dirs = Get-ChildItem -Path $Parent -Directory -ErrorAction SilentlyContinue | Sort-Object Name
    if ($dirs.Count -eq 0) { return $null }
    return $dirs[-1].FullName
}

function Invoke-Native([string]$Exe, [string[]]$ArgList) {
    # Helper to surface a clean error on non-zero exit. The build needs to
    # abort if any single compile / package / sign step fails.
    #
    # NB: parameter is named $ArgList (NOT $Args) because $Args is a
    # PowerShell automatic variable -- declaring a param named $Args
    # silently shadows it and the splat @Args does not pass through the
    # values you expect. Rename-to-avoid-collision is the canonical
    # workaround.
    & $Exe @ArgList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed (exit $LASTEXITCODE): $Exe $($ArgList -join ' ')"
    }
}

# ---------------------------------------------------------------------------
# SDK / NDK / JDK / build-tools discovery
# ---------------------------------------------------------------------------
#
# MAUI on Visual Studio 2022 typically puts:
#   SDK:  %LOCALAPPDATA%\Android\Sdk   (same as Android Studio)
#         or %ProgramFiles(x86)%\Android\android-sdk  (legacy)
#   NDK:  $SDK\ndk\<version>\
#   JDK:  %ProgramFiles%\Microsoft\jdk-*  (Microsoft Build of OpenJDK)
#         or Android Studio's jbr:  %ProgramFiles%\Android\Android Studio\jbr
#
# Any override passed on the command line wins over auto-discovery.
# ---------------------------------------------------------------------------

if (-not $Sdk) {
    $Sdk = Find-FirstPath @(
        "$env:LOCALAPPDATA\Android\Sdk",
        "$env:ANDROID_SDK_ROOT",
        "$env:ProgramFiles\Android\android-sdk",
        "${env:ProgramFiles(x86)}\Android\android-sdk",
        "$env:USERPROFILE\AppData\Local\Android\Sdk"
    )
}
if (-not $Sdk) { throw "Android SDK not found. Pass -Sdk or set `$env:ANDROID_HOME." }
if (-not (Test-Path $Sdk)) { throw "SDK path does not exist: $Sdk" }

if (-not $Ndk) {
    # Prefer the newest version-numbered dir under $SDK\ndk\, falling back
    # to the legacy single-dir ndk-bundle.
    $Ndk = Find-LatestSubdir (Join-Path $Sdk 'ndk')
    if (-not $Ndk) { $Ndk = Find-FirstPath @((Join-Path $Sdk 'ndk-bundle')) }
}
if (-not $Ndk) { throw "Android NDK not found under $Sdk\ndk\. Install one via SDK Manager or pass -Ndk." }

if (-not $BuildTools) {
    # aapt / zipalign / apksigner live under build-tools\<version>\. pick
    # the latest version folder present.
    $BuildTools = Find-LatestSubdir (Join-Path $Sdk 'build-tools')
}
if (-not $BuildTools) { throw "No build-tools found under $Sdk\build-tools\." }

# ---------------------------------------------------------------------------
# Pick an android.jar.
# ---------------------------------------------------------------------------
#
# aapt uses the android.jar purely as a compile-time reference for resource
# linking -- the runtime jar on the device is different. Any platform jar
# >= our target SDK works, so if the exact $AndroidVersion isn't installed,
# fall back to the highest version the user does have. MAUI installs tend
# to be on 33/34; pure rawdrawandroid users often sit on 30. Either works.
#
$platformsDir = Join-Path $Sdk 'platforms'
$wantedJar    = Join-Path $platformsDir "android-$AndroidVersion\android.jar"

if (Test-Path $wantedJar) {
    $AndroidJar = $wantedJar
} else {
    # Scan what's installed. Sort numerically so android-34 beats android-9.
    $available = @()
    if (Test-Path $platformsDir) {
        $available = Get-ChildItem -Path $platformsDir -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^android-(\d+)$' } |
            ForEach-Object {
                [PSCustomObject]@{
                    Name    = $_.Name
                    Level   = [int]$matches[1]
                    JarPath = Join-Path $_.FullName 'android.jar'
                }
            } |
            Where-Object { Test-Path $_.JarPath } |
            Sort-Object Level
    }
    if ($available.Count -eq 0) {
        throw "No Android platform jars found under $platformsDir. Install any 'Android SDK Platform X' via SDK Manager (Visual Studio Installer / Android Studio)."
    }

    # Prefer the highest installed level; compile just needs something recent
    # enough to contain the (minimal) manifest symbols we reference.
    $picked = $available[-1]
    $AndroidJar = $picked.JarPath
    Info "platform android-$AndroidVersion not installed; falling back to $($picked.Name) (android.jar = $AndroidJar)"

    # If the fallback level is lower than our requested level, keep the
    # requested level for the NDK side (the NDK sysroot is independent of
    # the platform jar). If higher, also leave it -- we compile against
    # the requested API level deliberately.
}

# Keytool for keystore generation. JAVA_HOME wins; otherwise search.
$Keytool = $null
if ($Jdk -and (Test-Path (Join-Path $Jdk 'bin\keytool.exe'))) {
    $Keytool = Join-Path $Jdk 'bin\keytool.exe'
}
if (-not $Keytool) {
    $Keytool = Find-FirstPath @(
        "$env:ProgramFiles\Android\Android Studio\jbr\bin\keytool.exe",
        "$env:ProgramFiles\Android\Android Studio\jre\bin\keytool.exe"
    )
}
if (-not $Keytool) {
    $msJdk = Find-LatestSubdir "$env:ProgramFiles\Microsoft"
    if ($msJdk -and (Test-Path (Join-Path $msJdk 'bin\keytool.exe'))) {
        $Keytool = Join-Path $msJdk 'bin\keytool.exe'
    }
}
if (-not $Keytool) {
    $adoptium = Find-LatestSubdir "$env:ProgramFiles\Eclipse Adoptium"
    if ($adoptium -and (Test-Path (Join-Path $adoptium 'bin\keytool.exe'))) {
        $Keytool = Join-Path $adoptium 'bin\keytool.exe'
    }
}
# Final fallback: rely on PATH (works if `keytool` runs from a stock cmd).
if (-not $Keytool) { $Keytool = (Get-Command keytool.exe -ErrorAction SilentlyContinue).Source }

$Aapt      = Join-Path $BuildTools 'aapt.exe'
$Zipalign  = Join-Path $BuildTools 'zipalign.exe'
$ApkSigner = Join-Path $BuildTools 'apksigner.bat'
$Adb       = Join-Path $Sdk 'platform-tools\adb.exe'
if (-not (Test-Path $Adb)) { $Adb = (Get-Command adb.exe -ErrorAction SilentlyContinue).Source }

# NDK clang wrappers (one per target ABI). Windows NDK exposes these as
# .cmd scripts -- each injects the right --target=<triple><api> flag. The
# wrapper's filename bakes in the API level, and each NDK version only
# ships a specific range of levels (typically 21..latest). If the user's
# NDK doesn't have our requested level, fall back to the nearest match:
# prefer the highest level <= requested (so we stay ABI-compatible with
# older devices); otherwise take the lowest level above.
$NdkBin = Join-Path $Ndk "toolchains\llvm\prebuilt\windows-x86_64\bin"
if (-not (Test-Path $NdkBin)) { throw "NDK clang bin not found: $NdkBin" }

function Resolve-ClangWrapper([string]$TripleNoLevel, [int]$DesiredLevel) {
    # Exact match first -- fastest path for properly-provisioned NDKs.
    $exact = Join-Path $NdkBin "$TripleNoLevel$DesiredLevel-clang.cmd"
    if (Test-Path $exact) { return $exact }

    # Enumerate all installed API levels for this triple.
    $pattern = "$TripleNoLevel*-clang.cmd"
    $levels  = @()
    foreach ($file in (Get-ChildItem -Path $NdkBin -Filter $pattern -ErrorAction SilentlyContinue)) {
        if ($file.Name -match ([regex]::Escape($TripleNoLevel) + '(\d+)-clang\.cmd$')) {
            $levels += [int]$matches[1]
        }
    }
    if ($levels.Count -eq 0) {
        throw "No clang wrappers found under $NdkBin for triple '$TripleNoLevel'. Check your NDK install."
    }

    # Prefer the highest level <= desired; if none, take the lowest above.
    $below = ($levels | Where-Object { $_ -le $DesiredLevel } | Measure-Object -Maximum).Maximum
    $above = ($levels | Where-Object { $_ -gt $DesiredLevel } | Measure-Object -Minimum).Minimum
    if ($below) { $chosen = $below } else { $chosen = $above }

    $path = Join-Path $NdkBin "$TripleNoLevel$chosen-clang.cmd"
    Info "NDK: no $TripleNoLevel$DesiredLevel-clang.cmd; using $TripleNoLevel$chosen-clang.cmd"
    return $path
}

$ClangArm64   = Resolve-ClangWrapper 'aarch64-linux-android'     $AndroidVersion
$ClangArm32   = Resolve-ClangWrapper 'armv7a-linux-androideabi'  $AndroidVersion
$ClangX86     = Resolve-ClangWrapper 'i686-linux-android'        $AndroidVersion
$ClangX86_64  = Resolve-ClangWrapper 'x86_64-linux-android'      $AndroidVersion

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

$ScriptDir    = $PSScriptRoot
$BuildDir     = Join-Path $ScriptDir 'build'
$MakecapkDir  = Join-Path $BuildDir  'makecapk'
$ManifestOut  = Join-Path $BuildDir  'AndroidManifest.xml'
$ManifestTpl  = Join-Path $ScriptDir 'AndroidManifest.xml.template'
$KeystorePath = Join-Path $ScriptDir $KeystoreFile
$ApkFinal     = Join-Path $ScriptDir "$AppName.apk"

$Root = Resolve-Path (Join-Path $ScriptDir '..')

#
# android_native_app_glue.c + .h ship inside the NDK at
# $NDK\sources\android\native_app_glue\ -- that's the canonical upstream
# location. We used to pull it from a vendored rawdrawandroid/ copy, but
# there's no reason to vendor when every NDK already has it. Using the
# NDK copy also tracks whatever glue updates Google ships per NDK version.
#
$GlueDir = Join-Path $Ndk 'sources\android\native_app_glue'
$GlueSrc = Join-Path $GlueDir 'android_native_app_glue.c'
if (-not (Test-Path $GlueSrc)) {
    throw "android_native_app_glue.c not found at $GlueSrc. Unusual NDK layout -- override `$GlueDir or file a bug."
}

$Sources = @(
    "$Root\gui\src\scene.c",
    "$Root\gui\src\platforms\android\fs_android.c",
    "$Root\gui\src\font.c",
    "$Root\gui\src\scroll.c",
    "$Root\gui\src\animator.c",
    "$Root\gui\src\parser_xml.c",
    "$Root\gui\src\parser_style.c",
    "$Root\gui\src\widget_registry.c",
    "$Root\gui\src\widgets\widget_window.c",
    "$Root\gui\src\widgets\widget_column.c",
    "$Root\gui\src\widgets\widget_row.c",
    "$Root\gui\src\widgets\widget_button.c",
    "$Root\gui\src\widgets\widget_slider.c",
    "$Root\gui\src\widgets\widget_div.c",
    "$Root\gui\src\widgets\widget_text.c",
    "$Root\gui\src\widgets\widget_input.c",
    "$Root\gui\src\widgets\widget_checkbox.c",
    "$Root\gui\src\widgets\widget_radio.c",
    "$Root\gui\src\widgets\widget_select.c",
    "$Root\gui\src\widgets\widget_option.c",
    "$Root\gui\src\widgets\widget_image.c",
    "$Root\gui\src\widgets\widget_collection.c",
    "$Root\gui\src\widgets\widget_colorpicker.c",
    "$Root\gui\src\widgets\widget_popup.c",
    "$Root\gui\src\widgets\widget_textarea.c",
    "$Root\gui\src\widgets\widget_canvas.c",
    "$Root\gui\src\widgets\widget_keyboard.c",
    "$Root\gui\src\third_party\log.c",
    "$Root\gui\src\renderers\gles3_renderer.c",
    "$Root\gui\src\platforms\android\platform_android.c",
    "$Root\gui\src\clib\memory_manager.c",
    "$Root\gui\src\clib\stdlib.c",
    "$Root\gui\src\hot_reload.c",
    $GlueSrc,
    "$ScriptDir\main.c"
)

#
# Build-time string defines (APPNAME etc.) go through a generated
# prefix-include header instead of -DFOO="bar" on the command line.
# Why: PowerShell 5.1's native-command arg passing mangles embedded
# double quotes -- it will split `-DAPPNAME="guidemo"` into three
# separate arguments even when you backslash-escape. Dodging the
# escape entirely via -include path/to/header.h is the bulletproof
# fix; every TU compiles as if it started with the generated #include.
#
# NB: the path is declared here but the FILE is written inside
# Target-Build, AFTER Target-Clean runs -- otherwise the clean would
# wipe the header we just wrote and the next compile would fail with
# "android_build_defines.h: file not found".
#
$defsHeader = Join-Path $BuildDir 'android_build_defines.h'

function Write-DefinesHeader {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    $defsContent = @"
/* Auto-generated by build.ps1 -- do not edit. */
#ifndef ANDROID_BUILD_DEFINES_H
#define ANDROID_BUILD_DEFINES_H
#define APPNAME "$AppName"
/* DEMO_SOURCE_DIR is the absolute dev-tree path on Windows (set by
 * demo-windows/CMakeLists.txt). On Android it doesn't exist -- assets
 * are resolved through AAssetManager via bare names. Expanding to ""
 * means DEMO_SOURCE_DIR "/main.ui" in host code yields "/main.ui",
 * which fs_android strips to "main.ui" before the asset lookup.
 */
#define DEMO_SOURCE_DIR ""
#endif
"@
    [System.IO.File]::WriteAllText($defsHeader, $defsContent, (New-Object System.Text.UTF8Encoding $false))
}

$CommonCflags = @(
    '-Os',
    '-ffunction-sections', '-fdata-sections', '-fvisibility=hidden',
    '-Wall',
    '-fPIC',
    '-DANDROID',
    "-DANDROIDVERSION=$AndroidVersion",
    '-DGUI_TRACK_ALLOCATIONS',
    '-include', $defsHeader,
    '-I', $GlueDir,
    '-I', (Join-Path $Root 'gui\src')
)

$CommonLdflags = @(
    '-lm', '-lGLESv3', '-lEGL', '-landroid', '-llog',
    '-shared',
    '-uANativeActivity_onCreate',
    '-Wl,--gc-sections',
    '-s'
)

function Build-So([string]$Clang, [string]$AbiDir, [string[]]$ExtraCflags) {
    if (-not (Test-Path $Clang)) {
        throw "Clang wrapper missing: $Clang -- check that NDK version supports API $AndroidVersion."
    }
    $outDir = Join-Path $MakecapkDir "lib\$AbiDir"
    $outSo  = Join-Path $outDir "lib$AppName.so"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    # NB: NOT $args -- that's a PowerShell automatic variable and
    # reassigning it silently drops the values on some PS versions.
    $clangArgs = @()
    $clangArgs += $CommonCflags
    $clangArgs += $ExtraCflags
    $clangArgs += '-o'
    $clangArgs += $outSo
    $clangArgs += $Sources
    $clangArgs += $CommonLdflags

    Info "compiling $AbiDir -> $outSo"
    Invoke-Native $Clang $clangArgs
    Ok "built $outSo"
}

function Target-TestSdk {
    Write-Host ""
    Write-Host "SDK         : $Sdk"
    Write-Host "NDK         : $Ndk"
    Write-Host "Build Tools : $BuildTools"
    Write-Host "android.jar : $AndroidJar"
    Write-Host "keytool     : $Keytool"
    Write-Host "adb         : $Adb"
    Write-Host ""
    Write-Host "Clang ARM64 : $ClangArm64"
    Write-Host "Clang ARM32 : $ClangArm32"
    Write-Host "Clang x86   : $ClangX86"
    Write-Host "Clang x64   : $ClangX86_64"
}

function Target-Manifest {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    if (-not (Test-Path $ManifestTpl)) { throw "Template not found: $ManifestTpl" }
    $text = Get-Content $ManifestTpl -Raw
    $text = $text -replace [regex]::Escape('${PACKAGENAME}'),    $PackageName
    $text = $text -replace [regex]::Escape('${ANDROIDVERSION}'), "$AndroidVersion"
    $text = $text -replace [regex]::Escape('${ANDROIDTARGET}'),  "$AndroidTarget"
    $text = $text -replace [regex]::Escape('${APPNAME}'),        $AppName
    $text = $text -replace [regex]::Escape('${LABEL}'),          $Label
    # No BOM -- aapt rejects BOM'd XML on some build-tools versions.
    [System.IO.File]::WriteAllText($ManifestOut, $text, (New-Object System.Text.UTF8Encoding $false))
    Ok "wrote $ManifestOut"
}

function Target-Keystore {
    if (Test-Path $KeystorePath) { return }
    if (-not $Keytool) { throw "keytool.exe not found. Pass -Jdk or install a JDK." }
    Info "generating debug keystore ($KeystorePath)"
    Invoke-Native $Keytool @(
        '-genkey', '-v',
        '-keystore', $KeystorePath,
        '-alias', $AliasName,
        '-keyalg', 'RSA',
        '-keysize', '2048',
        '-validity', '10000',
        '-storepass', $StorePass,
        '-keypass', $StorePass,
        '-dname', 'CN=example.com, OU=ID, O=Example, L=Doe, S=John, C=GB'
    )
}

function Target-Build {
    #
    # Always start with a wipe of build outputs so the APK is reproducible
    # regardless of whatever state prior runs left behind (stale manifest,
    # leftover intermediate apk, stale .so files from a different ABI set,
    # etc.). The keystore survives because it lives in $ScriptDir, not
    # $BuildDir -- regenerating the cert on every build would change the
    # signing identity and break 'adb install -r' (signatures wouldn't
    # match). Rebuilding the four .so files from scratch costs a few
    # seconds; worth the simpler mental model.
    #
    Target-Clean
    Target-Manifest
    Target-Keystore
    Write-DefinesHeader

    $AssetsStage = Join-Path $MakecapkDir 'assets'
    New-Item -ItemType Directory -Force -Path $AssetsStage | Out-Null

    #
    # Stage APK assets. Anything under demo-android/assets/ in source
    # becomes readable at runtime via AAssetManager_open("<filename>")
    # -- which fs.c routes through fs__set_asset_manager so
    # parser_xml__load_ui("main.ui") / parser_style__load_styles(
    # "main.style") resolve transparently against the APK.
    #
    # Also: legacy .ui/.style living directly in demo-android/ (not
    # under assets/) get copied too, so a host that keeps its data
    # files next to main.c doesn't have to reshuffle the directory.
    #
    $SourceAssets = Join-Path $ScriptDir 'assets'
    if (Test-Path $SourceAssets)
    {
        Copy-Item -Path (Join-Path $SourceAssets '*') -Destination $AssetsStage -Recurse -Force
        Info "staged assets from $SourceAssets -> $AssetsStage"
    }
    foreach ($pat in @('*.ui', '*.style'))
    {
        foreach ($f in Get-ChildItem -Path $ScriptDir -Filter $pat -ErrorAction SilentlyContinue)
        {
            Copy-Item -Path $f.FullName -Destination $AssetsStage -Force
            Info ("staged " + $f.Name + " -> " + $AssetsStage)
        }
    }
    #
    # Also stage .ui / .style from demo-windows/ so the Android APK
    # ships the same UI the Windows demo edits. Local demo-android
    # files (if any) win; demo-windows fills in anything missing.
    #
    $DemoWindowsDir = Join-Path $Root 'demo-windows'
    if (Test-Path $DemoWindowsDir)
    {
        foreach ($pat in @('*.ui', '*.style', '*.png', '*.jpg', '*.jpeg'))
        {
            foreach ($f in Get-ChildItem -Path $DemoWindowsDir -Filter $pat -ErrorAction SilentlyContinue)
            {
                #
                # aapt rejects asset filenames with bytes outside ASCII
                # (non-English screenshots, etc.). Skip them silently
                # rather than fail the whole build -- anything the UI
                # actually references is under dev control and will be
                # named ASCII.
                #
                $isAscii = $true
                foreach ($ch in $f.Name.ToCharArray())
                {
                    if ([int]$ch -gt 127) { $isAscii = $false; break }
                }
                if (-not $isAscii)
                {
                    Info ("skipped non-ASCII filename " + $f.Name + " (aapt can't package it)")
                    continue
                }
                $dest = Join-Path $AssetsStage $f.Name
                if (-not (Test-Path $dest))
                {
                    Copy-Item -Path $f.FullName -Destination $AssetsStage -Force
                    Info ("staged " + $f.Name + " from demo-windows -> " + $AssetsStage)
                }
            }
        }
        #
        # Stage the wallpapers/ subdirectory preserving its layout so
        # <image src="wallpapers/foo.jpg"/> resolves against the same
        # relative path on Android (AAssetManager subpaths) that it
        # does on Windows (filesystem subpaths). Same ASCII filter as
        # above; aapt rejects non-ASCII names.
        #
        $WallpapersDir = Join-Path $DemoWindowsDir 'wallpapers'
        if (Test-Path $WallpapersDir)
        {
            $WallpapersStage = Join-Path $AssetsStage 'wallpapers'
            New-Item -ItemType Directory -Force -Path $WallpapersStage | Out-Null
            foreach ($pat in @('*.png', '*.jpg', '*.jpeg'))
            {
                foreach ($f in Get-ChildItem -Path $WallpapersDir -Filter $pat -ErrorAction SilentlyContinue)
                {
                    $isAscii = $true
                    foreach ($ch in $f.Name.ToCharArray())
                    {
                        if ([int]$ch -gt 127) { $isAscii = $false; break }
                    }
                    if (-not $isAscii) { continue }
                    Copy-Item -Path $f.FullName -Destination $WallpapersStage -Force
                }
            }
            Info ("staged wallpapers/ from demo-windows -> " + $WallpapersStage)
        }
    }

    #
    # Stage TTF fonts from gui/src/fonts/ into assets/fonts/ so the
    # Android text pipeline has something to rasterize. platform_android
    # enumerates this directory at startup via AAssetManager_openDir
    # and calls font__register_from_file for each .ttf, using the
    # filename stem as the family name (same convention as the Windows
    # demo's font__init directory scan of GUI_FONTS_SOURCE_DIR).
    #
    $SourceFonts = Join-Path $Root 'gui\src\fonts'
    if (Test-Path $SourceFonts)
    {
        $FontsStage = Join-Path $AssetsStage 'fonts'
        New-Item -ItemType Directory -Force -Path $FontsStage | Out-Null
        foreach ($f in Get-ChildItem -Path $SourceFonts -Filter '*.ttf' -ErrorAction SilentlyContinue)
        {
            Copy-Item -Path $f.FullName -Destination $FontsStage -Force
            Info ("staged font " + $f.Name + " -> " + $FontsStage)
        }
    }

    # Compile each ABI. Flags mirror the Makefile's per-arch settings.
    Build-So $ClangArm64  'arm64-v8a'    @('-m64')
    Build-So $ClangArm32  'armeabi-v7a'  @('-mfloat-abi=softfp', '-m32')
    Build-So $ClangX86    'x86'          @('-march=i686', '-mssse3', '-mfpmath=sse', '-m32')
    Build-So $ClangX86_64 'x86_64'       @('-march=x86-64', '-msse4.2', '-mpopcnt', '-m64')

    # Package manifest + assets into a bare APK (no .so yet). aapt handles
    # the resources.arsc / AndroidManifest.xml compression rules itself.
    $TempApk = Join-Path $BuildDir 'temp.apk'
    if (Test-Path $TempApk) { Remove-Item $TempApk -Force }

    Info "aapt package (manifest + assets)"
    Invoke-Native $Aapt @(
        'package',
        '-f',
        '-F', $TempApk,
        '-I', $AndroidJar,
        '-M', $ManifestOut,
        '-A', (Join-Path $MakecapkDir 'assets'),
        '--target-sdk-version', "$AndroidTarget"
    )

    # Add the .so files. aapt stores native libs uncompressed (required so
    # the runtime loader can mmap them directly on API 23+).
    #
    # aapt add's archive path is relative to the current working directory,
    # so cd into makecapk/ for this stretch.
    Push-Location $MakecapkDir
    try {
        foreach ($abi in @('arm64-v8a','armeabi-v7a','x86','x86_64')) {
            $rel = "lib/$abi/lib$AppName.so"
            Info "aapt add $rel"
            Invoke-Native $Aapt @('add', '-v', $TempApk, $rel)
        }
    } finally { Pop-Location }

    # zipalign to 4-byte boundary (mandatory before v2 signing on newer SDKs).
    if (Test-Path $ApkFinal) { Remove-Item $ApkFinal -Force }
    Info "zipalign"
    Invoke-Native $Zipalign @('-v', '4', $TempApk, $ApkFinal)

    # v2 sign. apksigner ships as a .bat wrapper that invokes the .jar;
    # we invoke the .bat directly rather than chase the .jar path.
    Info "apksigner sign"
    Invoke-Native $ApkSigner @(
        'sign',
        '--ks', $KeystorePath,
        '--ks-pass', "pass:$StorePass",
        '--key-pass', "pass:$StorePass",
        $ApkFinal
    )

    Remove-Item $TempApk -Force -ErrorAction SilentlyContinue
    Ok "signed APK: $ApkFinal"
    $size = (Get-Item $ApkFinal).Length
    Info ("size: {0:N0} bytes" -f $size)
}

function Target-Clean {
    foreach ($p in @($BuildDir, $ApkFinal, (Join-Path $ScriptDir 'output.map'))) {
        if (Test-Path $p) {
            Info "removing $p"
            Remove-Item $p -Recurse -Force
        }
    }
    Ok 'clean done'
}

function Target-Push {
    #
    # Always rebuild before pushing -- Target-Build auto-cleans, so without
    # this the phone could get a stale APK if a previous $APK_FINAL still
    # exists on disk. The cost of an unconditional rebuild is a few seconds
    # of clang; the alternative (a half-deployed app) is confusing.
    #
    Target-Build
    if (-not $Adb) { throw 'adb.exe not found. Pass -Sdk with a valid platform-tools.' }
    Info "adb install -r $ApkFinal"
    Invoke-Native $Adb @('install', '-r', $ApkFinal)
}

function Target-Run {
    Target-Push
    $launchActivity = "$PackageName/android.app.NativeActivity"
    Info "adb shell am start -n $launchActivity"
    Invoke-Native $Adb @('shell', 'am', 'start', '-n', $launchActivity)
}

function Target-Uninstall {
    if (-not $Adb) { throw 'adb.exe not found.' }
    Info "adb uninstall $PackageName"
    & $Adb uninstall $PackageName
}

# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------

switch ($Target.ToLower()) {
    'run'       { Target-Run }        # default: build + install + launch
    ''          { Target-Run }
    'build'     { Target-Build }      # build APK only, no device required
    'apk'       { Target-Build }
    'push'      { Target-Push }       # build + install (no launch)
    'testsdk'   { Target-TestSdk }
    'clean'     { Target-Clean }
    'manifest'  { Target-Manifest }
    'keystore'  { Target-Keystore }
    'uninstall' { Target-Uninstall }
    default     {
        Err "Unknown target: $Target"
        Write-Host "Valid targets: run (default), build, push, testsdk, clean, manifest, uninstall"
        exit 2
    }
}
