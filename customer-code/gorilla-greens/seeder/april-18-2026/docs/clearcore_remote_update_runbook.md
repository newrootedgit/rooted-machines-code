# ClearCore Firmware Update Runbook

**Workflow: Compile on Windows, upload via the seeder's Pi over SSH**

---

## Why this workflow exists

Teknic's `bossac` upload tool has a known verification bug on macOS that causes uploads to fail mid-write, leaving the ClearCore in a corrupted state. Teknic does not support macOS for uploads. **Never attempt a ClearCore firmware upload from a Mac.**

Linux `bossac` works correctly. Since every seeder in the fleet has a Raspberry Pi already reachable via Tailscale, the Pi can act as the upload host for any ClearCore in the field. This eliminates the need to ship a Windows laptop or visit the customer site for firmware updates — only a USB cable on-site is required.

---

## Prerequisites

- A Windows machine (or any Linux machine) with Arduino IDE installed — **NOT macOS for the upload step**
- SSH access to the target seeder's Pi via Tailscale
- Someone on-site at the seeder who can:
  - Plug a USB cable between the Pi and the ClearCore
  - Press the ClearCore's reset button
- The ClearCore firmware source (the `.ino` sketch from the firmware repo)
- A known-good USB data cable on-site (charge-only cables will not work)

---

## Part 1 — Compile the firmware on Windows

The compile step can be done on Mac too — only the upload is broken on Mac. But if you have Windows available, do everything there to keep the workflow simple.

