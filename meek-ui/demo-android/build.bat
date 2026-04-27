@echo off
setlocal

rem ============================================================================
rem build.bat - thin launcher for build.ps1.
rem ============================================================================
rem
rem Runs the native-Windows PowerShell build script, forwarding all args.
rem No MSYS2 / WSL / bash / make required -- just the MAUI / Android Studio
rem toolchain you already have (SDK + NDK + JDK).
rem
rem Examples:
rem   build.bat                     build the APK
rem   build.bat testsdk             print discovered SDK / NDK / JDK paths
rem   build.bat run                 push + launch on attached device
rem   build.bat clean
rem   build.bat -AppName myapp -PackageName com.me.myapp
rem ============================================================================

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
