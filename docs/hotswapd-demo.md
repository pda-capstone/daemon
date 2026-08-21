# hotswapd Mass-Storage Demonstration Guide

This runbook demonstrates `hotswapd` on a CM5 using one USB mass-storage
module. It prioritizes the original requirements around service operation,
USB attach/detach detection, module registry lookup, D-Bus UI notifications,
power visibility, storage cleanup, GPIO safe release, and graceful handling of
an explicitly controlled surprise removal.

This is a focused demonstration of one module type. It does not, by itself,
satisfy the separate validation criterion requiring three distinct physical
module types.

## Demonstration Goals

Show that:

1. `hotswapd` runs as a systemd service and owns its system D-Bus name.
2. A connected USB drive is detected, classified as storage, and mounted.
3. `hsctl` exposes its identity, state, maximum USB power, and link speed.
4. D-Bus signals provide UI-consumable attach, power, ready, and detach events.
5. Opening the GPIO release contact flushes and unmounts storage before removal.
6. The daemon remains active across repeated attach/detach cycles.
7. The optional surprise-removal test is detected as unclean and does not crash
   the daemon.

## Hardware and Software

- Raspberry Pi Compute Module 5 target
- USB mass-storage device with a supported filesystem
- removable jumper or normally-closed release contact wired between:
  - physical pin 37: GPIO26 input
  - physical pin 39: ground
- GPIO header voltage set to 3.3 V
- Project uploaded to `~/pda-hotswapd`
- Build dependencies from the main `README.md` installed
- `hotswapd` and `hsctl` built for and installed on the CM5

Never connect the GPIO input to 5 V. Do not physically remove mounted storage
until the script reports `READY` unless intentionally performing the controlled
unclean-removal test.

## 1. Upload the Project

From the project directory on the development computer:

```bash
rsync -av \
  --exclude='.git/' \
  --exclude='.DS_Store' \
  --exclude='*.o' \
  --exclude='/docs/' \
  --exclude='/hotswapd' \
  --exclude='/hsctl' \
  ./ alex@pda-cm5.local:~/pda-hotswapd/
```

The `/docs/` exclusion means this guide remains on the development
computer. Remove that exclusion if a copy of the guide is also wanted on the
CM5.

## 2. Prepare the Storage Device

Begin the guided portion with the USB mass-storage module disconnected and the
release jumper installed (or the normally-closed contact closed). The input
must first be held low so opening it can produce a rising edge.

If it is already connected and mounted, do not simply unplug it. Open the GPIO
release contact or remove its jumper and wait for the ready indication, or
unmount it explicitly before disconnecting it.

Only one hot-swap module should be connected during the demonstration unless
the service has been configured with a release DEVPATH prefix. Without a
prefix, the GPIO release operation intentionally refuses to select between
multiple attached modules.

## 3. Build and Install

Connect to the target:

```bash
ssh alex@pda-cm5.local
```

If `/etc/hotswapd/modules.json` contains target-specific changes, back it up
before installation:

```bash
cp /etc/hotswapd/modules.json ~/modules.json.pre-demo
```

Build and run the automated tests:

```bash
cd ~/pda-hotswapd
make clean
make
make test
```

Install and prepare the system service:

```bash
sudo make install

sudo systemctl daemon-reload
sudo systemctl enable hotswapd.service
sudo systemctl restart hotswapd.service

systemctl --no-pager --full status hotswapd.service
busctl --system list | grep org.postmarketos.HotSwap
hsctl list
hsctl power
```

Expected results:

- the build and tests complete successfully
- `hotswapd.service` is active
- `org.postmarketos.HotSwap` appears on the system bus
- `hsctl list` reports no demo module while the drive is disconnected
- `hsctl power` reports the current tracked USB power total

`sudo make install` replaces `/etc/hotswapd/modules.json` with the repository
copy. Preserve and restore the backup if the target registry has custom entries
that should not be lost.

## 4. Open Two SSH Terminals

### Terminal 1: live daemon log

```bash
ssh alex@pda-cm5.local
journalctl -u hotswapd.service -f
```

Leave this running during the demonstration. Press `Ctrl+C` afterward.

### Terminal 2: guided demonstration

```bash
ssh alex@pda-cm5.local
cd ~/pda-hotswapd
scripts/demo-hotswapd.sh --output "$HOME/hotswapd-demo-evidence"
```

The explicit output directory retains the session transcript, D-Bus event log,
service journal, and kernel journal after the demonstration. Without
`--output`, the script creates a session directory under `/tmp`.

To check the setup without performing physical cycles:

```bash
scripts/demo-hotswapd.sh --preflight-only
```

## 5. Standard Safe-Release Demonstration

The script first verifies:

- Linux and required command availability
- active systemd service
- whether the service is enabled at boot
- ownership of the D-Bus service name
- readable module registry
- current module and power state

It then starts `hsctl monitor` in the background.

### Attach

1. Install the jumper or close the release contact so GPIO26 is held low.
2. When prompted, connect the USB mass-storage module.
3. Wait briefly for udev discovery, classification, and mounting.
4. Press Enter in the guided terminal.

Expected evidence includes:

```text
[ATTACH] ... Category: storage ... MaxPower: ... Speed: ...
[POWER ] Total Power Draw: ... Connected Devices: 1
Storage classification and automatic mount: PASS
```

