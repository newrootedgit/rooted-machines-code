#!/usr/bin/env bash
#
# harden-pi.sh — make a Rooted Pi survive in the field.
#
# >>> FIELD COPY, kept here so it is findable when servicing Superior Super
# >>> Foods machines. The canonical version lives in the Rooted-Web-App repo at
# >>> pi-src/harden-pi.sh, and that is the one wired into deploy-vector.sh.
# >>> If you change behaviour, change it THERE and re-copy, or the two drift
# >>> and the next person cannot tell which is current.
#
#   scp harden-pi.sh rooted@<TAILSCALE_IP>:/tmp/
#   ssh rooted@<TAILSCALE_IP> 'sudo bash /tmp/harden-pi.sh'
#
# The script is fully self-contained — every config it installs is embedded
# below — so this copy works standalone with nothing else checked out.
#
# Safe to run on any Rooted Pi — seeder, harvester, or a plain telemetry box —
# and safe to run REPEATEDLY. Every step is idempotent. It reads the machine
# and applies only what is missing.
#
# It does NOT restart any machine-control service. Restarting a poll or TCP
# service trips the ready_to_run fail-safe and STOPS the machine, which is not
# something a maintenance script should do behind your back. It prints what
# needs restarting and leaves the timing to you.
#
# WHAT IT FIXES
#
# 1. Unbounded telemetry spool. rooted-ingest appends one JSON record per
#    ClearCore status frame — one per second, ~435 bytes — to
#    telemetry_log.jsonl, and nothing truncates it. That is ~38 MB/day and
#    ~14 GB/year. On a 16 GB card it fills the disk in roughly eight months,
#    after which writes fail and the machine misbehaves in confusing ways.
#
# 2. Unbounded journal. journald defaults to 10% of the filesystem — about
#    1.6 GB on a 16 GB card — and grows silently until it gets there.
#
# 3. Invisible logs. Python block-buffers stdout when it is not a tty, so
#    print() output never reaches the journal and a service that is running
#    perfectly looks dead in journalctl. This has already cost real debugging
#    time on the Freshleaf machine.
#
# Flash WEAR is deliberately not addressed: these machines write on operator
# actions, which is centuries of headroom on industrial MLC. Running out of
# disk and unclean power loss are what actually kill a Pi in the field.

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must run as root:  sudo bash $0" >&2
    exit 1
fi

TELEMETRY_LOG="/home/rooted/telemetry_log.jsonl"
CHANGED=0

say()  { printf '\n\033[1;32m==> %s\033[0m\n' "$1"; }
note() { printf '    %s\n' "$1"; }
warn() { printf '\033[1;33m    WARNING: %s\033[0m\n' "$1"; }

say "Before"
note "disk:    $(df -h / | awk 'NR==2 {print $4" free of "$2" ("$5" used)"}')"
note "journal: $(journalctl --disk-usage 2>/dev/null | sed 's/^Archived and active journals take up //')"
if [ -f "$TELEMETRY_LOG" ]; then
    note "telemetry log: $(du -h "$TELEMETRY_LOG" | cut -f1)"
else
    note "telemetry log: not present"
fi

# ---------------------------------------------------------------------------
# 1. Bound the telemetry spool
# ---------------------------------------------------------------------------
say "Bounding the telemetry spool"

# copytruncate is REQUIRED, not stylistic: Vector tails this file with
# fingerprint.strategy = "device_and_inode". Renaming it changes the inode and
# makes Vector re-read a "new" file from the beginning, duplicating every
# event. copytruncate holds the inode and resets the length instead.
cat > /tmp/.rooted-telemetry.logrotate <<'LOGROTATE'
/home/rooted/telemetry_log.jsonl {
    size 50M
    rotate 2
    compress
    copytruncate
    missingok
    notifempty
}
LOGROTATE

# install(1) rather than mv: logrotate SILENTLY skips config files that are
# group or world writable. No error, no rotation, and nothing to notice.
if ! cmp -s /tmp/.rooted-telemetry.logrotate /etc/logrotate.d/rooted-telemetry 2>/dev/null; then
    install -o root -g root -m 644 /tmp/.rooted-telemetry.logrotate /etc/logrotate.d/rooted-telemetry
    note "installed /etc/logrotate.d/rooted-telemetry"
    CHANGED=1
else
    note "already installed, unchanged"
fi
rm -f /tmp/.rooted-telemetry.logrotate

