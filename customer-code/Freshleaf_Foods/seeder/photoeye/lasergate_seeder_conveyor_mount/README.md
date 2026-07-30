# Freshleaf Conveyor-Mount Seeder — Pi Service Setup & Debugging

This folder holds the Raspberry Pi software for the Freshleaf lasergate / conveyor-mount
tabletop seeder. Two Python processes run on the Pi as `systemd` services:

| Script | Role |
| --- | --- |
| `tabletop_seeder_poll.py` | Polls the Grayhill Touch Encoder over USB (HID). Reads operator input (variety selection, belt speed, delays/durations, start/stop) and writes it to the shared state file `TE_Variable_Values.json`. |
| `tabletop_seeder_tcp_server.py` | Reads that same JSON and streams it as a CSV line to the ClearCore motor controller over TCP (`192.168.10.1:8888`). This is the **only** bridge to the ClearCore. |

Both processes share one file, `/home/rooted/te-cli/TE_Variable_Values.json`, using advisory
file locks. The poll script writes it; the TCP server reads it. The poll script auto-creates a
default JSON on first run if none exists.

```
Touch Encoder ──USB──► tabletop_seeder_poll.py ──► TE_Variable_Values.json ──► tabletop_seeder_tcp_server.py ──TCP:8888──► ClearCore
```

---

## 1. Prerequisites

Everything lives under `/home/rooted/te-cli/` on the Pi, and the scripts run from a Python
virtual environment at `/home/rooted/te-cli/venv/`.

Confirm the venv and the `te` (Touch Encoder) package are present:

```bash
ls /home/rooted/te-cli/venv/bin/python
/home/rooted/te-cli/venv/bin/python -c "import te; print('te OK')"
```

If `import te` fails, the Grayhill `te-cli` package isn't installed in the venv — install it there
before continuing (the poll script imports `te.interface.common` and `te.utils.discovery_tool`).

## 2. Deploy the files to the Pi

From a terminal **on your Windows PC** (not from an SSH session), copy the two scripts into
`~/te-cli/` on the Pi. Adjust the Pi's IP as needed.

```powershell
cd "C:\Users\Owner\Desktop\Machine Code\rooted-machines-code"
scp "customer-code\Freshleaf_Foods\seeder\photoeye\lasergate_seeder_conveyor_mount\tabletop_seeder_poll.py" ^
    "customer-code\Freshleaf_Foods\seeder\photoeye\lasergate_seeder_conveyor_mount\tabletop_seeder_tcp_server.py" ^
    rooted@<PI_IP>:~/te-cli/
```

> Only copy `TE_Variable_Values.json` if you intend to **seed or replace** the operator's saved
> variety settings — it is the live state file. The poll script creates a fresh one automatically
> if it's missing.

---

## 3. Create the systemd services

Run these **on the Pi**.

**Poller service:**

```bash
sudo tee /etc/systemd/system/seeder_poll.service >/dev/null <<'EOF'
[Unit]
Description=Seeder Poll (Touch Encoder)
After=network-online.target
Wants=network-online.target
[Service]
Type=simple
User=rooted
WorkingDirectory=/home/rooted/te-cli
Environment=PYTHONUNBUFFERED=1
ExecStart=/home/rooted/te-cli/venv/bin/python /home/rooted/te-cli/tabletop_seeder_poll.py
Restart=always
RestartSec=2
NoNewPrivileges=true
[Install]
WantedBy=multi-user.target
EOF
```

**TCP server service:**

```bash
sudo tee /etc/systemd/system/seeder_tcp_server.service >/dev/null <<'EOF'
[Unit]
Description=Seeder TCP Server (ClearCore bridge)
After=network-online.target
Wants=network-online.target
[Service]
Type=simple
User=rooted
WorkingDirectory=/home/rooted/te-cli
Environment=PYTHONUNBUFFERED=1
ExecStart=/home/rooted/te-cli/venv/bin/python /home/rooted/te-cli/tabletop_seeder_tcp_server.py
Restart=always
RestartSec=2
NoNewPrivileges=true
[Install]
WantedBy=multi-user.target
EOF
```

> **`PYTHONUNBUFFERED=1` is not optional.** Without it Python block-buffers
> stdout when it isn't a tty, so `print()` output sits in a buffer instead of
> reaching the journal. The TCP server then looks silent in `journalctl` even
> while it is running perfectly — which has already cost real debugging time,
> because "no log output" reads as "service is broken" and sends you chasing
> the wrong thing entirely.

## 4. Enable and start

```bash
sudo systemctl daemon-reload
sudo systemctl enable seeder_poll.service
sudo systemctl enable seeder_tcp_server.service
sudo systemctl start seeder_poll.service
sudo systemctl start seeder_tcp_server.service
```

## 5. Verify

```bash
sudo systemctl is-active seeder_poll.service
sudo systemctl is-active seeder_tcp_server.service
```

Both should print `active`. For more detail or to diagnose a failure:

```bash
systemctl status seeder_poll.service
journalctl -u seeder_poll.service -n 50 --no-pager
journalctl -u seeder_tcp_server.service -n 50 --no-pager
```

Follow the logs live (handy while jogging the machine):

