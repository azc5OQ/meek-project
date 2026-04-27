@echo off
:: =============================================================================
:: deploy.bat - push meek-compositor sources to a Linux SSH target and build.
:: =============================================================================
::
:: libwayland-server has no Windows port, so we develop on Windows
:: and build + run on an actual Linux device over SSH. This script
:: rsyncs the repo to the target and invokes build.sh there.
::
:: Required environment variables (set them yourself or via a local
:: set_env.bat that you don't commit):
::   MEEK_SSH_HOST      - hostname or IP of the Linux target
::   MEEK_SSH_USER      - username on the target
::   MEEK_REMOTE_PATH   - absolute path on the target for the repo
::                        (e.g. /home/you/src/meek-compositor)
::
:: Optional:
::   MEEK_SSH_PORT      - ssh port (defaults to 22)
::   MEEK_BUILD_TARGET  - passed to build.sh (default: build)
::                        valid: build / run / clean / deps
::
:: Dependencies on the Windows side: rsync + ssh available on PATH.
:: On modern Windows these come with OpenSSH + Git for Windows. On
:: older boxes install git-for-windows which bundles both.
::
:: See session/toolchain_paths.md for setup details.

setlocal EnableDelayedExpansion

if "%MEEK_SSH_HOST%"=="" goto :missing_env
if "%MEEK_SSH_USER%"=="" goto :missing_env
if "%MEEK_REMOTE_PATH%"=="" goto :missing_env

if "%MEEK_SSH_PORT%"=="" set MEEK_SSH_PORT=22
if "%MEEK_BUILD_TARGET%"=="" set MEEK_BUILD_TARGET=build

set SCRIPT_DIR=%~dp0
set SCRIPT_DIR=%SCRIPT_DIR:~0,-1%

echo [deploy] target: %MEEK_SSH_USER%@%MEEK_SSH_HOST%:%MEEK_REMOTE_PATH%
echo [deploy] port:   %MEEK_SSH_PORT%
echo [deploy] build:  %MEEK_BUILD_TARGET%

:: Ensure remote path exists.
ssh -p %MEEK_SSH_PORT% %MEEK_SSH_USER%@%MEEK_SSH_HOST% "mkdir -p %MEEK_REMOTE_PATH%"
if errorlevel 1 goto :ssh_failed

:: rsync the compositor sources. Exclude build artifacts and vendored
:: inspiration tarballs. We also push the sibling meek-ui dir so the
:: target has a consistent layout for the #include "types.h" path.
::
:: --delete keeps the remote tree clean; --exclude lists match
:: .gitignore-ish patterns we don't want to push.
rsync -az --delete ^
  --exclude=build/ ^
  --exclude=inspiration/ ^
  --exclude=.git/ ^
  --exclude=*.o ^
  --exclude=meek_compositor ^
  -e "ssh -p %MEEK_SSH_PORT%" ^
  "%SCRIPT_DIR%/" "%MEEK_SSH_USER%@%MEEK_SSH_HOST%:%MEEK_REMOTE_PATH%/"
if errorlevel 1 goto :rsync_failed

:: Also push meek-ui (next to the compositor dir on the target).
:: Only types.h is needed in this pass but pushing the full tree
:: keeps us honest for subsequent passes.
rsync -az --delete ^
  --exclude=build/ ^
  --exclude=.git/ ^
  --exclude=*.o ^
  -e "ssh -p %MEEK_SSH_PORT%" ^
  "%SCRIPT_DIR%/../meek-ui/" "%MEEK_SSH_USER%@%MEEK_SSH_HOST%:%MEEK_REMOTE_PATH%/../meek-ui/"
if errorlevel 1 goto :rsync_failed

echo [deploy] sources pushed
echo [deploy] running build.sh %MEEK_BUILD_TARGET% on target

ssh -p %MEEK_SSH_PORT% %MEEK_SSH_USER%@%MEEK_SSH_HOST% ^
  "cd %MEEK_REMOTE_PATH% && chmod +x build.sh && ./build.sh %MEEK_BUILD_TARGET%"
if errorlevel 1 goto :build_failed

echo [deploy] done
endlocal
exit /b 0

:missing_env
echo [deploy] ERROR: required environment variables not set.
echo.
echo   MEEK_SSH_HOST      = %MEEK_SSH_HOST%
echo   MEEK_SSH_USER      = %MEEK_SSH_USER%
echo   MEEK_REMOTE_PATH   = %MEEK_REMOTE_PATH%
echo.
echo Set them before running deploy.bat. Example:
echo   set MEEK_SSH_HOST=192.168.1.50
echo   set MEEK_SSH_USER=you
echo   set MEEK_REMOTE_PATH=/home/you/src/meek-compositor
echo.
echo See session/toolchain_paths.md for details.
endlocal
exit /b 2

:ssh_failed
echo [deploy] ERROR: ssh to %MEEK_SSH_USER%@%MEEK_SSH_HOST%:%MEEK_SSH_PORT% failed.
echo Is the host reachable? Is key auth set up? Try manually:
echo   ssh -p %MEEK_SSH_PORT% %MEEK_SSH_USER%@%MEEK_SSH_HOST% whoami
endlocal
exit /b 3

:rsync_failed
echo [deploy] ERROR: rsync failed. Is rsync installed on both ends?
echo   windows: install git-for-windows or "rsync" via scoop/choco.
echo   linux:   apt install rsync / dnf install rsync / pacman -S rsync.
endlocal
exit /b 4

:build_failed
echo [deploy] ERROR: remote build.sh %MEEK_BUILD_TARGET% failed.
echo The target's build output is above. Common causes:
echo   - missing libwayland-dev (try: ./build.sh deps on the target)
echo   - clang not installed on target
endlocal
exit /b 5
