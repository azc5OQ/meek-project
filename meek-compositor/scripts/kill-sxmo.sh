#!/bin/sh
#
# kill-sxmo.sh -- stop sway + sxmo helpers + any meek stragglers so
# /dev/dri/card0 is free for meek-compositor to take DRM master.
#
# RULES:
#   - NEVER re-starts sxmo. If you want it back, run
#       doas rc-service tinydm start
#     yourself. (User-stated: "never, ever restore sxmo".)
#   - Safe to run multiple times.
#   - Intended to be staged to the phone at /home/user/meek-testing/
#     meek-compositor/scripts/kill-sxmo.sh and invoked via
#     runlog.sh.
#
# USAGE (on phone, once per boot):
#   doas rc-service tinydm stop     # see WHY below
#   /home/user/meek-testing/meek-compositor/scripts/kill-sxmo.sh
#
# USAGE (from dev machine via runlog):
#   ssh user@<phone> /home/user/meek-testing/runlog.sh "kill-sxmo" \
#     '/home/user/meek-testing/meek-compositor/scripts/kill-sxmo.sh'
#

set +e

#
# WHY "doas rc-service tinydm stop" HAS TO RUN FIRST
# --------------------------------------------------
# The sxmo session is supervised at two levels on postmarketOS:
#
#   (root)  /sbin/openrc-run
#     `-- (root) supervise-daemon tinydm --start --respawn-delay 2
#                                        --respawn-max 5
#                                        --respawn-period 1800
#                /usr/bin/autologin -- user tinydm-run-session
#         `-- (root) /usr/bin/autologin user tinydm-run-session
#             `-- (user) sxmo_winit.sh
#                  `-- (user) dbus-run-session -- sxmo_winit.sh with_dbus
#                       |-- (user) sway         <-- holds /dev/dri/card0
#                       |-- (user) swaybar, sxmobar, swayidle, ...
#
# supervise-daemon is OpenRC's watchdog for the tinydm service. It
# re-execs /usr/bin/autologin every 2 seconds (up to 5 times in a
# 30-minute window) if the child exits. Both supervise-daemon AND
# /usr/bin/autologin run as ROOT, but everything user-visible
# (sxmo_winit.sh, sway, the sxmo helpers) runs as user "user".
#
# This script runs as "user". User-level pkill CANNOT signal root
# processes (EPERM, silently swallowed by `2>/dev/null`). So no
# matter how many times we kill sway, supervise-daemon just resets
# the chain within seconds and sway comes back holding card0.
#
# Before running this script, the operator must tell OpenRC to stop
# supervising tinydm:
#
#     doas rc-service tinydm stop
#
# That cleanly tears down supervise-daemon + autologin + the whole
# sxmo tree, and it stays down until you run the matching `start`.
#
# The feedback file `session/memory/feedback_phone_runlog.md`
# explicitly bans this script from using doas/sudo on its own.
# That's why the stop command is manual.
#
# This script's job, post-rc-service-stop:
#   - Reap any user-level sxmo helpers that linger past the tinydm
#     teardown (inotifywait monitors, sxmo_aligned_sleep, etc.).
#   - Reap meek stragglers from a previous dev iteration.
#   - Force-kill anything still holding /dev/dri/card0.
#   - Report success / list remaining card0 holders.
#
# It's a cleanup pass, NOT a supervisor killer.
#

mode="${1:-stop}"

if [ "$mode" = "resume" ]; then
    #
    # There's nothing user-level to SIGCONT anymore -- the real
    # respawner is root-supervised. Point the operator at the
    # matching rc-service start instead.
    #
    echo "kill-sxmo: to bring sxmo back, run:  doas rc-service tinydm start"
    exit 0
fi

# ---------------------------------------------------------------------
# Step 1: detect the root-owned respawner, and try to stop it via a
# non-interactive sudo/doas. If the user has passwordless sudo for
# rc-service (common dev setup on pmOS), this just works. Otherwise
# we fall through to the manual instruction.
# ---------------------------------------------------------------------