```bash
journalctl -u seeder_poll.service -u seeder_tcp_server.service -f
```

---

## 6. Pausing the services to run the scripts manually (debugging)

While a service is running it **holds the resources the script needs** — the poller owns the Touch
Encoder USB device, and the TCP server owns port `8888`. If you launch a script by hand without
stopping its service first, you'll get device-contention errors (poll) or `Address already in use`
(TCP server). So always **stop the matching service first.**

`stop` only pauses the service for the current session — it stays *enabled*, so it will come back on
the next reboot automatically. This is exactly what you want for temporary debugging.

**Step 1 — stop the service(s) you want to debug:**

```bash
# Stop just one, or both:
sudo systemctl stop seeder_poll.service
sudo systemctl stop seeder_tcp_server.service
```

**Step 2 — run the script manually** so you can watch its output live (`Ctrl+C` to quit):

```bash
cd /home/rooted/te-cli
./venv/bin/python tabletop_seeder_poll.py
# or, in another shell:
./venv/bin/python tabletop_seeder_tcp_server.py
```

Or activate the venv first if you prefer:

```bash
cd /home/rooted/te-cli
source venv/bin/activate
python tabletop_seeder_poll.py     # prompt now shows (venv)
deactivate                          # when done
```

Useful while debugging the poller — force a hard process restart on disconnect instead of the
default in-process reconnect:

```bash
TE_RECOVERY=restart ./venv/bin/python tabletop_seeder_poll.py
```

**Step 3 — when finished, hand control back to the services:**

```bash
sudo systemctl start seeder_poll.service
sudo systemctl start seeder_tcp_server.service
```

Confirm they picked back up:

```bash
sudo systemctl is-active seeder_poll.service seeder_tcp_server.service
```

> **Fail-safe note:** on startup the TCP server forces `ready_to_run = false` in the JSON, so the
> machine always comes up **stopped** after a (re)start of the services or a manual run — the
> operator must press Start on the Touch Encoder again.

---

## 7. Common service commands (reference)

| Action | Command |
| --- | --- |
| Restart after editing a script | `sudo systemctl restart seeder_poll.service` |
| Stop temporarily (survives reboot as enabled) | `sudo systemctl stop seeder_poll.service` |
| Disable from boot (does not stop now) | `sudo systemctl disable seeder_poll.service` |
| Stop **and** disable in one step | `sudo systemctl disable --now seeder_poll.service` |
| Reload after editing a `.service` file | `sudo systemctl daemon-reload` |
| Clear a failed state | `sudo systemctl reset-failed` |

> After changing any file under `/etc/systemd/system/`, run `sudo systemctl daemon-reload` before
> `restart`. After changing a **`.py` script**, a plain `restart` is enough (no reload needed).

---

## 8. SD card longevity (do this on every new Pi)

Flash **wear** is not the risk on these machines — the seeder scripts only write
on operator actions, which works out to centuries of headroom on an industrial
MLC card. The two things that actually kill a Pi in the field are **running out
of disk** and **unclean power loss.** Both are addressed below, and both need
doing once per machine.

### Cap the systemd journal

Journald defaults to using up to **10% of the filesystem** — about 1.6 GB on a
16 GB card, growing silently until it gets there.

```bash
sudo mkdir -p /etc/systemd/journald.conf.d
sudo tee /etc/systemd/journald.conf.d/size.conf >/dev/null <<'EOF'
[Journal]
SystemMaxUse=200M
EOF
sudo systemctl restart systemd-journald
journalctl --disk-usage
```

At roughly 33 MB/month of logs, 200 MB keeps about six months of rolling
history at a fixed, predictable cost.

### Bound the telemetry log — only if this Pi runs the AWS pipeline

If `rooted-ingest.service` is installed (it comes from the **Rooted-Web-App**
repo, not this one), it appends one JSON record per ClearCore status frame to
`/home/rooted/telemetry_log.jsonl` — **~38 MB/day, ~14 GB/year, and nothing
truncates it.** On a 16 GB card that fills the disk in roughly eight months,
after which writes fail and the machine misbehaves in confusing ways.

Install the logrotate config from that repo
(`pi-src/vector/rooted-telemetry.logrotate`), which bounds it to ~56 MB
steady state. Verify with:

```bash
systemctl is-active rooted-ingest.service rooted-vector.service
ls -lh /home/rooted/telemetry_log.jsonl*
systemctl status logrotate.timer --no-pager     # must be active (waiting)
```

> **Check both services, not just one.** If `rooted-ingest` is running while
> `rooted-vector` is not, the Pi writes telemetry all day that nothing ever
> ships — and once rotation is installed, that data is silently deleted rather
> than merely piling up. Either enable Vector, or disable the ingest service so
> nothing is written in the first place.

### Power loss

The seeder scripts write `TE_Variable_Values.json` atomically (temp file →
`fsync` → `os.replace` → directory `fsync`) and keep a `.bak` alongside it that
is restored automatically at startup if the live file is unreadable. Nothing
extra to configure — but **never** add code that rewrites that file with a
plain `open(path, "w")`. That truncates before writing, and a power cut in the
window destroys every variety preset on the machine.
