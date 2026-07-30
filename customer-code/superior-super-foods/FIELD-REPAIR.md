# Superior Super Foods — field repair

## SD card / telemetry repair

**Symptom it prevents:** the Pi slowly fills its SD card and then starts
misbehaving in confusing ways — services failing to write, settings not saving,
odd errors that look like unrelated bugs. Caused by `rooted-ingest.service`
appending ~38 MB/day to `/home/rooted/telemetry_log.jsonl` with nothing ever
truncating it. On a 16 GB card that fills the disk in roughly **eight months**.

Flash *wear* is not the concern — these machines write on operator actions,
which is centuries of headroom on industrial MLC. Running out of **disk** and
**unclean power loss** are what actually kill a Pi in the field.

### The fix

Run [`harden-pi.sh`](harden-pi.sh) on the machine over Tailscale. Safe to run
repeatedly — every step is idempotent and it only applies what is missing.

```bash
scp harden-pi.sh rooted@<TAILSCALE_IP>:/tmp/
```

```bash
ssh rooted@<TAILSCALE_IP> 'sudo bash /tmp/harden-pi.sh'
```

Run those as two separate commands. Credentials and hostname are in
[`pi.txt`](pi.txt).

### What it does

1. Installs a logrotate config bounding the telemetry log to ~56 MB steady
   state, and forces one rotation immediately to reclaim whatever has already
   piled up.
2. Caps the systemd journal at 200 MB. It otherwise defaults to 10% of the
   filesystem — about 1.6 GB on a 16 GB card — and grows silently to get there.
3. Adds `PYTHONUNBUFFERED=1` to every Python service, so `print()` output
   actually reaches the journal. Without it a service that is running perfectly
   looks dead in `journalctl`, which is a genuinely misleading failure mode.

### What it deliberately does NOT do

**It never restarts a machine-control service.** Restarting a poll or TCP
service trips the `ready_to_run` fail-safe and **stops the machine.** The script
prints the restart commands and leaves the timing to you — do them when the
machine is idle, not mid-run.

### Watch for this in the output

If it warns that `rooted-ingest` is running but `rooted-vector` is not, that Pi
is writing telemetry that **nothing ever ships to AWS** — and now that rotation
is installed, that data gets deleted rather than merely accumulating. Resolve it
one way or the other:

```bash
sudo systemctl enable --now rooted-vector.service     # ship it
sudo systemctl disable --now rooted-ingest.service    # or stop writing it
```

## Checking on a machine remotely

```bash
journalctl -u seeder_poll.service -u seeder_tcp_server.service -f
```

```bash
df -h / && journalctl --disk-usage && ls -lh /home/rooted/telemetry_log.jsonl*
```

Service names vary by machine — seeder boxes use `seeder_*`, harvesters use
`harvester_*` or `autoadjust_harvester_*`. `systemctl list-units --type=service`
will show what this one actually runs.

---

The canonical copy of `harden-pi.sh` lives in the **Rooted-Web-App** repo at
`pi-src/harden-pi.sh`, where it is also wired into `deploy-vector.sh` so newly
provisioned Pis get this automatically. The copy here is for findability when
servicing these machines; change the canonical one and re-copy.
