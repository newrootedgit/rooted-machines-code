#!/usr/bin/env bash
#
# install-seeder-integrity.sh — self-heal the seeder scripts after storage damage.
#
#   scp install-seeder-integrity.sh rooted@<TAILSCALE_IP>:/tmp/
#   ssh rooted@<TAILSCALE_IP> 'sudo bash /tmp/install-seeder-integrity.sh'
#
# Safe to run repeatedly: every step is idempotent and only applies what is
# missing. Installs nothing that runs during normal operation — the check fires
# once at boot, before the machine-control services start, and then exits.
#
# WHY THIS EXISTS
#
# On 2026-08-11 the Freshleaf seeder lost power to an e-stop and came back with
# BOTH tabletop_seeder_poll.py and tabletop_seeder_tcp_server.py corrupted —
# runs of 0xFF (the pattern erased flash reads back as) starting on clean 4 KiB
# boundaries. Neither file is ever written at runtime. Two mechanisms do that:
# writeback still pending from a recent deploy, and the SD card's own garbage
# collection losing blocks mid-reclaim when power disappears.
#
# Both services then crash-looped on SyntaxError roughly every two seconds —
# poll for ~300 restarts, the TCP server for ~450 — with `systemctl is-active`
# reporting "activating" rather than "failed", because Restart=always keeps
# rescheduling. The machine was down for about twenty minutes and took an hour
# of remote debugging to identify. Nothing about that is recoverable by the
# machine on its own, and every minute of it was avoidable: a pristine copy of
# both files and a hash comparison at boot turns the whole incident into a log
# line and a restored file.
#
# WHAT IT INSTALLS
#
#   /opt/rooted/pristine/                  known-good copies + manifest.sha256
#   /usr/local/sbin/seeder-integrity-check  the check itself
#   /etc/systemd/system/seeder-integrity.service
#   drop-ins on seeder_poll.service and seeder_tcp_server.service making them
#   Require + follow the check
#
# IMPORTANT — THE PI STOPS BEING EDITABLE IN PLACE
#
# After this is installed, the live scripts must match the pristine copies. A
# change made on the Pi with nano SURVIVES until the next boot and is then
# reverted. That is the intended contract — the repo is the source of truth, not
# the card — but it will surprise anyone who edits in place. To change a script,
# deploy it with deploy-seeder.sh, which refreshes the pristine copy as part of
# the deploy. To experiment on the Pi, stop the check first:
#
#   sudo systemctl disable seeder-integrity.service
#
# It does NOT restart any machine-control service. Restarting poll or the TCP
# server trips the ready_to_run fail-safe and STOPS THE MACHINE, which is not
# something a maintenance script should do behind your back.

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must run as root:  sudo bash $0" >&2
    exit 1
fi

TE_DIR="/home/rooted/te-cli"
PRISTINE_DIR="/opt/rooted/pristine"
FILES="tabletop_seeder_poll.py tabletop_seeder_tcp_server.py"
CHANGED=0

say()  { printf '\n\033[1;32m==> %s\033[0m\n' "$1"; }
note() { printf '    %s\n' "$1"; }
warn() { printf '\033[1;33m    WARNING: %s\033[0m\n' "$1"; }
err()  { printf '\033[1;31m    ERROR: %s\033[0m\n' "$1"; }

# ---------------------------------------------------------------------------
# 1. Seed the pristine copies
# ---------------------------------------------------------------------------
say "Seeding pristine copies in $PRISTINE_DIR"

mkdir -p "$PRISTINE_DIR"

# Only ever seed from a file that actually compiles. Seeding from a corrupt live
# file would bake the damage into the baseline and make the checker restore
# garbage over good copies forever after — the failure mode that turns a safety
# net into the thing that breaks you.
for f in $FILES; do
    live="$TE_DIR/$f"

    if [ ! -f "$live" ]; then
        err "$f is missing from $TE_DIR — cannot seed a baseline from it"
        err "Deploy it first with deploy-seeder.sh, then re-run this script."
        exit 1
    fi

    if ! python3 -m py_compile "$live" 2>/dev/null; then
        err "$f does not compile — refusing to use it as a baseline."
        err "Restore a good copy from the repo first:"
        err "    scp $f rooted@<this-pi>:$TE_DIR/"
        exit 1
    fi

    if cmp -s "$live" "$PRISTINE_DIR/$f" 2>/dev/null; then
        note "$f — baseline already current"
    else
        install -o root -g root -m 444 "$live" "$PRISTINE_DIR/$f"
        note "$f — baseline written"
        CHANGED=1
    fi
done

# py_compile leaves bytecode next to the source; it is regenerable and only
# clutters the deploy directory.
rm -rf "$TE_DIR/__pycache__"

( cd "$PRISTINE_DIR" && sha256sum $FILES > manifest.sha256 )
chmod 444 "$PRISTINE_DIR/manifest.sha256"
sync
note "manifest.sha256 regenerated"

# ---------------------------------------------------------------------------
# 2. Install the check
# ---------------------------------------------------------------------------
say "Installing /usr/local/sbin/seeder-integrity-check"

cat > /tmp/.seeder-integrity-check <<'CHECK'
#!/usr/bin/env bash
#
# seeder-integrity-check — restore the seeder scripts if storage damaged them.
#
# Runs once at boot, before seeder_poll and seeder_tcp_server. Installed by
# install-seeder-integrity.sh; see that script for the reasoning.
#
# Exit 0 when every file is good or was successfully restored. Exit non-zero
# only when a file is damaged AND cannot be repaired — in which case the
# services that Require= this unit will not start, and systemd reports a failed
# dependency. That is a deliberate choice: a clear "dependency failed" beats a
# service crash-looping several hundred times while is-active reports
# "activating", which is what actually happened on 2026-08-11.

