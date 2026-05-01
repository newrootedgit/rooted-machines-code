# SC-Hub USB Driver Setup on the Seeder Pi

## TL;DR

The Teknic SC4-Hub enumerates as a USB CDC-ACM device. Linux's generic
`cdc_acm` driver claims it first and exposes it as `/dev/ttyACM0`, but the
Teknic sFoundation library (and therefore `seeder_test`) requires the Exar
`xr_usb_serial_common` driver, which exposes the Hub as `/dev/ttyXRUSB0`.
On a fresh Pi, three things have to be true before `seeder_test` can talk to
the Hub:

1. Kernel headers matching the running kernel are installed, so the
   out-of-tree Exar driver compiles cleanly.
2. The Exar driver is built **from source** against the running kernel (the
   prebuilt `.ko` shipped in the tarball is for a different kernel).
3. `cdc_acm` is prevented from binding to VID `2890` PID `0213`, and
   `cdc_xr_usb_serial` is told to claim it instead.

If any of those is missing, `seeder_test` fails at boot with:

```
=== ClearCoreClient init ===
ls: cannot access '/dev/ttyXRUSB*': No such file or directory
Init failed: No SC Hub ports found
```

## Symptoms by failure mode

| Symptom | Root cause |
|---|---|
| `Init failed: No SC Hub ports found`, no `/dev/ttyACM*` either, `lsusb` doesn't show `2890:0213` | Hub not powered, bad USB cable (charge-only), or wrong Pi USB port |
| `/dev/ttyACM0` exists but `/dev/ttyXRUSB*` does not | `cdc_acm` claimed the Hub before `xr_usb_serial` could bind |
| `Install_DRV_SCRIPT.sh` errors with `Kernel headers not found at /lib/modules/<ver>/build` | Missing `linux-headers-$(uname -r)` |
| `insmod: ERROR: ... Unknown symbol in module` (e.g. `tty_port_tty_hangup`) | Stale prebuilt `xr_usb_serial_common.ko` from the tarball was inserted instead of being rebuilt against the running kernel |
| Driver loads cleanly but no `/dev/ttyXRUSB*` and `dmesg` shows `cdc_acm 2-1:1.0: ttyACM0: USB ACM device` | `cdc_acm` won the race; need to unbind it and bind `cdc_xr_usb_serial` |
| `/sys/bus/usb-serial/drivers/cdc_xr_usb_serial/new_id: No such file or directory` | Wrong sysfs path. This driver registers with usbcore, not the usb-serial subsystem. Use `/sys/bus/usb/drivers/cdc_xr_usb_serial/new_id` |

## Background — why two drivers fight over the same device

The SC4-Hub presents itself as a USB CDC-ACM communications device. Both
drivers are willing to bind to it:

- `cdc_acm` — in-tree, generic, loads automatically. Exposes `ttyACMx`.
- `cdc_xr_usb_serial` — Teknic's out-of-tree fork of the Exar/MaxLinear
  XR21V/XR21B serial driver. Exposes `ttyXRUSBx`. Required because
  sFoundation's `SysManager::FindComHubPorts` searches specifically for
  `ttyXRUSB*` devices, and the Exar driver implements control transfers
  and baud handling that the generic `cdc_acm` does not.

