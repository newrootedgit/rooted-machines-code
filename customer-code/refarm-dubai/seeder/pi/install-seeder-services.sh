#!/usr/bin/env bash
#
# install-seeder-services.sh — install the two machine-control systemd units.
#
#   scp install-seeder-services.sh rooted@100.81.209.84:/tmp/
#   ssh -t rooted@100.81.209.84 'sudo bash /tmp/install-seeder-services.sh'
#
# Safe to run repeatedly: every step is idempotent and only applies what is
# missing. Run deploy-seeder.sh FIRST — this script refuses to install a unit
# pointing at a script that is not there or does not compile.
#
# It enables the units but does NOT start them. Starting the poll service drives
# the Touch Encoder and starting the TCP server opens the ClearCore link, which
# are machine-visible actions; the start commands are printed for you to run
# when you are standing at the machine.
#
# ORDER FOR A NEW MACHINE
#
#   1. deploy-seeder.sh                 puts the scripts on the Pi
#   2. install-seeder-services.sh       this script — creates the units
#   3. install-seeder-integrity.sh      self-healing (needs the units to exist)
#   4. install-shutdown-button.sh       clean halt from the HMI
#   5. harden-pi.sh                     journal + telemetry caps
#
# Steps 3 and 5 both look for the unit files, so running them before this one
# silently skips half their work.

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must run as root:  sudo bash $0" >&2
    exit 1
fi

TE_DIR="/home/rooted/te-cli"
VENV_PY="$TE_DIR/venv/bin/python"
SERVICE_USER="rooted"
CHANGED=0

say()  { printf '\n\033[1;32m==> %s\033[0m\n' "$1"; }
note() { printf '    %s\n' "$1"; }
warn() { printf '\033[1;33m    WARNING: %s\033[0m\n' "$1"; }
err()  { printf '\033[1;31m    ERROR: %s\033[0m\n' "$1"; }

# ---------------------------------------------------------------------------
# 1. Refuse to install units that cannot possibly work
# ---------------------------------------------------------------------------
say "Checking prerequisites"

if [ ! -x "$VENV_PY" ]; then
    err "No interpreter at $VENV_PY"
    err "The venv holding the Grayhill 'te' package is missing. Create it and"
    err "install the TE library before installing these services."
    exit 1
fi
note "interpreter: $VENV_PY"

# A unit that starts a script which cannot import its library just crash-loops,
# which is precisely the failure mode all the other tooling here exists to
# avoid. Catch it now, while someone is watching.
if ! sudo -u "$SERVICE_USER" "$VENV_PY" -c 'from te.interface.common import ScreenID' 2>/dev/null; then
    err "The 'te' package does not import in that venv as user $SERVICE_USER."
    err "Check with:"
    err "    sudo -u $SERVICE_USER $VENV_PY -c 'from te.interface.common import ScreenID'"
    exit 1
fi
note "Grayhill 'te' package imports as $SERVICE_USER"

for f in autoadjust_seeder_poll.py autoadjust_seeder_tcp_server.py; do
    if [ ! -f "$TE_DIR/$f" ]; then
        err "$f is missing from $TE_DIR — run deploy-seeder.sh first"
        exit 1
    fi
    if ! "$VENV_PY" -m py_compile "$TE_DIR/$f" 2>/dev/null; then
        err "$f does not compile — refusing to install a unit that points at it"
        exit 1
    fi
    note "$f present and compiles"
done
rm -rf "$TE_DIR/__pycache__"

# ---------------------------------------------------------------------------
# 2. Write the units
# ---------------------------------------------------------------------------
say "Installing unit files"

# PYTHONUNBUFFERED is not optional. Without it Python block-buffers stdout when
# it is not a tty, so print() output sits in a buffer instead of reaching the
# journal — a service running perfectly looks dead in journalctl. That has
# already cost real debugging time on the Freshleaf machine.
#
# StartLimitBurst/StartLimitIntervalSec: with Restart=always and no limit, a
# fatally broken script loops forever while `systemctl is-active` reports
# "activating" rather than "failed". That ambiguity is what made the Freshleaf
# outage on 2026-08-11 take an hour to diagnose — the service had restarted
# ~450 times and still did not read as broken. Five failures inside a minute is
# generous for a transient fault and decisive for a real one.
#
# This pairs with install-seeder-integrity.sh: the recoverable case (a corrupted
# script) is repaired at boot before these units start, so reaching "failed"
# means something the machine genuinely cannot fix itself.

write_unit() {
    local name="$1" desc="$2" script="$3"
    cat > "/tmp/.$name" <<UNIT
[Unit]
Description=$desc
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
User=$SERVICE_USER
WorkingDirectory=$TE_DIR
Environment=PYTHONUNBUFFERED=1
ExecStart=$VENV_PY $TE_DIR/$script
Restart=always
RestartSec=2
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
UNIT

    if ! cmp -s "/tmp/.$name" "/etc/systemd/system/$name" 2>/dev/null; then
        install -o root -g root -m 644 "/tmp/.$name" "/etc/systemd/system/$name"
        note "$name installed"
        CHANGED=1
    else
        note "$name already installed, unchanged"
    fi
    rm -f "/tmp/.$name"
}

write_unit "autoadjust_seeder_poll.service" \
           "Autoadjust Seeder Poll (Touch Encoder)" \
           "autoadjust_seeder_poll.py"

write_unit "autoadjust_seeder_tcp_server.service" \
           "Autoadjust Seeder TCP Server (ClearCore bridge)" \
           "autoadjust_seeder_tcp_server.py"

# ---------------------------------------------------------------------------
# 3. Enable, but do not start
# ---------------------------------------------------------------------------
say "Enabling at boot"

systemctl daemon-reload
systemctl enable autoadjust_seeder_poll.service >/dev/null 2>&1 || true
systemctl enable autoadjust_seeder_tcp_server.service >/dev/null 2>&1 || true
note "both units enabled"

# The TCP server binds a static address on the machine-side NIC. If that
# interface is not configured, it will fail at bind rather than at start, which
# looks like a code problem and is not.
if ! ip addr show | grep -q '192.168.10.1'; then
    warn "192.168.10.1 is not configured on any interface."
    warn "The TCP server binds that address and will fail to start without it."
    warn "Check the machine-side NIC configuration before starting the services."
fi

say "Done"
note "Units are installed and enabled but NOT running."
note "Start them when you are at the machine:"
note ""
note "    sudo systemctl start autoadjust_seeder_poll.service autoadjust_seeder_tcp_server.service"
note "    systemctl is-active autoadjust_seeder_poll.service autoadjust_seeder_tcp_server.service"
note "    journalctl -u autoadjust_seeder_poll.service -f"
note ""
note "In the poll log you want: 'poll: discovered Touch Encoder'"
note ""
note "Next: install-seeder-integrity.sh, which needs these units to exist"
note "before it can wire its drop-ins."
if [ "$CHANGED" -eq 0 ]; then
    note ""
    note "Nothing needed changing — this Pi was already set up."
fi