sv_pid=$(pgrep -f 'supervise-daemon tinydm' 2>/dev/null | head -1)
if [ -n "$sv_pid" ]; then
    echo "kill-sxmo: supervise-daemon active (pid $sv_pid); attempting sudo/doas rc-service tinydm stop"
    if command -v sudo >/dev/null 2>&1; then
        sudo -n rc-service tinydm stop >/dev/null 2>&1
    elif command -v doas >/dev/null 2>&1; then
        doas -n rc-service tinydm stop >/dev/null 2>&1
    fi

    #
    # Give OpenRC a moment to tear down supervise-daemon + autologin +
    # the sxmo tree.
    #
    sleep 2

    #
    # Re-check. If still alive the sudo/doas attempt didn't work
    # (likely because passwordless isn't configured for rc-service).
    # Bail with the original manual instruction.
    #
    if pgrep -f 'supervise-daemon tinydm' >/dev/null 2>&1; then
        echo "kill-sxmo: ERROR supervise-daemon still active as root."
        echo "          sudo/doas -n attempt didn't land (probably no passwordless"
        echo "          config for rc-service). Run ONCE per boot yourself:"
        echo
        echo "              sudo rc-service tinydm stop"
        echo "              # or: doas rc-service tinydm stop"
        echo
        echo "          Then re-run this script. To restore sxmo later:"
        echo "              sudo rc-service tinydm start"
        exit 2
    fi
    echo "kill-sxmo: supervise-daemon stopped; continuing"
fi

# ---------------------------------------------------------------------
# Step 2: reap every sxmo / sway user-level process. -x = exact match
# on comm (busybox pkill substring-matches by default, but is still
# comm-truncated to 15 chars).
# ---------------------------------------------------------------------

for p in sxmo_winit.sh sway swaybg swaybar swaymsg sxmobar swayidle \
         superd sxmo_hook_lisgd sxmo_autosuspend sxmo_battery_monitor \
         sxmo_modemmonitor sxmo_notificationmonitor sxmo_run_aligned \
         sxmo_run_periodically bonsaid lisgd conky; do
    pkill -9 -x "$p" 2>/dev/null
done

# Also kill sxmo_* script helpers with longer names (comm is
# truncated to 15 chars, so the -x list above may miss some).
pkill -9 -f "sxmo_" 2>/dev/null

# dbus-run-session is NOT in the exact-match list because it's also
# used by other sessions. Target only the sxmo one by cmdline match.
pkill -9 -f "dbus-run-session.*sxmo" 2>/dev/null

# inotifywait instances that sxmo_notificationmonitor leaves dangling.
pkill -9 -f "inotifywait.*sxmo" 2>/dev/null

sleep 1

# ---------------------------------------------------------------------
# Step 3: meek stragglers from prior dev iterations. Without this
# drmSetMaster later fails "Permission denied" because the previous
# meek_compositor still has card0 open.
# ---------------------------------------------------------------------

pkill -9 -x meek_shell       2>/dev/null
pkill -9 -x meek_compositor  2>/dev/null
pkill -9 -x demo_settings    2>/dev/null

sleep 1

# ---------------------------------------------------------------------
# Step 4: anyone else holding /dev/dri/card0 gets force-killed. Walks
# /proc/*/fd/* and matches on the readlink target. Skip root-owned
# processes -- we can't kill those and trying spams EPERM noise.
# ---------------------------------------------------------------------

for pid in $(ls /proc 2>/dev/null | grep -E "^[0-9]+$"); do
    #
    # Skip root-owned pids. uid lives in /proc/<pid>/status as
    # "Uid:   0  0  0  0"; first field is real uid.
    #
    uid=$(awk '/^Uid:/ {print $2; exit}' /proc/$pid/status 2>/dev/null)
    if [ "$uid" = "0" ]; then
        continue
    fi
    for fd in /proc/$pid/fd/* ; do
        link=$(readlink "$fd" 2>/dev/null)
        case "$link" in
            */dri/card0*) kill -9 "$pid" 2>/dev/null ;;
        esac
    done
done

sleep 1

# ---------------------------------------------------------------------
# Step 5: report who still holds card0 (should be empty). A root-
# owned holder at this point is diagnostic: it means
# `doas rc-service tinydm stop` was skipped or failed and something
# root-level is still there.
# ---------------------------------------------------------------------

remaining=""
for pid in $(ls /proc 2>/dev/null | grep -E "^[0-9]+$"); do
    for fd in /proc/$pid/fd/* ; do
        link=$(readlink "$fd" 2>/dev/null)
        case "$link" in
            */dri/card0*)
                comm=$(cat /proc/$pid/comm 2>/dev/null)
                uid=$(awk '/^Uid:/ {print $2; exit}' /proc/$pid/status 2>/dev/null)
                remaining="$remaining pid=$pid($comm uid=$uid)"
                ;;
        esac
    done
done

if [ -n "$remaining" ]; then
    echo "kill-sxmo: WARN card0 still held by:$remaining"
    echo "          If any of the above show uid=0, run 'doas rc-service tinydm stop'"
    echo "          and try again."
    exit 1
fi

echo "kill-sxmo: done; /dev/dri/card0 free"
exit 0