Whichever driver registers and matches first wins the device. On a fresh
boot, `cdc_acm` is built into the kernel and binds immediately on
enumeration. The Exar module loads later (or only when `modprobe`'d),
finds the device already claimed, and does nothing.

## First-time setup (this is what worked on the seeder Pi)

Running kernel was `6.8.0-1052-raspi` (Ubuntu raspi kernel, not Raspberry
Pi OS). The Teknic driver source lives at
`~/teknic/Linux_Software/Teknic_SC4Hub_USB_Driver/ExarKernelDriver/`.

### 1. Install kernel headers and build tools

```bash
sudo apt update
sudo apt install -y linux-headers-$(uname -r) linux-headers-raspi build-essential
```

Verify the kernel build directory exists (this is the path the install
script checks):

```bash
ls /lib/modules/$(uname -r)/build
```

You should see a populated tree (`Makefile`, `include/`, etc.). If the
`linux-headers-$(uname -r)` package isn't found by apt, install
`linux-headers-raspi` first — it's the meta package that pulls in the
matching version.

> Note: if `apt` upgrades the kernel as a side effect, `uname -r` will
> still report the **old** kernel until you reboot. Always rebuild the
> module against the kernel you'll actually be running.

### 2. Rebuild the Exar driver from source

The tarball contains a prebuilt `xr_usb_serial_common.ko`. Don't trust it
— it was compiled against a different kernel and `insmod` will fail with
`Unknown symbol in module` (we saw `tty_port_tty_hangup err -2`). Clean
and rebuild:

```bash
cd ~/teknic/Linux_Software/Teknic_SC4Hub_USB_Driver/ExarKernelDriver
sudo make -C /lib/modules/$(uname -r)/build M=$PWD clean
sudo make -C /lib/modules/$(uname -r)/build M=$PWD modules
```

A successful build ends with `LD [M] xr_usb_serial_common.ko`. The
`Skipping BTF generation ... due to unavailability of vmlinux` warning is
harmless.

Install it so it's available system-wide and auto-loadable:

```bash
sudo make -C /lib/modules/$(uname -r)/build M=$PWD modules_install
sudo depmod -a
sudo modprobe usbserial
sudo modprobe xr_usb_serial_common
```

(`Install_DRV_SCRIPT.sh` does roughly the equivalent, but on a stale
prebuilt `.ko`. Prefer the manual sequence above on first install.)

Make it auto-load at boot:

```bash
echo xr_usb_serial_common | sudo tee /etc/modules-load.d/xr_usb_serial.conf
```

### 3. Evict cdc_acm and bind cdc_xr_usb_serial

After step 2 the module is loaded but the Hub is still owned by
`cdc_acm`. Confirm:

```bash
ls /dev/ttyACM*           # /dev/ttyACM0 → cdc_acm has the device
ls /dev/ttyXRUSB*         # missing → xr_usb_serial has nothing to talk to
```

The interface address comes from `dmesg` — look for the line that says
`cdc_acm <addr>: ttyACM0: USB ACM device`. On the seeder Pi it was
`2-1:1.0`. Adjust if yours differs.

Unbind from `cdc_acm`:

```bash
echo "2-1:1.0" | sudo tee /sys/bus/usb/drivers/cdc_acm/unbind
```

Then tell the Exar driver to accept this VID/PID and bind:

```bash
echo "2890 0213" | sudo tee /sys/bus/usb/drivers/cdc_xr_usb_serial/new_id
```

> **Path gotcha:** the Teknic driver registers directly with usbcore
> (`usbcore: registered new interface driver cdc_xr_usb_serial` in
> `dmesg`), **not** the usb-serial subsystem. The `new_id` attribute lives
> at `/sys/bus/usb/drivers/cdc_xr_usb_serial/new_id`,
> **not** `/sys/bus/usb-serial/drivers/cdc_xr_usb_serial/new_id`. The
> latter does not exist for this driver and `tee` will silently report
> "No such file or directory."

Confirm:

```bash
ls /dev/ttyXRUSB*         # /dev/ttyXRUSB0
sudo dmesg | tail -10
```

If `new_id` doesn't trigger a bind for some reason, force it directly:

```bash
echo "2-1:1.0" | sudo tee /sys/bus/usb/drivers/cdc_xr_usb_serial/bind
```

### 4. Persist the binding across reboots

The unbind/new_id commands above are scoped to the current kernel
session. On the next reboot `cdc_acm` will grab the Hub again. Install a
udev rule that runs the same dance on every USB add event for
`2890:0213`:

```bash
sudo tee /etc/udev/rules.d/99-teknic-schub.rules > /dev/null <<'EOF'
# Prevent cdc_acm from binding to the Teknic SC-Hub so xr_usb_serial can claim it
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="2890", ATTR{idProduct}=="0213", ATTR{bInterfaceNumber}=="00", RUN+="/bin/sh -c 'echo $kernel > /sys/bus/usb/drivers/cdc_acm/unbind 2>/dev/null; echo 2890 0213 > /sys/bus/usb/drivers/cdc_xr_usb_serial/new_id 2>/dev/null'"
EOF

sudo udevadm control --reload-rules
```

The `2>/dev/null` suppresses errors when the device wasn't bound to
`cdc_acm` to begin with (e.g. on subsequent re-plugs after `new_id` has
already been registered for the lifetime of the kernel).

### 5. Group permissions

The seeder runs as `rooted` and opens `/dev/ttyXRUSB0` directly. The
device node is owned by `root:dialout`, so the user must be in
`dialout`:

```bash
groups | grep -q dialout || sudo usermod -aG dialout $USER
# log out and back in for group membership to take effect
```

### 6. Reboot and verify

```bash
sudo reboot
# after login:
lsmod | grep xr_usb_serial      # cdc_xr_usb_serial / xr_usb_serial_common loaded
ls /dev/ttyXRUSB*               # /dev/ttyXRUSB0
ls /dev/ttyACM*                 # should be empty for the SC-Hub
cd ~/runtime && ./seeder_test
```

`seeder_test` should now print `Init OK, N node(s)` and proceed to enable
nodes.

## Reference: dmesg signature of a healthy bring-up

```
usb 2-1: new full-speed USB device number 3 using xhci-hcd
usb 2-1: New USB device found, idVendor=2890, idProduct=0213
usb 2-1: Product: 4-axis Comm Hub
usb 2-1: Manufacturer: Teknic ClearPat
usb 2-1: SerialNumber: Q...
xr_usb_serial_common: Exar/MxL USB UART (serial port) driver version 1G
cdc_xr_usb_serial 2-1:1.0: ttyXRUSB0: Exar USB device
```

If you see `cdc_acm 2-1:1.0: ttyACM0: USB ACM device` instead, the udev
rule didn't fire — recheck `/etc/udev/rules.d/99-teknic-schub.rules` and
confirm `udevadm control --reload-rules` was run.

## Why `seeder_test` looks for `ttyXRUSB*` specifically

`ClearCoreClient::init()` calls `SysManager::FindComHubPorts()` from
sFoundation. That function globs `/dev/ttyXRUSB*` (you can see this in
`scripts/`-level wrappers and in
`runtime/src/utils/ClearCoreClient.cpp` where `discovered_ports_` is
populated and passed to `ComHubPort()`). Renaming `ttyACM0 → ttyXRUSB0`
via a symlink will not work — sFoundation issues XR-specific control
transfers that `cdc_acm` does not implement, and ioctls will fail.

## What we deliberately did *not* do

- **Did not** blacklist `cdc_acm` globally. It's needed for other CDC-ACM
  devices (modems, dev boards, the harvester's TouchEncoder in some
  configs). The udev rule scopes the eviction to `2890:0213` only.
- **Did not** patch sFoundation to look for `ttyACM*`. The driver
  mismatch isn't just a naming issue; the protocol differs.
- **Did not** rely on `Install_DRV_SCRIPT.sh` alone. It assumes the
  tarball's prebuilt `.ko` matches the running kernel. On the seeder Pi
  it didn't, and the script swallowed the `insmod` failure as a generic
  "Unknown symbol in module."