set -uo pipefail

TE_DIR="/home/rooted/te-cli"
PRISTINE_DIR="/opt/rooted/pristine"
FILES="tabletop_seeder_poll.py tabletop_seeder_tcp_server.py"
rc=0

if [ ! -d "$PRISTINE_DIR" ]; then
    echo "integrity: no pristine directory at $PRISTINE_DIR — nothing to check against"
    exit 0
fi

# Verify the BASELINE before trusting it. The pristine copies live on the same
# card as everything else and are just as corruptible; restoring from a damaged
# baseline would destroy good live files.
if ! ( cd "$PRISTINE_DIR" && sha256sum --status -c manifest.sha256 ); then
    echo "integrity: PRISTINE COPIES ARE THEMSELVES DAMAGED — refusing to restore anything."
    echo "integrity: re-deploy from the repo with deploy-seeder.sh to rebuild the baseline."
    exit 1
fi

for f in $FILES; do
    live="$TE_DIR/$f"
    good="$PRISTINE_DIR/$f"

    if cmp -s "$good" "$live"; then
        echo "integrity: $f OK"
        continue
    fi

    if [ ! -f "$live" ]; then
        echo "integrity: $f MISSING — restoring from pristine"
    else
        echo "integrity: $f DOES NOT MATCH PRISTINE ($(stat -c %s "$live") bytes) — restoring"
    fi

    if ! install -o rooted -g rooted -m 644 "$good" "$live"; then
        echo "integrity: $f RESTORE FAILED — could not write $live"
        rc=1
        continue
    fi

    # Without this the restored file is only in the page cache, and a second
    # power cut in the same window loses it exactly as the first one did.
    sync

    if cmp -s "$good" "$live"; then
        echo "integrity: $f restored and verified"
    else
        echo "integrity: $f STILL DOES NOT MATCH AFTER RESTORE — the card is corrupting writes."
        echo "integrity: this machine needs a reflash, not another restore."
        rc=1
    fi
done

exit "$rc"
CHECK

if ! cmp -s /tmp/.seeder-integrity-check /usr/local/sbin/seeder-integrity-check 2>/dev/null; then
    install -o root -g root -m 755 /tmp/.seeder-integrity-check /usr/local/sbin/seeder-integrity-check
    note "installed"
    CHANGED=1
else
    note "already installed, unchanged"
fi
rm -f /tmp/.seeder-integrity-check

# ---------------------------------------------------------------------------
# 3. Install the unit
# ---------------------------------------------------------------------------
say "Installing seeder-integrity.service"

cat > /tmp/.seeder-integrity.service <<'UNIT'
[Unit]
Description=Verify and restore seeder scripts before the machine services start
# Ordering only. The Requires= that makes this actually gate the services lives
# in the drop-ins on each service, so that starting a service by hand also runs
# the check rather than silently skipping it.
Before=seeder_poll.service seeder_tcp_server.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/seeder-integrity-check

[Install]
WantedBy=multi-user.target
UNIT

if ! cmp -s /tmp/.seeder-integrity.service /etc/systemd/system/seeder-integrity.service 2>/dev/null; then
    install -o root -g root -m 644 /tmp/.seeder-integrity.service /etc/systemd/system/seeder-integrity.service
    note "installed"
    CHANGED=1
else
    note "already installed, unchanged"
fi
rm -f /tmp/.seeder-integrity.service

# ---------------------------------------------------------------------------
# 4. Make the machine services depend on it
# ---------------------------------------------------------------------------
say "Wiring the machine services to the check"

for unit in seeder_poll.service seeder_tcp_server.service; do
    if [ ! -f "/etc/systemd/system/$unit" ]; then
        warn "$unit is not installed on this Pi — skipping"
        continue
    fi

    dropin_dir="/etc/systemd/system/${unit}.d"
    mkdir -p "$dropin_dir"

    printf '[Unit]\nRequires=seeder-integrity.service\nAfter=seeder-integrity.service\n' \
        > /tmp/.integrity-dropin.conf

    if ! cmp -s /tmp/.integrity-dropin.conf "${dropin_dir}/integrity.conf" 2>/dev/null; then
        install -o root -g root -m 644 /tmp/.integrity-dropin.conf "${dropin_dir}/integrity.conf"
        note "$unit — drop-in installed"
        CHANGED=1
    else
        note "$unit — drop-in already present"
    fi
    rm -f /tmp/.integrity-dropin.conf
done

systemctl daemon-reload
systemctl enable seeder-integrity.service >/dev/null 2>&1 || true
note "seeder-integrity.service enabled at boot"

# ---------------------------------------------------------------------------
# 5. Prove it works, now, without touching the machine services
# ---------------------------------------------------------------------------
say "Running the check once"

if /usr/local/sbin/seeder-integrity-check; then
    note "check passed"
else
    warn "check reported a problem — read the output above before rebooting"
fi

sync

say "Done"
note "The check runs automatically at every boot from here on."
note "To see what it did after a reboot:"
note "    journalctl -u seeder-integrity.service -b --no-pager"
if [ "$CHANGED" -eq 0 ]; then
    note "Nothing needed changing — this Pi was already set up."
fi
warn "Editing the scripts on the Pi with nano now only lasts until the next boot."
warn "Deploy with deploy-seeder.sh instead; it refreshes the pristine baseline."
