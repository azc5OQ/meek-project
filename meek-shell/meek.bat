@echo off
setlocal enabledelayedexpansion
REM ============================================================================
REM meek.bat -- one-shot sync + build + launch driver for the meek stack.
REM
REM Lives at G:\claude\project\meek-shell\meek.bat. Operates on the three
REM project trees one level up: meek-ui, meek-shell, meek-compositor.
REM
REM Default action with no args: kill stale processes, sync sources to the
REM phone, rebuild meek-shell, launch the run-meek.sh stack. Run after
REM editing any .c / .h / .ui / .style on Windows -- the latest is on the
REM phone in seconds. Double-click works (window pauses at the end so you
REM can read the output).
REM
REM Subcommands:
REM   meek            -- kill + sync + build + run (the everyday case)
REM   meek sync       -- copy sources only, no build
REM   meek build      -- rebuild meek-shell on the phone, no sync
REM   meek run        -- start the stack (after a previous build)
REM   meek kill       -- pkill every meek_* + demo_settings + foot
REM   meek logs       -- tail shell.log + comp.log on the phone (Ctrl+C exits)
REM   meek shot       -- capture screenshot, save to meek-compositor\images\
REM   meek ip <addr>  -- run with a one-off PHONE override
REM
REM Phone address: defaults to user@192.168.1.104. Override per run via
REM   set PHONE=user@10.0.0.42 ^&^& meek
REM Or persistently by editing the SET PHONE= line below.
REM
REM Requirements:
REM   ssh.exe + scp.exe on PATH (Windows 10+ ships them via OpenSSH).
REM   bash on PATH (sync-to-phone.sh is bash). Falls back to
REM   "C:\Program Files\Git\bin\bash.exe" if `bash` isn't on PATH.
REM ============================================================================

if not defined PHONE       set PHONE=user@192.168.1.104
if not defined REMOTE_ROOT set REMOTE_ROOT=/home/user/meek-testing

REM Project root is the parent of this .bat's directory.
set PROJECT_ROOT=%~dp0..
for %%I in ("%PROJECT_ROOT%") do set PROJECT_ROOT=%%~fI

REM Resolve bash. Prefer whatever is on PATH; fall back to Git for Windows.
where bash >nul 2>nul
if errorlevel 1 (
    set "BASH=C:\Program Files\Git\bin\bash.exe"
) else (
    set "BASH=bash"
)

set CMD=%~1
if "%CMD%"=="" set CMD=all

REM `meek ip <addr>` overrides PHONE for this invocation, then runs the
REM default `all` flow.
if /i "%CMD%"=="ip" (
    if "%~2"=="" (
        echo error: meek ip requires an address, e.g. meek ip user@192.168.1.50
        goto :error_end
    )
    set PHONE=%~2
    echo PHONE override active: %PHONE%
    set CMD=all
)

REM Dispatch. Each `call :do_*` invokes a subroutine that ends with
REM `goto :eof` (i.e. returns to caller). The bare-goto form would exit
REM the whole script, which broke earlier multi-step commands.
if /i "%CMD%"=="sync"  ( call :do_sync   & if errorlevel 1 goto :error_end & goto :pause_end )
if /i "%CMD%"=="build" ( call :do_build  & if errorlevel 1 goto :error_end & goto :pause_end )
if /i "%CMD%"=="run"   ( call :do_run    & if errorlevel 1 goto :error_end & goto :pause_end )
if /i "%CMD%"=="kill"  ( call :do_kill   & if errorlevel 1 goto :error_end & goto :pause_end )
if /i "%CMD%"=="logs"  ( call :do_logs   & goto :pause_end )
if /i "%CMD%"=="shot"  ( call :do_shot   & if errorlevel 1 goto :error_end & goto :pause_end )
if /i "%CMD%"=="all"   ( call :do_all    & if errorlevel 1 goto :error_end & goto :pause_end )

echo unknown command: %CMD%
echo usage: meek [sync^|build^|run^|kill^|logs^|shot^|all^|ip ^<addr^>]
goto :error_end


REM ============================================================================
REM Subroutines. Each ends with `goto :eof` to return to its caller cleanly.
REM ============================================================================

:do_all
echo === phone: %PHONE% ===
call :do_kill_silent
call :do_sync  || exit /b 1
call :do_build || exit /b 1
call :do_run   || exit /b 1
goto :eof


:do_sync
echo === sync ===
"%BASH%" "%~dp0sync-to-phone.sh" meek-shell meek-ui meek-compositor
if errorlevel 1 (
    echo sync failed.
    exit /b 1
)
goto :eof


:do_build
echo === build ===
ssh -o ConnectTimeout=20 %PHONE% "%REMOTE_ROOT%/runlog.sh rebuild 'cd %REMOTE_ROOT%/meek-shell && rm -f meek_shell && bash build.sh 2>&1 | tail -12'"
if errorlevel 1 (
    echo build failed.
    exit /b 1
)
goto :eof


:do_run
echo === run ===
ssh -o ConnectTimeout=20 %PHONE% "%REMOTE_ROOT%/runlog.sh launch %REMOTE_ROOT%/meek-compositor/scripts/run-meek.sh"
if errorlevel 1 (
    echo run failed.
    exit /b 1
)
echo stack launched. Tap an app icon on the phone, or run 'meek logs' to tail.
goto :eof


:do_kill
echo === kill ===
call :do_kill_silent
goto :eof


:do_kill_silent
ssh -o ConnectTimeout=20 %PHONE% "%REMOTE_ROOT%/runlog.sh kill-all 'pkill -9 -x meek_shell; pkill -9 -x meek_compositor; pkill -9 -x demo_settings; pkill -9 -x foot; sleep 1'" >nul 2>nul
goto :eof


:do_logs
echo === logs (Ctrl+C to exit) ===
ssh -o ConnectTimeout=20 %PHONE% "tail -F /tmp/shell.log /tmp/comp.log"
goto :eof


:do_shot
echo === screenshot ===
for /f "tokens=*" %%i in ('powershell -NoProfile -Command "Get-Date -AsUTC -Format 'yyyy-MM-dd_HHmmss'"') do set TS=%%i
set "OUT=%PROJECT_ROOT%\meek-compositor\images\%TS%__shot.png"
ssh -o ConnectTimeout=20 %PHONE% "%REMOTE_ROOT%/runlog.sh shot 'kill -USR1 $(pidof meek_compositor); sleep 1; ls -la /tmp/meek-screenshot.ppm'"
scp -o ConnectTimeout=20 %PHONE%:/tmp/meek-screenshot.ppm "%TEMP%\meek-shot.ppm"
if errorlevel 1 (
    echo scp failed.
    exit /b 1
)
where ffmpeg >nul 2>nul
if errorlevel 1 (
    set "FFMPEG=D:\ffmpeg-master-latest-win64-gpl-shared\bin\ffmpeg.exe"
) else (
    set "FFMPEG=ffmpeg"
)
"%FFMPEG%" -y -loglevel error -i "%TEMP%\meek-shot.ppm" "%OUT%"
del /q "%TEMP%\meek-shot.ppm" 2>nul
echo saved: %OUT%
goto :eof


REM ============================================================================
REM Exit paths. pause keeps the window open when double-clicked.
REM ============================================================================

:pause_end
echo.
echo done.
pause
endlocal
exit /b 0

:error_end
echo.
echo failed -- see output above.
pause
endlocal
exit /b 1