The detailed `hsctl info` output should show:

- the USB DEVPATH and sysfs path
- vendor and product IDs
- product name and serial number when provided by the device
- `category: storage`
- `state: attached`
- maximum power and speed
- `mount_count` greater than zero

The daemon log should show the discovered block device and tracked mount point,
normally `/run/media/hotswapd/<device>`.

### Safe release

1. When prompted, open the release contact or remove the jumper between pins
   37 and 39.
2. Do not unplug the drive yet.
3. Wait for the script to display the ready event.

Expected evidence:

```text
[READY ] ... Safe to remove now
Tracked filesystems safely unmounted: yes
```

The script queries the daemon again and refuses to recommend removal unless
`mount_count` is zero. A `[FAILED]` event or timeout means the drive must remain
connected.

### Physical removal

After `READY` is confirmed:

1. Physically remove the USB module.
2. Press Enter when prompted.

Expected evidence:

```text
[DETACH] ... Clean detach: YES
Cycle result: PASS (daemon remains active)
```

This demonstrates the intended mechanical sequence:

```text
open release contact -> rising edge -> sync -> normal unmount -> READY -> physical removal
```

## 6. Five-Cycle Storage Validation

To repeat the storage-focused test five times:

```bash
scripts/demo-hotswapd.sh \
  --validation \
  --output "$HOME/hotswapd-validation-evidence"
```

For every cycle, confirm:

- one attach event is received
- storage is classified and mounted
- power information remains available
- GPIO release produces `READY`
- the tracked mount count becomes zero
- physical removal is reported clean
- `hotswapd.service` remains active

The summary should report:

```text
Hardware cycles passed: 5/5
Daemon still active: yes
```

## 7. Optional Controlled Unclean-Removal Test

This step is intentionally opt-in because surprise removal always carries some
risk. Use a disposable drive or a drive whose contents are backed up.

Run:

```bash
scripts/demo-hotswapd.sh \
  --unclean-storage-test \
  --output "$HOME/hotswapd-unclean-evidence"
```

After the normal safe-release cycle, the script will:

1. ask for the storage module to be connected again
2. locate its mount under `/run/media/hotswapd`
3. write an 8 MiB random payload and checksum file
4. call `sync` explicitly
5. require the presenter to type `REMOVE` before surprise removal
6. verify that D-Bus reports the detach as unclean
7. confirm that the daemon remains active
8. ask for the drive to be reconnected
9. verify the saved checksum
10. delete and sync the demonstration payload
11. ask for a final GPIO safe release

Expected detach evidence:

```text
[DETACH] ... Clean detach: NO (UNCLEAN - potential data loss)
```

Expected verification evidence after reconnection:

```text
payload.bin: OK
Synced payload verification: PASS
```

This proves that the explicitly synced sample survived that test. It does not
prove that arbitrary unsynced writes can survive physical removal, and the
daemon should continue to warn about that risk.

## 8. Evidence and Closing Checks

At the end of the demonstration, retain the output directory printed by the
script. It contains:

- `session-*.log`: complete guided-session transcript
- `events-*.log`: human-readable `hsctl monitor` D-Bus events
- `journal-*.log`: daemon service log for the session
- `kernel-*.log`: kernel log for the session

Run these closing checks:

```bash
systemctl is-active hotswapd.service
hsctl list
hsctl power
journalctl -u hotswapd.service --since today --no-pager
```

Confirm that the service is still active and that no removed module remains in
the daemon's tracked runtime state. Review the saved kernel journal for new USB,
filesystem, or I/O errors rather than inferring stability only from the daemon
output.

## Troubleshooting

### Service does not start

```bash
systemctl --no-pager --full status hotswapd.service
journalctl -u hotswapd.service -n 100 --no-pager
```

Confirm that `/usr/sbin/hotswapd`, the registry, and the D-Bus policy were
installed.

### D-Bus name is absent

Confirm that the service is active and inspect its journal for a failure to
acquire `org.postmarketos.HotSwap`.

### Attach signal is not observed

```bash
lsusb
udevadm monitor --kernel --udev --subsystem-match=usb
```

Reconnect the drive and verify that the kernel and udev both see it.

### Storage is detected but not mounted

Check:

```bash
hsctl list
hsctl info <devpath>
lsblk -o NAME,TRAN,SIZE,FSTYPE,LABEL,MOUNTPOINTS
journalctl -u hotswapd.service -n 100 --no-pager
```

Confirm that the registry has a storage mount action or storage defaults, that
`mount` is installed, and that the filesystem is supported.

### GPIO release does not produce READY

Inspect the service journal for GPIO initialization or release-selection
errors. Confirm the pin-37/pin-39 wiring and ensure only one module is tracked.
If multiple modules are present, configure a stable release DEVPATH prefix.

### Release reports FAILED

Do not remove the module. Close programs or terminals using its mount point and
retry. The daemon deliberately refuses to declare the device ready when sync or
normal unmount fails.

## Presenter Summary

The most important result is not merely that Linux detects a USB drive. The
demonstration shows that a long-running user-space service identifies it,
applies registry policy, publishes state for a UI, accounts for its USB power,
and coordinates a physical release contact with filesystem synchronization and
unmounting. It also distinguishes a prepared removal from a surprise removal
and remains operational after both paths.