1. **Install Arduino IDE** from [arduino.cc](https://www.arduino.cc/en/software). Version 2.x is fine.

2. **Add the Teknic board manager URL.** Open Arduino IDE → **File → Preferences**. In the field labeled "Additional Boards Manager URLs", paste:

   ```
   https://www.teknic.com/files/downloads/package_clearcore_index.json
   ```

   Click OK.

3. **Install the ClearCore board package.** Tools → Board → Boards Manager. Search for "ClearCore" and click **Install** on the Teknic ClearCore entry.

4. **Open the firmware sketch.** File → Open → navigate to the firmware's `.ino` file (e.g., from your local clone of the firmware git repo).

5. **Select the target board.** Tools → Board → ClearCore → **Teknic ClearCore**.

6. **Compile to a binary.** Sketch → **Export Compiled Binary** (or `Ctrl+Alt+S`). This compiles without uploading.

7. **Find the `.bin` file.** It will appear in the sketch folder, named like `<sketchname>.ino.bin`. This is the file you'll transfer to the Pi.

---

## Part 2 — Upload via the Pi

1. **Transfer the binary to the Pi** from the machine where you compiled:

   ```bash
   scp <sketchname>.ino.bin rooted@<seeder-tailscale-ip>:~/
   ```

   Get the seeder's Tailscale IP from the Tailscale admin console.

2. **SSH into the Pi:**

   ```bash
   ssh rooted@<seeder-tailscale-ip>
   ```

3. **Install bossac if it's not already there** (one-time per Pi):

   ```bash
   sudo apt update
   sudo apt install -y bossa-cli
   ```

4. **Coordinate with on-site:**

   a. Plug a USB cable from a USB port on the Pi to the USB port on the ClearCore. Must be a data cable, not charge-only.

   b. Confirm the ClearCore's main power LED is on.

   c. **Double-click the RESET button on the ClearCore** to enter bootloader mode. Two distinct presses within about 500ms.

   d. **Confirm bootloader mode visually**: the six I/O LEDs along the bottom (IO-0 through IO-5) will blink in sequence, and the underglow LEDs will slowly breathe in and out. If you don't see this pattern, the double-click didn't take — have them try again.

5. **Verify the bootloader is exposed to the Pi.** In your SSH session:

   ```bash
   ls /dev/ttyACM*
   ```

   You should see `/dev/ttyACM0` (or similar). If nothing shows up, the bootloader didn't start — see Troubleshooting below.

6. **Run the upload:**

   ```bash
   sudo bossac -i -d -a --port=ttyACM0 -U -e -w -v --offset=0x4000 ~/<sketchname>.ino.bin -R
   ```

   Flag reference:
   - `-i` show device info
   - `-d` debug output
   - `-a` auto-detect device
   - `-U` use USB port
   - `-e` erase flash before writing
   - `-w` write the binary
   - `-v` verify after writing
   - `--offset=0x4000` application start address (bootloader occupies `0x0–0x4000` and is write-protected)
   - `-R` reset the device after upload

7. **Watch for success.** The end of the output should look like:

   ```
   [==============================] 100% (391/391 pages)
   Verify successful
   Done in 1.346 seconds
   writeWord(addr=0xe000ed0c,value=0x5fa0004)
   ```

   The `writeWord` line is the ARM reset command — the ClearCore will reboot into the new firmware automatically.

---

## Verification

After the upload completes, confirm the ClearCore is back online:

1. **Ping the ClearCore:**

   ```bash
   ping -c 5 192.168.10.2
   ```

   Should return 5 successful replies within a second or two of the upload finishing.

2. **Watch the TCP server logs** to confirm the ClearCore is polling:

   ```bash
   sudo journalctl -u conveyor_seeder_tcp_server.service -f
   ```

   Within a few seconds you should see entries like:

   ```
   tcp: ('192.168.10.2', xxxxx) -> '0,1,22,9,0,0,0,0,0,0'
   ```

   That's the ClearCore polling the Pi every ~second. The seeder is back in business.

---

## Troubleshooting

### `Failed to open port at 1200bps`

`/dev/ttyACM*` doesn't exist. Possible causes:

- USB cable isn't actually connected (visually confirm both ends with on-site)
- Cable is charge-only — swap for a known-good data cable
- ClearCore isn't in bootloader mode — have on-site double-click reset again
- ClearCore main power isn't on

While they retry, leave `sudo dmesg -w` running on your SSH session — you should see USB enumeration events the moment the bootloader actually starts.

### `Verify failed` with hundreds of page errors

You're running this on macOS, or you're using a macOS-built bossac. **Stop immediately.** Each failed attempt potentially leaves the ClearCore's flash in a worse state. Re-run from the Pi (Linux) or directly from a Windows machine.

### `Permission denied` opening `/dev/ttyACM0`

Run with `sudo`. To avoid `sudo` going forward, add the `rooted` user to the dialout group:

```bash
sudo usermod -a -G dialout rooted
```

Then log out and back in for the group change to take effect.

### Bootloader doesn't respond to double-click reset

Try the double-click several times with varied timing. If still nothing, try the alternate method:

1. Unplug USB
2. Hold the RESET button down
3. Plug USB back in while still holding RESET
4. Release RESET after 2 seconds

If after multiple attempts there's still no I/O LED sequencing and no `dmesg` events on the Pi, the failure is hardware, not firmware. The bootloader resides in a write-protected flash region (`0x0000–0x4000`) and cannot normally be corrupted by a bad upload. Hardware causes include MCU damage, a dead USB peripheral on the ClearCore, or a dead 3.3V rail. Plan for a ClearCore swap.

### On-site says cable is plugged in but `dmesg` shows zero USB events

Either the cable is dead, the cable is charge-only, or one end isn't actually seated in what they think it's seated in. Have them swap to a different known-good data cable and watch `dmesg -w` while they re-plug.

---

## Hardware-recovery checklist (for the bench)

If a ClearCore comes back to Rooted as unresponsive after a failed Mac upload:

1. Connect the ClearCore via USB to a **Windows or Linux** machine
2. Double-click reset to enter bootloader mode
3. Verify I/O LED sequence + underglow breathing
4. Verify `/dev/ttyACM*` (Linux) or COM port (Windows) appears
5. Run bossac with a known-good firmware `.bin` and `--offset=0x4000`
6. Confirm `Verify successful`
7. Power-cycle, confirm normal operation

If steps 2–4 fail, it's hardware; bench-diagnose with a multimeter on the 3.3V rail and consider SWD recovery if available.

---

## Notes for the team

- **Document the firmware version** that was uploaded each time (date, git commit hash, or release tag) in the seeder's CRM record. If this firmware breaks something later, you'll want to know exactly what was deployed.
- **Standardize the USB cable** between Pi and ClearCore on every seeder build. Running a permanent zip-tied USB cable between Pi and ClearCore on every unit converts every future firmware update from "field service trip" into "ssh + scp + bossac" — no customer-side action needed at all.
- **Update the firmware repo README** to point at this runbook so the next person who needs to update a ClearCore doesn't re-discover the Mac issue the hard way.