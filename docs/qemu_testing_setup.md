Use a full ARM64 Linux VM under `qemu-system-aarch64`. That exercises the pieces this daemon actually depends on: ARM64 binaries, systemd, system D-Bus, udev, sysfs, and USB hotplug. QEMU user-mode emulation alone is insufficient.

Your Apple Silicon host already has QEMU 11.0.2 and ARM64 UEFI firmware installed, so you can use HVF acceleration.

### 1. Create the VM

Download Debian’s current ARM64 netinst ISO from the [official Debian ARM64 installer page](https://www.debian.org/CD/netinst/index.html), then:

```bash
mkdir -p ~/qemu/hotswapd
qemu-img create -f qcow2 ~/qemu/hotswapd/debian-arm64.qcow2 24G
qemu-img create -f raw ~/qemu/hotswapd/usb-stick.raw 512M
```

Set `ISO` to the downloaded ARM64 ISO and start the installer:

```bash
ISO=~/Downloads/debian-13.6.0-arm64-netinst.iso

qemu-system-aarch64 \
  -machine virt,accel=hvf \
  -cpu host \
  -smp 4 \
  -m 4096 \
  -bios /opt/homebrew/share/qemu/edk2-aarch64-code.fd \
  -device virtio-gpu-pci \
  -display cocoa \
  -device qemu-xhci,id=xhci \
  -device usb-kbd,bus=xhci.0 \
  -device usb-tablet,bus=xhci.0 \
  -drive if=none,file="$HOME/qemu/hotswapd/debian-arm64.qcow2",format=qcow2,id=os \
  -device virtio-blk-pci,drive=os \
  -device virtio-scsi-pci,id=scsi \
  -drive if=none,file="$ISO",format=raw,media=cdrom,readonly=on,id=install \
  -device scsi-cd,drive=install,bus=scsi.0 \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22
```

Install Debian, including the SSH server.

The generic QEMU `virt` board is the recommended choice when testing ordinary ARM64 Linux rather than reproducing a particular physical board. [`virt` documentation](https://www.qemu.org/docs/master/system/arm/virt)

### 2. Start the installed VM

Remove the installer ISO devices and expose the USB-stick backend without attaching it yet:

```bash
qemu-system-aarch64 \
  -machine virt,accel=hvf \
  -cpu host \
  -smp 4 \
  -m 4096 \
  -bios /opt/homebrew/share/qemu/edk2-aarch64-code.fd \
  -device virtio-gpu-pci \
  -display cocoa \
  -device qemu-xhci,id=xhci \
  -device usb-kbd,bus=xhci.0 \
  -device usb-tablet,bus=xhci.0 \
  -drive if=none,file="$HOME/qemu/hotswapd/debian-arm64.qcow2",format=qcow2,id=os \
  -device virtio-blk-pci,drive=os \
  -drive if=none,file="$HOME/qemu/hotswapd/usb-stick.raw",format=raw,id=stick \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -monitor stdio
```

The terminal running QEMU is now its monitor. The VM remains accessible through its graphical window and SSH.

### 3. Copy and build the project

From the repository on your Mac:

```bash
scp -P 2222 -r "$PWD" youruser@localhost:~/pda-hotswapd
ssh -p 2222 youruser@localhost
```

Inside Debian:

```bash
sudo apt update
sudo apt install \
  build-essential pkg-config \
  libudev-dev libdbus-1-dev libjson-c-dev \
  dbus systemd usbutils

cd ~/pda-hotswapd
make clean
make
make test

uname -m
file hotswapd
```

The last two commands should report `aarch64`/ARM64.

Install and start it:

```bash
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now hotswapd.service

systemctl status hotswapd.service
sudo journalctl -u hotswapd.service -f
```

In another SSH session:

```bash
hsctl list
hsctl power
hsctl monitor
```

### 4. Simulate USB insertion and removal

At the QEMU monitor prompt, attach the virtual USB drive:

```text
device_add usb-storage,bus=xhci.0,drive=stick,id=stickdev,serial=HOTSWAP001
```

Inside the guest, verify the event:

```bash
lsusb
lsblk
udevadm monitor --udev --property --subsystem-match=usb
```

Remove it from the QEMU monitor:

```text
device_del stickdev
```

Reattach with the same `device_add` command. QEMU officially supports adding USB devices through `device_add` and removing them with `device_del`. [QEMU USB hotplug documentation](https://www.qemu.org/docs/master/system/devices/usb.html)

### 5. Exercise the storage-specific path

There is a project-specific wrinkle: emulated `usb-storage` commonly reports its USB device class as per-interface (`00`). Your code only infers storage from `bDeviceClass == 08`; otherwise it needs a registry match.

After attaching, determine QEMU’s IDs:

```bash
sudo udevadm info --query=property --name=/dev/sda |
  grep -E 'ID_VENDOR_ID|ID_MODEL_ID'
```

Add those IDs to `/etc/hotswapd/modules.json` as a `"storage"` entry, then format and mount the virtual drive:

```bash
sudo mkfs.ext4 /dev/sda
sudo mkdir -p /mnt/hotswap-test
sudo mount /dev/sda /mnt/hotswap-test
echo test | sudo tee /mnt/hotswap-test/test.txt
sudo systemctl restart hotswapd.service
```

Restarting while the drive is mounted causes startup enumeration to record the mount. Then issue:

```text
device_del stickdev
```

Check that the daemon observed the detach and attempted its sync/lazy-unmount behavior:

```bash
sudo journalctl -u hotswapd.service --since '2 minutes ago'
findmnt /mnt/hotswap-test
hsctl list
```

QEMU’s direct `device_del` tests the unclean-detach path. The clean path is
entered by the GPIO safe-release input, which must be tested on the CM5 (or by
injecting an equivalent GPIO character-device event in a dedicated test
environment). QEMU also will not validate CM5-specific USB-C PD sysfs behavior
or physical surprise-removal electrical characteristics. Those still require
real hardware.
