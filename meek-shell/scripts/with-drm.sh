#!/usr/bin/env bash
# =============================================================================
# scripts/with-drm.sh -- orchestrate stop-sway / run-drm / restart-sway
# for testing meek_shell_drm on a device whose host compositor is sway
# (via sxmo or otherwise).
# =============================================================================
#
# USAGE (run from the meek-shell checkout root or from anywhere --
# the script resolves its own paths):
#
#   ./scripts/with-drm.sh stop         # stop sway (sends SIGTERM; waits for
#                                      # DRM master release)
#   ./scripts/with-drm.sh run          # launch meek_shell_drm (requires sway
#                                      # already stopped + perms to take DRM
#                                      # master -- typically sudo)
#   ./scripts/with-drm.sh restart      # try to restart sway via whatever
#                                      # service manager is in use; falls
#                                      # back to printing the command you'd
#                                      # run manually
#   ./scripts/with-drm.sh              # with no args: prints this usage
#
# DESIGN NOTES:
#
# Each step is a separate explicit subcommand by design -- I don't
# want to pkill sway automatically on any default invocation. sxmo
# is the user's live session; stopping it is destructive to their
# open work and the user should be the one asking for it.
#
# DRM master: meek_shell_drm needs to become DRM master on
# /dev/dri/card0. On pmOS that typically requires either:
#   - running as root (sudo ./meek_shell_drm), OR
#   - being in the 'video' group AND having a seat session that
#     holds the logind inhibit, OR
#   - launching via libseat (not yet wired; F3+).
# For the F1 sketch we just use sudo. Later passes can replace
# with libseat-based privilege drop.
#
# RECOVERY: if something goes wrong and you're stuck on a black
# screen with no sway and no meek_shell_drm, SSH in and run
#   sudo reboot
# Reboot will bring sxmo back up cleanly. That's the escape hatch.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHELL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$SHELL_DIR/meek_shell_drm"

info() { printf '\033[36m[with-drm]\033[0m %s\n' "$*"; }
ok()   { printf '\033[32m[with-drm]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[with-drm]\033[0m %s\n' "$*"; }
err()  { printf '\033[31m[with-drm]\033[0m %s\n' "$*" >&2; }

sub=${1:-help}

cmd_stop() {
    if pgrep -x sway >/dev/null 2>&1; then
        info "sending SIGTERM to sway..."
        pkill -TERM -x sway || true
        # Give sway up to 3 seconds to exit cleanly and drop DRM
        # master. Without this pause, the next drmSetMaster() would
        # race and usually fail with EBUSY.
        for _ in 1 2 3; do
            sleep 1
            pgrep -x sway >/dev/null 2>&1 || { ok "sway stopped"; return 0; }
        done
        warn "sway still running after 3s; consider 'pkill -KILL sway' manually"
        return 1
    else
        info "sway not running (nothing to stop)"
    fi
}

cmd_run() {
    if [[ ! -x "$BIN" ]]; then
        err "$BIN not found or not executable"
        err "build it first:  PLATFORM=drm ./build.sh build"
        exit 1
    fi
    if pgrep -x sway >/dev/null 2>&1; then
        err "sway is still running; can't take DRM master"
        err "run:  $0 stop"
        exit 1
    fi
    info "launching $BIN (Ctrl-C to exit; needs DRM master perms)"
    # We don't blindly prepend sudo -- the user may have set up
    # udev/groups to run DRM master-capable as an unprivileged user.
    # If $BIN exits immediately with "drmSetMaster: Permission
    # denied", the user knows to retry with sudo.
    "$BIN"
}

cmd_restart() {
    info "attempting to restart the host compositor..."
    #
    # pmOS + sxmo uses openrc; the service name varies (sxmo, sxmo-
    # sway, sway...) depending on the install. Probe the most
    # common names; fall back to telling the user what to do.
    #
    for svc in sxmo sxmo-sway sway; do
        if command -v rc-service >/dev/null 2>&1 \
           && rc-service --exists "$svc" >/dev/null 2>&1; then
            info "starting openrc service: $svc"
            sudo rc-service "$svc" start
            ok "requested '$svc' start; give it a few seconds"
            return 0
        fi
    done
    warn "no recognized sway/sxmo openrc service found."
    warn "if your sxmo is tty-autostart-based, just switch to that tty"
    warn "or reboot (sudo reboot) to pick it back up."
}

cmd_help() {
    cat <<EOF
with-drm.sh -- test meek_shell_drm against the device's physical panel.

USAGE:
  $0 stop       # stop sway (sends SIGTERM)
  $0 run        # launch meek_shell_drm (sway must be stopped first)
  $0 restart    # try to restart sway
  $0 help       # this message

Typical flow:
  $0 stop
  sudo $0 run          # or: sudo ./meek_shell_drm directly
  # Ctrl-C to exit when you've seen enough
  $0 restart           # or sudo reboot

If the phone is stuck on a black screen with nothing running,
SSH in and 'sudo reboot' -- sxmo will come back up cleanly.
EOF
}

case "$sub" in
    stop)    cmd_stop ;;
    run)     cmd_run ;;
    restart) cmd_restart ;;
    help|-h|--help|"") cmd_help ;;
    *)       err "unknown subcommand: $sub"; cmd_help; exit 2 ;;
esac
