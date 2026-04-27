#!/bin/sh
#
# run-meek-gated.sh -- launch meek stack with configurable
# MEEK_FRACTIONAL_SCALE for amberol A/B bisection. Mirrors
# run-meek.sh but passes the env var through. First arg is the
# value (none/fs/vp/both); default "both".
#
set +e

ROOT=/home/user/meek-testing
XDG=/dev/shm/user/10000
FS=${1:-both}

stop_by_pid() {
    if [ -f "$1" ]; then
        pid=$(cat "$1" 2>/dev/null)
        if [ -n "$pid" ]; then
            kill -9 "$pid" 2>/dev/null
        fi
        rm -f "$1"
    fi
}
stop_by_name() {
    pids=$(pidof "$1" 2>/dev/null)
    if [ -n "$pids" ]; then
        for p in $pids; do
            kill -9 "$p" 2>/dev/null
        done
    fi
}

stop_by_pid  /tmp/comp.pid
stop_by_pid  /tmp/shell.pid
stop_by_pid  /tmp/ds.pid
stop_by_name meek_compositor
stop_by_name meek_shell
stop_by_name demo_settings
stop_by_name amberol
sleep 2

mkdir -p "$XDG"
rm -f "$XDG"/wayland-* 2>/dev/null

cd "$ROOT" || exit 1

nohup env XDG_RUNTIME_DIR=$XDG MEEK_FRACTIONAL_SCALE=$FS ./meek-compositor/meek_compositor \
    > /tmp/comp.log 2>&1 &
COMP=$!
echo $COMP > /tmp/comp.pid
sleep 3

SOCKET=$(grep -oE 'WAYLAND_DISPLAY=wayland-[0-9]+' /tmp/comp.log | head -1 | cut -d= -f2)
if [ -z "$SOCKET" ]; then
    echo "run-meek-gated: FAIL compositor did not bind a wayland socket"
    tail -10 /tmp/comp.log
    exit 1
fi

nohup env XDG_RUNTIME_DIR=$XDG WAYLAND_DISPLAY=$SOCKET \
    ./meek-shell/meek_shell > /tmp/shell.log 2>&1 &
SHELL_PID=$!
echo $SHELL_PID > /tmp/shell.pid
sleep 2

nohup env XDG_RUNTIME_DIR=$XDG WAYLAND_DISPLAY=$SOCKET \
    ./meek-shell/demo-settings/demo_settings > /tmp/ds.log 2>&1 &
DS_PID=$!
echo $DS_PID > /tmp/ds.pid
sleep 2

echo "run-meek-gated: OK MEEK_FRACTIONAL_SCALE=$FS"
echo "  compositor: pid=$COMP   log=/tmp/comp.log   socket=$SOCKET"
echo "  shell:      pid=$SHELL_PID  log=/tmp/shell.log"
echo "  demo:       pid=$DS_PID     log=/tmp/ds.log"
echo
grep -E "Level-1 scale globals" /tmp/comp.log | tail -1
exit 0
