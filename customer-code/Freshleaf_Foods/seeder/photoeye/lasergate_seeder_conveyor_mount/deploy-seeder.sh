#!/usr/bin/env bash
#
# deploy-seeder.sh — deploy the seeder scripts so the deploy actually survives.
#
#   ./deploy-seeder.sh rooted@100.91.169.39
#   ./deploy-seeder.sh rooted@freshleaf-seeder-1        # MagicDNS also works
#
# Run from this directory. On Windows use Git Bash, not PowerShell.
#
# WHY NOT JUST scp
#
# scp returns as soon as the data is in the Pi's page cache, NOT when it is on
# the card. ext4 commits every ~5 seconds and the SD card buffers on top of
# that. Cut power inside that window — an e-stop, a breaker, a pulled plug —
# and the file exists at its final size with contents that were never written,
# which reads back as runs of 0xFF and a Python SyntaxError on the next start.
#
# That is what took the Freshleaf machine down on 2026-08-11: both scripts
# corrupted, both services crash-looping, twenty minutes of downtime and an hour
# of remote debugging. The fix is two extra commands — sync, then verify — which
# is what this script adds around the copy.
#
# It also refreshes the pristine baseline used by seeder-integrity-check, so the
# self-healing copies track what you just deployed instead of drifting behind it.
#
# It does NOT restart anything. Restarting poll or the TCP server trips the
# ready_to_run fail-safe and STOPS THE MACHINE; the restart commands are printed
# for you to run when the machine is idle.

set -euo pipefail

HOST="${1:-}"
if [ -z "$HOST" ]; then
    echo "usage: $0 rooted@<host>" >&2
    echo "   eg: $0 rooted@100.91.169.39" >&2
    exit 1
fi

cd "$(dirname "$0")"

TE_DIR="/home/rooted/te-cli"
PRISTINE_DIR="/opt/rooted/pristine"
FILES="tabletop_seeder_poll.py tabletop_seeder_tcp_server.py"

say()  { printf '\n\033[1;32m==> %s\033[0m\n' "$1"; }
note() { printf '    %s\n' "$1"; }
err()  { printf '\033[1;31m    ERROR: %s\033[0m\n' "$1"; }

# ---------------------------------------------------------------------------
# 1. Check what we are about to ship
# ---------------------------------------------------------------------------
say "Checking the local copies"

# Git Bash on Windows usually has `python` and no `python3`; Linux and macOS
# usually have the reverse. Take whichever exists rather than silently skipping
# the one check that catches a broken file before it reaches the machine.
PY=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
        PY="$candidate"
        break
    fi
done

for f in $FILES; do
    if [ ! -f "$f" ]; then
        err "$f not found in $(pwd)"
        exit 1
    fi

    size="$(wc -c < "$f" | tr -d ' ')"

    # Catch a locally broken file here rather than in the field, once the
    # service is already crash-looping.
    if [ -n "$PY" ]; then
        if ! "$PY" -m py_compile "$f" 2>/dev/null; then
            err "$f does not compile — refusing to deploy it"
            exit 1
        fi
        note "$f — $size bytes, compiles"
    else
        # Say so rather than implying a check that never ran.
        note "$f — $size bytes (no python found locally; NOT compile-checked)"
    fi
done
rm -rf __pycache__

LOCAL_SUMS="$(sha256sum $FILES)"

# ---------------------------------------------------------------------------
# 2. Copy, then force it to the card
# ---------------------------------------------------------------------------
say "Copying to $HOST:$TE_DIR"
scp $FILES "$HOST:$TE_DIR/"

say "Flushing to disk"
# The whole point of this script. Without it the deploy is only as durable as
# the next five seconds of uptime.
ssh "$HOST" 'sync'
note "sync complete"

# ---------------------------------------------------------------------------
# 3. Verify what actually landed
# ---------------------------------------------------------------------------
say "Verifying"

REMOTE_SUMS="$(ssh "$HOST" "cd $TE_DIR && sha256sum $FILES")"

if [ "$LOCAL_SUMS" = "$REMOTE_SUMS" ]; then
    note "both files match byte for byte"
else
    err "MISMATCH between local and deployed files."
    err "The card may be corrupting writes — do not restart the services."
    echo
    echo "  local:"
    echo "$LOCAL_SUMS" | sed 's/^/    /'
    echo "  on $HOST:"
    echo "$REMOTE_SUMS" | sed 's/^/    /'
    echo
    err "Re-run once. If it mismatches again, this machine needs a reflash."
    exit 1
fi

# ---------------------------------------------------------------------------
# 4. Refresh the self-healing baseline
# ---------------------------------------------------------------------------
say "Refreshing the pristine baseline"

# Only if the integrity checker is actually installed. A Pi without it deploys
# fine; it just has no self-healing, which is the pre-existing state and not an
# error worth failing a deploy over.
if ssh "$HOST" "test -d $PRISTINE_DIR"; then
    # -t allocates a TTY so sudo can prompt for a password. Without it, a Pi
    # whose sudo is not passwordless fails here with "no tty present and no
    # askpass program specified" — after the files are already deployed, which
    # is a confusing place to stop.
    ssh -t "$HOST" "sudo install -o root -g root -m 444 $TE_DIR/tabletop_seeder_poll.py $PRISTINE_DIR/ \
        && sudo install -o root -g root -m 444 $TE_DIR/tabletop_seeder_tcp_server.py $PRISTINE_DIR/ \
        && cd $PRISTINE_DIR && sudo sh -c 'sha256sum $FILES > manifest.sha256' \
        && sudo chmod 444 manifest.sha256 && sync"
    note "baseline now matches what was just deployed"
else
    note "$PRISTINE_DIR not present — integrity checker is not installed on this Pi"
    note "install it with: install-seeder-integrity.sh"
fi

# ---------------------------------------------------------------------------
# 5. Hand the restart back to a human
# ---------------------------------------------------------------------------
say "Deployed"
note "The running services are STILL ON THE OLD CODE until restarted."
note "Restarting stops the machine (the ready_to_run fail-safe trips), so do it"
note "when the machine is idle:"
echo
# -t on the sudo line: these Pis do not have passwordless sudo, and without a
# TTY the restart fails with "a terminal is required to read the password"
# while the follow-up is-active still prints "active" — from the OLD code that
# never restarted. That combination reads as success and is easy to miss.
echo "    ssh -t $HOST 'sudo systemctl restart seeder_poll.service seeder_tcp_server.service'"
echo "    ssh $HOST 'systemctl is-active seeder_poll.service seeder_tcp_server.service'"
echo