if [ -f "$TELEMETRY_LOG" ]; then
    note "forcing one rotation to reclaim space now..."
    logrotate -f /etc/logrotate.d/rooted-telemetry || warn "logrotate returned non-zero"
    note "telemetry log now: $(du -h "$TELEMETRY_LOG" | cut -f1)"
fi

if ! systemctl is-active --quiet logrotate.timer; then
    warn "logrotate.timer is NOT active — rotation will never run on its own."
    warn "Fix with: sudo systemctl enable --now logrotate.timer"
fi

# ---------------------------------------------------------------------------
# 2. Cap the journal
# ---------------------------------------------------------------------------
say "Capping the systemd journal"

mkdir -p /etc/systemd/journald.conf.d
printf '[Journal]\nSystemMaxUse=200M\n' > /tmp/.journald-size.conf
if ! cmp -s /tmp/.journald-size.conf /etc/systemd/journald.conf.d/size.conf 2>/dev/null; then
    install -o root -g root -m 644 /tmp/.journald-size.conf /etc/systemd/journald.conf.d/size.conf
    systemctl restart systemd-journald
    note "capped at 200M and restarted journald"
    CHANGED=1
else
    note "already capped, unchanged"
fi
rm -f /tmp/.journald-size.conf

# ---------------------------------------------------------------------------
# 3. Make Python services log promptly
# ---------------------------------------------------------------------------
say "Making Python services log promptly"

# Discovered rather than hardcoded: the fleet uses seeder_*, harvester_*,
# autoadjust_harvester_* and conveyor_seeder_* naming, and a fixed list would
# quietly miss machines.
NEEDS_RESTART=""
for unit_file in /etc/systemd/system/*.service; do
    [ -e "$unit_file" ] || continue
    grep -qi 'ExecStart=.*python' "$unit_file" || continue
    unit="$(basename "$unit_file")"

    # Already set directly in the unit? Nothing to do.
    if grep -q 'PYTHONUNBUFFERED' "$unit_file"; then
        note "$unit — already unbuffered in the unit file"
        continue
    fi

    dropin_dir="/etc/systemd/system/${unit}.d"
    dropin="${dropin_dir}/unbuffered.conf"
    if [ -f "$dropin" ]; then
        note "$unit — drop-in already present"
        continue
    fi

    mkdir -p "$dropin_dir"
    printf '[Service]\nEnvironment=PYTHONUNBUFFERED=1\n' > "$dropin"
    chmod 644 "$dropin"
    note "$unit — added drop-in"
    NEEDS_RESTART="${NEEDS_RESTART} ${unit}"
    CHANGED=1
done

if [ -n "$NEEDS_RESTART" ]; then
    systemctl daemon-reload
fi

# ---------------------------------------------------------------------------
# 4. Report anything that needs a human
# ---------------------------------------------------------------------------
say "After"
note "disk:    $(df -h / | awk 'NR==2 {print $4" free of "$2" ("$5" used)"}')"
note "journal: $(journalctl --disk-usage 2>/dev/null | sed 's/^Archived and active journals take up //')"

# The ingest/vector split is worth shouting about: ingest running WITHOUT
# vector means the Pi writes telemetry all day that nothing ever ships — and
# now that rotation is installed, that data is deleted rather than piling up.
ingest_active=$(systemctl is-active rooted-ingest.service 2>/dev/null || true)
vector_active=$(systemctl is-active rooted-vector.service 2>/dev/null || true)
if [ "$ingest_active" = "active" ] && [ "$vector_active" != "active" ]; then
    say "Attention"
    warn "rooted-ingest is running but rooted-vector is ${vector_active:-not installed}."
    warn "This Pi writes telemetry that nothing ships, and rotation now deletes it."
    warn "Either:  sudo systemctl enable --now rooted-vector.service"
    warn "    or:  sudo systemctl disable --now rooted-ingest.service   (stops the writes)"
fi

if [ -n "$NEEDS_RESTART" ]; then
    say "Restart needed for log visibility"
    warn "These services picked up a config change but are still running the old"
    warn "environment. Restarting a poll or TCP service STOPS THE MACHINE (the"
    warn "ready_to_run fail-safe trips), so do it when the machine is idle:"
    for u in $NEEDS_RESTART; do
        printf '        sudo systemctl restart %s\n' "$u"
    done
fi

if [ "$CHANGED" -eq 0 ]; then
    say "Nothing to change — this Pi was already hardened."
else
    say "Done."
fi
