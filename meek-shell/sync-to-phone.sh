#!/usr/bin/env bash
#
# sync-to-phone.sh: copy .c/.h/.sh/.ui/.style/.md/.proto sources from
# Windows -> Poco F1 over scp. Rsync isn't available on Windows bash.
# .c and .h files are tiny so re-transmitting everything each run is
# fine.
#
# Usage:
#   ./sync-to-phone.sh              # push all three projects
#   ./sync-to-phone.sh meek-shell   # push one project
#
# Destination: /home/user/meek-testing/<project>
# Host:        user@10.250.90.37 (override via PHONE=... env var)

set -eu
PHONE="${PHONE:-user@192.168.1.101}"
REMOTE_ROOT="/home/user/meek-testing"
#
# This script lives at <project_root>/meek-shell/sync-to-phone.sh.
# HERE walks up to the project root so $HERE/<project> still
# resolves to the sibling project directories (meek-ui / meek-shell
# / meek-compositor).
#
HERE="$(cd "$(dirname "$0")/.." && pwd)"

projects=("$@")
if [ ${#projects[@]} -eq 0 ]; then
    projects=(meek-ui meek-compositor meek-shell)
fi

#
# Kill any live meek processes on the phone before syncing. Two
# reasons:
#   1. Avoid scp overwriting binaries that mmap() back to the
#      running process -- that can segfault the running copy or
#      corrupt the new one on some filesystems.
#   2. Guarantee a clean slate for the next test. No "is the old
#      compositor still scanning out the old shell buffer?"
#      confusion.
#
# Quiet on success (missing process = pkill rc=1, ignore). Runs
# through runlog so it shows up in claude.log.
#
echo "sync: stopping any running meek processes on $PHONE"
ssh "$PHONE" /home/user/meek-testing/runlog.sh "sync-kill-meek" \
    'pkill -9 -x meek_compositor 2>/dev/null; pkill -9 -x meek_shell 2>/dev/null; pkill -9 -x demo_settings 2>/dev/null; sleep 1; exit 0' \
    >/dev/null 2>&1 || true

#
# For each project we scp matching files under their original subdir
# structure. Using find to enumerate preserves directory layout; we
# pipe the file list through a single ssh+tar to keep ssh sessions
# short -- but the user asked for scp, so: one scp invocation per
# top-level subdir we care about. That's "dumb-but-works".
#
for p in "${projects[@]}"; do
    src="$HERE/$p"
    if [ ! -d "$src" ]; then
        echo "sync: skipping $p (no such dir)" >&2
        continue
    fi
    echo "sync: $p -> $PHONE:$REMOTE_ROOT/$p"

    # Make sure remote subtree exists.
    ssh "$PHONE" "mkdir -p $REMOTE_ROOT/$p"

    # Directories to sync per project. Globs are resolved locally;
    # scp -r preserves subdir structure under the destination.
    subdirs=()
    for d in src gui assets session views demo-linux-wayland demo-linux-drm demo-settings protocols scripts; do
        [ -d "$src/$d" ] && subdirs+=("$src/$d")
    done

    # Plus any build scripts + top-level .md files at the project root.
    top_files=()
    for f in "$src"/*.sh "$src"/*.md "$src"/build.ninja "$src"/Makefile; do
        [ -f "$f" ] && top_files+=("$f")
    done

    if [ ${#subdirs[@]} -gt 0 ]; then
        scp -q -r "${subdirs[@]}" "$PHONE:$REMOTE_ROOT/$p/"
    fi
    if [ ${#top_files[@]} -gt 0 ]; then
        scp -q "${top_files[@]}" "$PHONE:$REMOTE_ROOT/$p/"
    fi
done

echo "sync: done"
