# Harvester Pi Setup

Bring-up for the harvester Raspberry Pi and Teknic ClearPath-SC hardware.

This document reflects the path that was actually validated on the target Pi:

- Raspberry Pi running Ubuntu 24.04 with kernel `6.8.0-1047-raspi`
- Teknic 4-axis SC Hub on USB `2890:0213`
- Teknic `sFoundation` Linux SDK
- Exar USB serial driver loaded as `cdc_xr_usb_serial`

## What matters

The SC Hub may enumerate under the kernel's built-in `cdc_acm` driver as `/dev/ttyACM0`, but that is not sufficient for `sFoundation`.

`HelloWorld` only worked after the Teknic/Exar driver was installed and the hub interfaces were bound to `cdc_xr_usb_serial`, which exposes `/dev/ttyXRUSB0`.

Do not use a `ttyXRUSB0` symlink to `/dev/ttyACM0`. That masks the problem and still fails the `sFoundation` handshake.

## 1. Hardware prerequisites

- Raspberry Pi with Ubuntu 24.04
- Teknic SC Hub connected over USB
- At least one ClearPath-SC motor connected to the hub
- User account in the `dialout` group

Verify group membership:

```bash
groups
```

If `dialout` is missing:

```bash
sudo usermod -aG dialout "$USER"
```

Log out and back in after changing groups.

## 2. Download Teknic's Linux bundle

```bash
mkdir -p ~/teknic
cd ~/teknic
wget https://teknic.com/files/downloads/Linux_Software.tar.gz
tar xzf Linux_Software.tar.gz
cd Linux_Software
```

Important contents:

- `sFoundation.tar`: userspace SDK we build and run
- `Teknic_SC4Hub_USB_Driver.tar`: kernel driver package we do need on this hub
- `doc/`, `readme.txt`, license files

## 3. Install build prerequisites

```bash
sudo apt update
sudo apt install -y build-essential dkms linux-headers-$(uname -r)
```

Explanation:

- `build-essential`: compiler and make
- `linux-headers-$(uname -r)`: required to build the kernel module for the running Pi kernel
- `dkms`: useful if Teknic's installer uses it, and generally helpful for kernel-module rebuilds

## 4. Build and install the Exar USB serial driver

Extract the driver package and run Teknic's installer:

```bash
cd ~/teknic/Linux_Software
tar xf Teknic_SC4Hub_USB_Driver.tar
cd Teknic_SC4Hub_USB_Driver
sudo ./Install_DRV_SCRIPT.sh
```

If the package layout differs slightly, find the installer first:

```bash
find ~/teknic/Linux_Software -iname 'Install_DRV_SCRIPT.sh'
```

Verify the module exists and matches the running kernel:

```bash
modinfo xr_usb_serial_common | grep -iE 'filename|alias|vermagic'
```

Expected details:

- module path under `/lib/modules/$(uname -r)/kernel/drivers/usb/serial/`
- alias list includes `usb:v2890p0213`
- `vermagic` matches the running Pi kernel

When loaded, the driver registers on the USB bus as `cdc_xr_usb_serial`:

```bash
sudo modprobe xr_usb_serial_common
sudo dmesg | tail -20
ls /sys/bus/usb/drivers/
```

Expected `dmesg` lines:

```text
usbcore: registered new interface driver cdc_xr_usb_serial
xr_usb_serial_common: Exar/MxL USB UART (serial port) driver version 1G
```

And `/sys/bus/usb/drivers/` should include `cdc_xr_usb_serial`.

## 5. Confirm the hub is owned by the Exar driver

Check USB binding:

```bash
lsusb -t
```

Expected:

```text
|__ Port 002: Dev 00X, If 0, Class=Communications, Driver=cdc_xr_usb_serial, 12M
|__ Port 002: Dev 00X, If 1, Class=CDC Data, Driver=cdc_xr_usb_serial, 12M
```

Check device nodes:

```bash
ls -l /dev/ttyXRUSB* /dev/ttyACM* 2>/dev/null
```

Expected:

- `/dev/ttyXRUSB0` exists as a real character device
- `/dev/ttyACM0` is absent once the Exar driver owns the hub

If the hub is still on `cdc_acm`, manually rebind once to prove the path:

```bash
echo -n "4-2:1.0" | sudo tee /sys/bus/usb/drivers/cdc_acm/unbind
echo -n "4-2:1.0" | sudo tee /sys/bus/usb/drivers/cdc_xr_usb_serial/bind
```

The exact interface name may differ from `4-2:1.0`; use the value shown by `lsusb -t` and `/sys/bus/usb/drivers/cdc_acm/`.

In the validated setup, rebinding interface `1.0` was enough for the Exar driver to claim both interfaces and create `/dev/ttyXRUSB0`.

## 6. Make the binding persistent with udev

`cdc_acm` can grab the hub first during boot. Install a udev rule that flips interface `00` over to `cdc_xr_usb_serial`.

Create `/etc/udev/rules.d/99-teknic-schub.rules` with this single line:

```text
ACTION=="add", SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_interface", ATTRS{idVendor}=="2890", ATTRS{idProduct}=="0213", ATTR{bInterfaceNumber}=="00", DRIVER=="cdc_acm", RUN+="/bin/sh -c 'echo -n %k > /sys/bus/usb/drivers/cdc_acm/unbind; echo -n %k > /sys/bus/usb/drivers/cdc_xr_usb_serial/bind'"
```

