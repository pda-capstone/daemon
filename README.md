# hotswapd

`hotswapd` is a C user-space daemon for Pocket Distro Alpha. It monitors USB
attach/detach events with `libudev`, tracks known module types from a JSON
registry, publishes state on the D-Bus system bus, and performs conservative
storage cleanup for removable devices. On CM5 hardware it can also use an
active-low GPIO release switch to flush and unmount a module before removal.

The daemon is systemd-managed in this version of the project. It remains a
normal user-space process; it does not require kernel modules or kernel-space
hot-swap logic.

## Components

- `hotswapd`: long-running daemon
- `hsctl`: CLI for `list`, `info`, `power`, and `monitor`
- `config/hotswapd.service`: systemd unit
- `config/hotswapd.conf`: D-Bus policy
- `config/modules.json`: example module registry

## D-Bus API

Identity:

- Bus name: `org.postmarketos.HotSwap`
- Object path: `/org/postmarketos/HotSwap`
- Interface: `org.postmarketos.HotSwap`

Signals:

- `ModuleAttached(sssssuu)`
- `ModuleDetached(ssb)`
- `PowerChanged(uu)`
- `ModuleReadyForRemoval(ss)`
- `ModuleReleaseFailed(ss)`

Methods:

- `ListModules() -> a(ssssu)`
- `GetModuleInfo(s) -> a{sv}`
- `GetTotalPowerDraw() -> u`

## Build And Test

Build locally:

```sh
make clean
make
make test
systemd-analyze verify config/hotswapd.service
```

### ARM64 QEMU

The host-side QEMU runner can perform the one-time Debian installation, install
guest build dependencies, and run repeatable ARM64 builds and unit tests:

```sh
# One-time interactive installation; select the SSH server package.
scripts/qemu-arm64-test.sh install ~/Downloads/debian-arm64-netinst.iso

# One-time guest dependency installation.
scripts/qemu-arm64-test.sh provision --user <guest-user>

# Repeatable headless ARM64 build and test run.
scripts/qemu-arm64-test.sh test --user <guest-user>
# Repeatable headless ARM64 build and test run for `--user alex`
scripts/qemu-arm64-test.sh test --user alex
```

The `install` command refuses to run when the VM disk already exists. Use
`--force-install` only when intentionally reinstalling or repairing the guest;
it preserves the existing disk and boots the installer against it.

SSH key authentication makes the test command unattended. Use `--identity` if
the guest key is not one of the standard SSH identities. Run
`scripts/qemu-arm64-test.sh --help` for path, port, and VM lifecycle options.

`make test` is hardware-free. It currently covers:

- registry parsing and reload behavior
- device-state add/find/remove behavior
- legacy power parsing, including `speed=1.5`
- storage device-name extraction for `sdX`, `sdX1`, `mmcblk0p1`, and `nvme0n1p1`

## Install

The install target supports `DESTDIR` and standard path overrides.

```sh
sudo make install
```

Installed paths:

- `/usr/sbin/hotswapd`
- `/usr/bin/hsctl`
- `/etc/hotswapd/modules.json`
- `/etc/dbus-1/system.d/hotswapd.conf`
- `/usr/lib/systemd/system/hotswapd.service`

`make install` does not enable or start the service automatically.

## systemd

The daemon runs in foreground mode under systemd:

```ini
[Service]
Type=dbus
BusName=org.postmarketos.HotSwap
ExecStart=/usr/sbin/hotswapd -f
```

Typical target-system workflow:

```sh
sudo make install
sudo systemctl daemon-reload
sudo systemctl restart hotswapd.service
systemctl status hotswapd.service
busctl --system list | grep org.postmarketos.HotSwap
hsctl list
hsctl power
journalctl -u hotswapd.service -f
```

## Behavior Notes

### GPIO safe release (CM5 IO Board)

The default input is GPIO26, which is physical pin 37 on the 40-pin header.
Wire a normally-open switch between physical pin 37 (GPIO26) and physical pin
39 (ground). The daemon requests an internal pull-up and a 50 ms debounce, so a
press produces a falling edge. Ensure the CM5 IO Board GPIO-voltage selector is
set to 3.3 V for normal HAT-header use, and never connect this input to 5 V. For
a production carrier, add an external pull-up (for example, 10 kΩ to the
selected GPIO rail) so the line has a defined level before Linux starts.

GPIO monitoring is enabled by default but is non-fatal when no suitable GPIO
character device exists. It uses the Linux GPIO v2 character-device API, not
the deprecated sysfs GPIO interface. Options are:

```text
--gpio-chip auto|/dev/gpiochipN
--gpio-line N
--release-devpath-prefix PREFIX
--no-gpio-release
```

`auto` locates the Raspberry Pi GPIO controller by its line names or controller
label, avoiding assumptions about the unstable `gpiochipN` number. If exactly
one USB module is attached, no DEVPATH prefix is required. When multiple
devices are present, configure the stable USB-port portion of the module's
DEVPATH (shown by `hsctl list`) with `--release-devpath-prefix`; every tracked
device below that USB topology prefix is prepared together.

On a press, storage filesystems are synced and normally unmounted. Only after
all cleanup succeeds does the device enter `detaching` state and emit
`ModuleReadyForRemoval`. An unmount or sync error emits `ModuleReleaseFailed`;
the module must not be removed. A switch alone cannot physically prevent early
removal, so the mechanical design should require the button to be pressed and
the user should wait for a UI/LED indication driven by the ready signal.

- Storage cleanup is conservative. A requested release uses `syncfs()` and a
  normal unmount while the device is present; surprise removal only cleans up
  still-matching stale mount points with a lazy unmount.
- The daemon minimizes data-loss risk, but it cannot guarantee zero data loss
  after sudden physical removal.
- Registry defaults apply when no exact `(vendor_id, product_id)` match exists.
- Exact registry categories override USB descriptor inference. Otherwise the
  daemon uses the device class and then interface classes; storage wins the
  fixed priority for composite devices so cleanup policy is retained.
- Storage `mount` actions are only executed when explicitly configured. Block
  nodes are discovered asynchronously for up to 10 seconds and associated
  with their USB parent through sysfs before mounting.
- Registry reload is designed to survive normal editor atomic replacement.
- USB speed is exposed as integer Mbps. Fractional sysfs values such as `1.5`
  are rounded to the nearest whole Mbps for the public D-Bus API.
- USB-C PD reporting is intentionally conservative. If the code cannot tie PD
  sysfs data to the actual attached device, it reports no PD data rather than
  risking incorrect attribution.

## Validation Scope

Local automated tests do not prove end-to-end hardware behavior. The following
items still require target validation on CM5 or equivalent hardware:

- repeated attach/detach cycling across at least three module types
- real storage surprise-removal behavior
- D-Bus/systemd behavior on the installed target image
- GPIO26 falling-edge detection, debounce, and ready/failed indication
- any accurate per-device USB-C PD reporting

# License

Copyright (C) 2026 Alex Olivier

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
