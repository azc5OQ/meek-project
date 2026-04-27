#!/usr/bin/env bash
# =============================================================================
# deploy.sh - push sources to a Linux SSH target and build.
# =============================================================================
#
# Linux-side (or WSL / git-bash) equivalent of deploy.bat. Same env
# var contract, same rsync + ssh plumbing, same error surface.
#
# Required environment variables:
#   MEEK_SSH_HOST      - hostname or IP of the Linux target
#   MEEK_SSH_USER      - username on the target
#   MEEK_REMOTE_PATH   - absolute path on the target for the repo
#
# Optional:
#   MEEK_SSH_PORT      - ssh port (defaults to 22)
#   MEEK_BUILD_TARGET  - passed to build.sh (default: build)
#
# See session/toolchain_paths.md for setup details.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

info()  { printf '\033[36m[deploy]\033[0m %s\n' "$*"; }
ok()    { printf '\033[32m[deploy]\033[0m %s\n' "$*"; }
err()   { printf '\033[31m[deploy]\033[0m %s\n' "$*" >&2; }
fatal() { err "$*"; exit 1; }

missing=()
[[ -n "${MEEK_SSH_HOST:-}"    ]] || missing+=("MEEK_SSH_HOST")
[[ -n "${MEEK_SSH_USER:-}"    ]] || missing+=("MEEK_SSH_USER")
[[ -n "${MEEK_REMOTE_PATH:-}" ]] || missing+=("MEEK_REMOTE_PATH")

if [[ ${#missing[@]} -gt 0 ]]; then
    err "missing env vars: ${missing[*]}"
    err ""
    err "Example:"
    err "  export MEEK_SSH_HOST=192.168.1.50"
    err "  export MEEK_SSH_USER=you"
    err "  export MEEK_REMOTE_PATH=/home/you/src/meek-compositor"
    err ""
    err "See session/toolchain_paths.md for details."
    exit 2
fi

SSH_PORT=${MEEK_SSH_PORT:-22}
BUILD_TARGET=${MEEK_BUILD_TARGET:-build}

info "target: $MEEK_SSH_USER@$MEEK_SSH_HOST:$MEEK_REMOTE_PATH"
info "port:   $SSH_PORT"
info "build:  $BUILD_TARGET"

#
# Ensure the remote path exists. -o BatchMode=yes makes the ssh
# fail fast if key auth isn't set up instead of prompting for a
# password in a non-interactive context.
#
ssh -p "$SSH_PORT" -o BatchMode=yes "$MEEK_SSH_USER@$MEEK_SSH_HOST" "mkdir -p '$MEEK_REMOTE_PATH'"

#
# Push compositor sources. Exclude build artifacts + the fat
# inspiration/ dir (tarballs are ~130 MB and aren't needed on the
# target). --delete keeps the remote tree in sync with ours.
#
info "rsync meek-compositor sources"
rsync -az --delete \
    --exclude=build/ \
    --exclude=inspiration/ \
    --exclude=.git/ \
    --exclude='*.o' \
    --exclude=meek_compositor \
    -e "ssh -p $SSH_PORT -o BatchMode=yes" \
    "$SCRIPT_DIR/" "$MEEK_SSH_USER@$MEEK_SSH_HOST:$MEEK_REMOTE_PATH/"

#
# Push the sibling meek-ui tree so the target has a consistent
# layout. Only types.h is actually used in this pass but future
# passes will compile meek-ui/gui/src/*.c and we want the deploy
# story to stay the same.
#
MEEK_UI_DIR="$(cd "$SCRIPT_DIR/../meek-ui" 2>/dev/null && pwd || true)"
if [[ -z "$MEEK_UI_DIR" || ! -d "$MEEK_UI_DIR" ]]; then
    fatal "expected sibling meek-ui at: $SCRIPT_DIR/../meek-ui (not found)"
fi

MEEK_UI_REMOTE="$(dirname "$MEEK_REMOTE_PATH")/meek-ui"
info "rsync meek-ui (types.h + future deps) -> $MEEK_UI_REMOTE"
rsync -az --delete \
    --exclude=build/ \
    --exclude=.git/ \
    --exclude='*.o' \
    -e "ssh -p $SSH_PORT -o BatchMode=yes" \
    "$MEEK_UI_DIR/" "$MEEK_SSH_USER@$MEEK_SSH_HOST:$MEEK_UI_REMOTE/"

ok "sources pushed"
info "running build.sh $BUILD_TARGET on target"

ssh -p "$SSH_PORT" -o BatchMode=yes "$MEEK_SSH_USER@$MEEK_SSH_HOST" \
    "cd '$MEEK_REMOTE_PATH' && chmod +x build.sh && ./build.sh $BUILD_TARGET"

ok "done"
