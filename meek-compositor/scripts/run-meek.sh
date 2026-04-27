#!/bin/sh
#
# run-meek.sh -- stop any running meek_compositor / meek_shell /
# demo_settings, then relaunch the full stack fresh. Saves pids to
# /tmp/*.pid so the next invocation can kill cleanly even if pkill
# name-matching is unreliable (busybox pkill + argument ordering
# are subtle).
#
# USAGE (on phone):
#   /home/user/meek-testing/run-meek.sh
#
# USAGE (from dev machine):
#   ssh user@<phone> /home/user/meek-testing/runlog.sh "relaunch" \
#     '/home/user/meek-testing/run-meek.sh'
#
# Requires: /dev/dri/card0 must be free. Run kill-sxmo.sh first if
# sxmo / sway is still holding it.
#

set +e

ROOT=/home/user/meek-testing
XDG=/dev/shm/user/10000

# ------------------------------------------------------------------
# Step 1: stop anything already running. Try pidfile first (precise),
# then fall back to pattern-match (catches leaks from earlier runs).
# ------------------------------------------------------------------

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
    # pidof is more reliable than busybox pkill for exact-name match.
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

sleep 2

# Sanity check -- nothing should remain.
survivors=$(pidof meek_compositor meek_shell demo_settings 2>/dev/null)
if [ -n "$survivors" ]; then
    echo "run-meek: WARN survivors still alive: $survivors"
fi

# ------------------------------------------------------------------
# Step 2: fresh socket, launch stack.
# ------------------------------------------------------------------

mkdir -p "$XDG"
rm -f "$XDG"/wayland-* 2>/dev/null

cd "$ROOT" || exit 1

nohup env XDG_RUNTIME_DIR=$XDG ./meek-compositor/meek_compositor \
    > /tmp/comp.log 2>&1 &
COMP=$!
echo $COMP > /tmp/comp.pid
sleep 3

SOCKET=$(grep -oE 'WAYLAND_DISPLAY=wayland-[0-9]+' /tmp/comp.log | head -1 | cut -d= -f2)
if [ -z "$SOCKET" ]; then
    echo "run-meek: FAIL compositor did not bind a wayland socket"
    tail -10 /tmp/comp.log
    exit 1
fi

#
# Phase 2 render gating is on by default. To revert to pre-Phase-2
# always-render behaviour for debugging, set
# MEEK_RENDER_GATING=off in this env block. The gate is implemented
# in meek-ui's platform_linux_wayland_client.c.
#
nohup env XDG_RUNTIME_DIR=$XDG WAYLAND_DISPLAY=$SOCKET \
    ./meek-shell/meek_shell > /tmp/shell.log 2>&1 &
SHELL_PID=$!
echo $SHELL_PID > /tmp/shell.pid
sleep 2

nohup env XDG_RUNTIME_DIR=$XDG WAYLAND_DISPLAY=$SOCKET \
    ./meek-shell/demo-settings/demo_settings > /tmp/ds.log 2>&1 &
DS_PID=$!
echo $DS_PID > /tmp/ds.pid
sleep 3

echo "run-meek: OK"
echo "  compositor: pid=$COMP       log=/tmp/comp.log   socket=$SOCKET"
echo "  shell:      pid=$SHELL_PID  log=/tmp/shell.log"
echo "  demo:       pid=$DS_PID     log=/tmp/ds.log"

# Final proc list (should be exactly 3 meek-ish processes).
echo
echo "running:"
ps -eo pid,comm | grep -E 'meek_compositor|meek_shell|demo_settings'

exit 0
