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

**Short version: run `harden-pi.sh` and you are done.** It lives in the
**Rooted-Web-App** repo at `pi-src/harden-pi.sh`, is safe on any Rooted Pi
(seeder, harvester, or plain telemetry box), and is safe to run repeatedly —
every step is idempotent and it only applies what is missing.

```bash
scp pi-src/harden-pi.sh rooted@<TAILSCALE_IP>:/tmp/
ssh rooted@<TAILSCALE_IP> 'sudo bash /tmp/harden-pi.sh'
```

New Pis provisioned with `deploy-vector.sh` get the telemetry and journal caps
automatically; the script is for machines already in the field, and as a
belt-and-braces check on anything you are unsure about.

The rest of this section explains *what* it does and why, so the reasoning
survives even if the script does not.

Flash **wear** is not the risk on these machines — the seeder scripts only write
on operator actions, which works out to centuries of headroom on an industrial
MLC card. The two things that actually kill a Pi in the field are **running out
of disk** and **unclean power loss.**

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

---

## 9. Surviving power loss

`harden-pi.sh` names unclean power loss as one of the two things that kill a Pi
in the field, and then only fixes the other one. This section is the missing
half.

### What actually happened

On **2026-08-11** an e-stop cut power to the Freshleaf seeder. It came back with
**both** `tabletop_seeder_poll.py` and `tabletop_seeder_tcp_server.py`
corrupted — runs of `0xFF` starting on clean 4 KiB boundaries, the pattern
erased flash reads back as. Both services crash-looped on `SyntaxError` every
two seconds (poll ~300 restarts, TCP ~450) while `systemctl is-active` reported
`activating` rather than `failed`, because `Restart=always` keeps rescheduling.
Twenty minutes of downtime, an hour of remote debugging.

Neither file is ever written at runtime, which is the part worth understanding —
"we barely write to the card" is not protection. Two mechanisms corrupt files
nobody is touching:

- **Pending writeback.** `scp` returns when data reaches the page cache, not the
  card. ext4 commits every ~5 s and the card buffers on top. Power lost inside
  that window leaves a file at its final size whose contents were never written.
- **FTL garbage collection.** SD cards do wear-levelling and block reclamation
  internally and invisibly. Power lost mid-reclaim can destroy blocks belonging
  to files untouched for months. This is what power-loss-protected industrial
  cards exist to prevent.

Application code cannot fix either one. What follows shrinks the window, makes
the damage self-healing, and protects the presets.

### Deploy durably — use `deploy-seeder.sh`

Never `scp` the scripts by hand. Two extra commands are the difference between a
deploy that survives and one that exists only in RAM:

```bash
./deploy-seeder.sh rooted@100.91.169.39
```

It compiles both files locally first, copies them, **`sync`s**, verifies the
deployed SHA-256 matches byte for byte, and refreshes the self-healing baseline
below. A hash mismatch after `sync` means the card is corrupting writes and the
machine needs a reflash rather than another attempt.

It deliberately does **not** restart anything — restarting trips the
`ready_to_run` fail-safe and stops the machine. It prints the restart commands
for you to run when the machine is idle.

### Self-healing — `install-seeder-integrity.sh`

Run once per machine:

```bash
scp install-seeder-integrity.sh rooted@<TAILSCALE_IP>:/tmp/
ssh rooted@<TAILSCALE_IP> 'sudo bash /tmp/install-seeder-integrity.sh'
```

This keeps known-good copies of both scripts in `/opt/rooted/pristine/` and
installs `seeder-integrity.service`, which runs before `seeder_poll` and
`seeder_tcp_server` at every boot. If a live script no longer matches its
pristine copy, it is restored, `sync`ed, and re-verified — and the journal says
so. The same incident becomes a log line instead of an hour of debugging.

Two details that make it a safety net rather than a footgun:

- **The baseline is verified before it is trusted.** The pristine copies are on
  the same card and just as corruptible. If `manifest.sha256` does not check
  out, the service refuses to restore anything rather than overwriting good
  files with damaged ones.
- **The baseline can only be seeded from a file that compiles.** Running the
  installer on an already-broken machine would otherwise bake the corruption in
  permanently.

> **The Pi stops being editable in place.** The live scripts must match the
> pristine copies, so a `nano` edit on the machine survives only until the next
> boot. That is the intended contract — the repo is the source of truth, not the
> card. To change a script, deploy it. To experiment on the machine, run
> `sudo systemctl disable seeder-integrity.service` first and re-enable it after.

This covers the **code** only. `TE_Variable_Values.json` is never touched by the
check — it is live state that must keep changing, and restoring it from a
baseline would wipe the operator's presets on every boot.

### Protecting the presets

Two changes in `tabletop_seeder_poll.py` cover the settings file:

- **The backup is created at startup if missing.** `backup_settings()` only ran
  after an operator saved a variety, so a machine nobody had saved on had no
  `.bak` at all — and `recover_settings_if_needed()` had nothing to restore
  from. The recovery path existed but was disarmed, which is exactly how
  Freshleaf was found. An existing backup is never overwritten at startup: it is
  the last state an operator explicitly saved, and refreshing it from whatever
  is on disk would let a damaged-but-loadable file replace a good copy.
- **Stored values are range-checked.** Damage does not always produce
  unparseable JSON — a corrupted block can land inside a number and leave a file
  that loads cleanly and carries nonsense. Values are checked against the
  `variable_ranges` block **in the settings file itself**, so a machine tuned on
  site is judged by its own limits. Out-of-range values are reported at startup;
  a garbled encoder read is refused rather than persisted, leaving the previous
  preset in place.

> Note that the delay and duration fields legitimately go **negative** —
> they are offsets, with a declared range of `-100..100`. Any "nothing can be
> negative" rule would reject valid presets. This is why the check reads the
> declared ranges rather than hardcoding assumptions.

### The Shut Down button — HMI screen 2

Not every power cut on this machine is an e-stop. Most of them are somebody
pulling the plug at the end of the shift, and that one is entirely avoidable:
the operator just has no way to halt the Pi from the HMI. Screen 2 carries a
**Shut Down** button that gives them one.

```
operator taps Shut Down  ->  GUIDE writes 1 into screen 2 / var 5
poll sees the latch      ->  clears ready_to_run  (machine parked first)
                         ->  writes /run/rooted/shutdown-request   (tmpfs)
                         ->  rooted-shutdown.path notices
                         ->  rooted-shutdown.service (root) flushes, syncs, halts
```

Install the privileged half once per machine:

```bash
scp install-shutdown-button.sh rooted@<host>:/tmp/
ssh -t rooted@<host> 'sudo bash /tmp/install-shutdown-button.sh'
```

It is idempotent — re-running it only applies what is missing. It does not test
itself, because the test halts the machine.

**The operator routine:**

1. press **Shut Down** on screen 2
2. wait for the Touch Encoder screen to go dark
3. unplug

The screen going dark is the safe-to-unplug signal: a Pi 5 cuts USB power when
it halts and the encoder is USB powered. Confirm that on the bench for this
carrier board before teaching it as the procedure.

**Why poll cannot just call `poweroff`.** It runs as `rooted` with
`NoNewPrivileges=true` (see section 3) and should stay that way — a
machine-control service does not need the right to switch the computer off. It
writes an unprivileged flag into `/run`; a systemd `.path` unit bridges that to
the root action. `/run` is tmpfs, so the request never touches the card and is
cleared at boot, which means a request that somehow survived a halt cannot
re-trigger a shutdown on the next start.

**The stale-press guard matters here.** The encoder dies with the Pi, so a press
that was never consumed can still be sitting in var 5 on the next boot. Poll
zeroes the latch on arrival at screen 2 and skips one read, so the leftover
cannot fire — otherwise the machine would shut itself down the moment anyone
opened the wrong screen. Same guard as screens 18 and 19.

If the request file cannot be written, poll logs `THE MACHINE WILL NOT HALT` and
re-arms the button rather than latching. Check for that line before assuming the
button works:

```bash
journalctl -u seeder_poll.service -b | grep -i shutdown
journalctl -u rooted-shutdown.service -b -1 --no-pager   # after it comes back
```

### What this still does not fix

The Shut Down button removes the daily unplug as a power event, and none of
the above stops the card's garbage collection from corrupting blocks nobody
touched. Software can shrink the window, make the code self-healing, and
protect the presets — but only two things remove the mechanism:

1. **Keep the Pi powered through an e-stop.** An e-stop's safety function is
   removing power from hazardous motion, not from the control computer; PLCs
   stay powered while the safety relay drops the motor contactor. If the Pi sits
   on the load side of that contactor, moving it to the always-on supply makes
   e-stop presses stop being power events at all. Needs sign-off from whoever
   owns the safety circuit, but it does not weaken the e-stop.
2. **Hold-up power.** A UPS/supercap HAT with a GPIO "power good" line and a
   unit that runs `systemctl poweroff` when it drops. Ten seconds of hold-up
   turns every power cut into a clean shutdown.

At the next reflash, use an industrial card with power-loss protection, or boot
from a USB SSD. Consumer SD is the wrong part for a machine whose power gets
yanked by design.

### Power loss

The seeder scripts write `TE_Variable_Values.json` atomically (temp file →
`fsync` → `os.replace` → directory `fsync`) and keep a `.bak` alongside it that
is restored automatically at startup if the live file is unreadable. Nothing
extra to configure — but **never** add code that rewrites that file with a
plain `open(path, "w")`. That truncates before writing, and a power cut in the
window destroys every variety preset on the machine.
