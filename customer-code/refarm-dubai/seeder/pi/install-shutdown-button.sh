#!/usr/bin/env bash
#
# install-shutdown-button.sh — let the Touch Encoder halt the machine cleanly.
#
#   scp install-shutdown-button.sh rooted@100.81.209.84:/tmp/
#   ssh -t rooted@100.81.209.84 'sudo bash /tmp/install-shutdown-button.sh'
#
# Safe to run repeatedly: every step is idempotent and only applies what is
# missing. Installs nothing that runs during normal operation — the watcher is
# a systemd .path unit, which costs nothing until the file appears.
#
# WHY THIS EXISTS
#
# This machine is unplugged at the end of every working day. Without a clean
# halt first, that is an unclean shutdown roughly 250 times a year. One of those
# is what corrupted BOTH Python scripts on the Freshleaf seeder on 2026-08-11:
# runs of 0xFF starting on clean 4 KiB boundaries, both services crash-looping,
# twenty minutes down and an hour of remote debugging.
#
# The operator routine this supports:
#
#     press the Shut Down button on the HMI
#     wait for the Touch Encoder screen to go dark
#     unplug the machine
#
# The screen going dark is the safe-to-unplug signal — a Pi 5 cuts USB power
# when it halts, and the encoder is USB powered. Verify that on the bench for
# your carrier board before teaching it as the procedure.
#
# HOW IT WORKS
#
#     poll sees screen 4  ->  writes /run/rooted/shutdown-request  (tmpfs)
#                         ->  rooted-shutdown.path notices
#                         ->  rooted-shutdown.service (root) flushes, syncs, halts
#
# poll runs as `rooted` with NoNewPrivileges=true and therefore cannot power the
# machine off itself — which is correct. A machine-control service should not
# hold the right to switch the computer off. The .path unit is what bridges the
# unprivileged request to the privileged action, without weakening the service.
#
# /run is tmpfs, so the request never touches storage and is cleared at boot for
# free. A request that somehow survived could not re-trigger a halt on the next
# start, which would be a genuinely nasty failure mode on a machine that is
# supposed to come up when it is plugged in.

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must run as root:  sudo bash $0" >&2
    exit 1
fi

RUN_DIR="/run/rooted"
REQUEST="$RUN_DIR/shutdown-request"
SERVICE_USER="rooted"
CHANGED=0

say()  { printf '\n\033[1;32m==> %s\033[0m\n' "$1"; }
note() { printf '    %s\n' "$1"; }
warn() { printf '\033[1;33m    WARNING: %s\033[0m\n' "$1"; }

# ---------------------------------------------------------------------------
# 1. Give poll somewhere it is allowed to write
# ---------------------------------------------------------------------------
say "Creating $RUN_DIR"

# /run is root-owned and mode 755, so `rooted` cannot mkdir inside it. Without
# this the request write fails with EACCES and the operator gets a machine that
# never halts — the failure this whole thing exists to prevent, arriving
# silently. tmpfiles.d rather than RuntimeDirectory= on the service, because
# RuntimeDirectory is removed when the service stops and the .path unit needs
# the directory to keep existing.
printf 'd %s 0755 %s %s -\n' "$RUN_DIR" "$SERVICE_USER" "$SERVICE_USER" \
    > /tmp/.rooted-tmpfiles.conf

if ! cmp -s /tmp/.rooted-tmpfiles.conf /etc/tmpfiles.d/rooted.conf 2>/dev/null; then
    install -o root -g root -m 644 /tmp/.rooted-tmpfiles.conf /etc/tmpfiles.d/rooted.conf
    note "installed /etc/tmpfiles.d/rooted.conf"
    CHANGED=1
else
    note "already installed, unchanged"
fi
rm -f /tmp/.rooted-tmpfiles.conf

# Apply now rather than waiting for the next boot, so the button works today.
systemd-tmpfiles --create /etc/tmpfiles.d/rooted.conf
note "$RUN_DIR present, owned by $SERVICE_USER"

# ---------------------------------------------------------------------------
# 2. The privileged half
# ---------------------------------------------------------------------------
say "Installing /usr/local/sbin/rooted-shutdown"

cat > /tmp/.rooted-shutdown <<'SHUTDOWN'
#!/usr/bin/env bash
#
# rooted-shutdown — halt the machine cleanly on operator request.
#
# Triggered by rooted-shutdown.path when the Touch Encoder shutdown screen
# writes /run/rooted/shutdown-request. Installed by install-shutdown-button.sh.

set -uo pipefail

REQUEST="/run/rooted/shutdown-request"

echo "shutdown: request received — $(cat "$REQUEST" 2>/dev/null || echo 'no detail')"

# Remove it before halting, not after. If the halt is interrupted the request
# must not still be sitting there, or the machine would try to shut down again
# the moment the .path unit starts on the next boot.
# Time each step, so a slow halt says which part was slow. The Freshleaf seeder
# took anywhere from 5 seconds to over a minute for the same button press, which
# reads as a crash rather than a shutdown; the journal and the page cache both
# grow with uptime, so the cost varies with how long the machine has been on.
# Read these from the PREVIOUS boot after a slow one:
#
#     journalctl -u rooted-shutdown.service -b -1 --no-pager
step_start=$(date +%s%N)
step() {
    local now elapsed
    now=$(date +%s%N)
    elapsed=$(( (now - step_start) / 1000000 ))
    echo "shutdown: $1 took ${elapsed} ms"
    step_start=$now
}

rm -f "$REQUEST"