Then reload udev and reboot:

```bash
sudo udevadm control --reload
sudo reboot
```

Post-reboot verification:

```bash
ls -l /dev/ttyXRUSB* /dev/ttyACM* 2>/dev/null
lsusb -t
```

Validated-good state:

- `/dev/ttyXRUSB0` exists immediately after boot
- no `/dev/ttyACM0`
- both hub interfaces show `Driver=cdc_xr_usb_serial`

## 7. Build sFoundation

Extract the SDK:

```bash
cd ~/teknic/Linux_Software
tar xf sFoundation.tar
```

Build the library:

```bash
cd ~/teknic/Linux_Software/sFoundation/sFoundation
make
```

Install the shared library and driver XML:

```bash
sudo cp MNuserDriver20.xml libsFoundation20.so /usr/local/lib
sudo ldconfig
ldconfig -p | grep sFoundation
```

Expected:

```text
libsFoundation20.so.1 (libc6,aarch64) => /usr/local/lib/libsFoundation20.so.1
```

If `/usr/local/lib` is not in the linker path:

```bash
echo "/usr/local/lib" | sudo tee -a /etc/ld.so.conf
sudo ldconfig
```

## 8. Run Teknic's HelloWorld example

Build and run:

```bash
cd ~/teknic/Linux_Software/sFoundation/SDK_Examples/HelloWorld
make
./HelloWorld
```

Validated output on the target Pi:

```text
Hello World, I am SysManager
Found 1 SC Hubs
 Port[0]: state=5, nodes=1
```

Interpretation:

- the Pi opened the SC Hub successfully

## 9. Field reliability risk: kernel updates can break SC discovery

The Teknic/Exar SC Hub driver is an out-of-tree kernel module. If the Pi boots into a
new kernel that does not have a matching `xr_usb_serial_common` build installed, the hub
will typically fall back to the kernel's built-in `cdc_acm` driver.

When that happens:

- the hub enumerates as `/dev/ttyACM0` instead of `/dev/ttyXRUSB0`
- `sFoundation` does not discover the SC Hub
- the harvester runtime fails at startup with `No SC Hub ports found`
- motors do not come online

This is a machine-down condition after reboot, not a motion-safety runaway condition. The
expected failure mode is that startup aborts cleanly and the machine remains unavailable
until the driver is restored for the running kernel.

### How to recognize this failure

Symptoms:

- `ls /dev/ttyXRUSB*` shows nothing
- `ls /dev/ttyACM*` shows `/dev/ttyACM0`
- `modinfo xr_usb_serial_common` returns nothing or does not match `uname -r`
- `dmesg` shows the SC Hub bound to `cdc_acm`

Example bad state:

```text
usb 4-2: New USB device found, idVendor=2890, idProduct=0213
usb 4-2: Product: 4-axis Comm Hub
cdc_acm 4-2:1.0: ttyACM0: USB ACM device
```

### Recovery

Check the running kernel and whether the Exar module exists:

```bash
uname -r
modinfo xr_usb_serial_common 2>/dev/null | grep -iE 'filename|alias|vermagic'
lsmod | grep -E 'xr_usb|cdc_xr|cdc_acm'
```

If `modinfo xr_usb_serial_common` returns nothing, the module is not installed for the
current kernel. Rebuild/reinstall the Teknic driver against the running kernel, then:

```bash
sudo modprobe xr_usb_serial_common
```

Unplug/replug the hub and verify:

```bash
ls /dev/ttyXRUSB*
lsusb -t
```

### Production mitigations

For field machines, do not assume the SC driver survives arbitrary kernel updates. Use one
or more of the following:

- pin the Pi kernel to a tested version
- disable unattended kernel upgrades on deployed machines
- package the Exar driver with DKMS if feasible so it rebuilds on kernel changes
- add a boot-time/service startup health check that verifies:
  - `xr_usb_serial_common` exists
  - `/dev/ttyXRUSB*` exists
  - `sFoundation` can discover the SC Hub
- fail clearly and raise an alert if the system comes up on `ttyACM0`

The key operational rule is: a successful SC setup on one boot does not guarantee the
driver will still be present after a kernel change.
- `sFoundation` enumerated the hub
- one node was found on the bus
- port state `5` indicates the port is ready

This is the minimum bring-up milestone. After this, run one of Teknic's motion examples to prove the motor actually moves.

## 9. Real exit criterion

`HelloWorld` proves enumeration. It does not prove motion.

Phase 1 is only done when a Teknic example or our own smoke binary can:

- open the SC Hub through `cdc_xr_usb_serial`
- enumerate at least one ClearPath-SC node
- send a move command
- read back position or status successfully

## Known-good facts

- USB ID: `2890:0213`
- Required kernel module file: `xr_usb_serial_common.ko`
- Registered USB driver name: `cdc_xr_usb_serial`
- Working device node: `/dev/ttyXRUSB0`
- Failing path: `/dev/ttyACM0` via `cdc_acm`
- `sFoundation` handshake failure under the wrong driver showed up in `lnkAccessCommon.cpp:832`

## Do not waste time on these again

- Do not assume `cdc_acm` is good enough because the hub enumerates.
- Do not create a symlink from `/dev/ttyXRUSB0` to `/dev/ttyACM0`.
- Do not spend time on ModemManager workarounds before the Exar driver is bound correctly.