# Move journal records out of the volatile store so the shutdown is actually
# recorded. Without this, the log of the clean shutdown is the first casualty
# of the clean shutdown.
journalctl --flush || true
step "journal flush"

# Belt and braces: systemd unmounts cleanly on poweroff, but poll wrote
# ready_to_run=false moments ago and that write should be on the card before
# anything else happens.
sync
step "sync"

echo "shutdown: halting now — safe to unplug once the encoder screen goes dark"
systemctl poweroff
SHUTDOWN

if ! cmp -s /tmp/.rooted-shutdown /usr/local/sbin/rooted-shutdown 2>/dev/null; then
    install -o root -g root -m 755 /tmp/.rooted-shutdown /usr/local/sbin/rooted-shutdown
    note "installed"
    CHANGED=1
else
    note "already installed, unchanged"
fi
rm -f /tmp/.rooted-shutdown

# ---------------------------------------------------------------------------
# 2b. Bound how long any single unit may delay the halt
# ---------------------------------------------------------------------------
say "Capping the per-unit stop timeout"

# systemd's default is 90 seconds per unit. One service declining to exit is a
# minute and a half of an operator watching a screen that says the machine is
# shutting down and wondering whether it has crashed — and they cannot tell,
# which is the whole problem. This is what took the Freshleaf seeder from a
# wildly variable halt to a consistent ten seconds.
#
# Ten seconds is generous for everything here: autoadjust_seeder_poll and
# autoadjust_seeder_tcp_server both exit on SIGTERM immediately, and the only
# state that must reach the card is a settings JSON that was already fsynced
# when it was written. A unit that has not stopped in ten seconds is not going
# to, and SIGKILL is the right answer for a machine whose next action is losing
# power anyway.
#
# System-wide rather than per-unit on purpose: the units that delay a shutdown
# are usually not the ones anybody thought to configure.
install -d -m 755 /etc/systemd/system.conf.d
cat > /tmp/.rooted-timeout.conf <<'TIMEOUT'
# Installed by install-shutdown-button.sh — see that script for the reasoning.
[Manager]
DefaultTimeoutStopSec=10s
TIMEOUT

if ! cmp -s /tmp/.rooted-timeout.conf /etc/systemd/system.conf.d/rooted-shutdown-timeout.conf 2>/dev/null; then
    install -o root -g root -m 644 /tmp/.rooted-timeout.conf         /etc/systemd/system.conf.d/rooted-shutdown-timeout.conf
    note "installed /etc/systemd/system.conf.d/rooted-shutdown-timeout.conf"
    warn "this takes effect after the NEXT reboot — systemd cannot reload"
    warn "manager configuration without one."
    CHANGED=1
else
    note "already installed, unchanged"
fi
rm -f /tmp/.rooted-timeout.conf

# ---------------------------------------------------------------------------
# 3. The watcher
# ---------------------------------------------------------------------------
say "Installing rooted-shutdown.path and rooted-shutdown.service"

cat > /tmp/.rooted-shutdown.service <<'UNIT'
[Unit]
Description=Halt the machine cleanly on operator request from the Touch Encoder

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/rooted-shutdown
UNIT

# No [Install] on the service: it is started by the .path unit, never enabled
# directly. Enabling it would run a shutdown at every boot.
if ! cmp -s /tmp/.rooted-shutdown.service /etc/systemd/system/rooted-shutdown.service 2>/dev/null; then
    install -o root -g root -m 644 /tmp/.rooted-shutdown.service /etc/systemd/system/rooted-shutdown.service
    note "rooted-shutdown.service installed"
    CHANGED=1
else
    note "rooted-shutdown.service already installed, unchanged"
fi
rm -f /tmp/.rooted-shutdown.service

cat > /tmp/.rooted-shutdown.path <<'UNIT'
[Unit]
Description=Watch for a shutdown request from the Touch Encoder

[Path]
PathExists=/run/rooted/shutdown-request
Unit=rooted-shutdown.service

[Install]
WantedBy=multi-user.target
UNIT

if ! cmp -s /tmp/.rooted-shutdown.path /etc/systemd/system/rooted-shutdown.path 2>/dev/null; then
    install -o root -g root -m 644 /tmp/.rooted-shutdown.path /etc/systemd/system/rooted-shutdown.path
    note "rooted-shutdown.path installed"
    CHANGED=1
else
    note "rooted-shutdown.path already installed, unchanged"
fi
rm -f /tmp/.rooted-shutdown.path

systemctl daemon-reload
systemctl enable --now rooted-shutdown.path >/dev/null 2>&1 || true

if systemctl is-active --quiet rooted-shutdown.path; then
    note "rooted-shutdown.path is active and watching"
else
    warn "rooted-shutdown.path is NOT active — the button will do nothing."
    warn "Investigate with: systemctl status rooted-shutdown.path"
fi

# ---------------------------------------------------------------------------
# 4. Tell the operator how to test it
# ---------------------------------------------------------------------------
say "Done"
note "Deliberately NOT tested automatically — the test halts the machine."
note ""
note "To test from this Pi (IT WILL SHUT DOWN):"
note "    sudo -u $SERVICE_USER touch $REQUEST"
note ""
note "To test the real path, press the Shut Down button on the HMI."
note "Either way the machine should halt within a couple of seconds."
note ""
note "After it comes back up, confirm it went down cleanly:"
note "    journalctl -u rooted-shutdown.service -b -1 --no-pager"
note "    journalctl --list-boots | tail -3"
if [ "$CHANGED" -eq 0 ]; then
    note ""
    note "Nothing needed changing — this Pi was already set up."
fi