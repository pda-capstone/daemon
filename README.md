# hotswapd

`hotswapd` is a C user-space daemon for Pocket Distro Alpha. It monitors USB
attach/detach events with `libudev`, tracks known module types from a JSON
registry, publishes state on the D-Bus system bus, and performs conservative
storage cleanup for removable devices. On CM5 hardware it can also use a
rising-edge GPIO release contact to flush and unmount a module before removal.

The daemon is systemd-managed in this version of the project. It remains a
normal user-space process; it does not require kernel modules or kernel-space
hot-swap logic.

## Components

- `hotswapd`: long-running daemon
- `hsctl`: CLI for device status, registry inspection/registration, and events
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
- `ListRegistry() -> a(sssss)`
- `RegisterModule(sbsss) -> sssssb` (root callers only)

## Dependencies

The daemon links directly against these libraries:

- `libudev` for USB enumeration and attach/detach events
- `libdbus-1` for the system D-Bus API and UI notifications
- `json-c` for parsing the module registry
- the standard C library and Linux userspace headers

Building also requires GCC, Make, and `pkg-config`. GPIO safe release uses the
Linux GPIO v2 ioctl API from `<linux/gpio.h>` directly, so `libgpiod` is not a
dependency. The `hsctl` client only links directly against `libdbus-1` and the
standard C library.

On Debian, Raspberry Pi OS, or another Debian-based CM5 image, install the
build dependencies with:

```sh
sudo apt update
sudo apt install \
  build-essential \
  pkg-config \
  libudev-dev \
  libdbus-1-dev \
  libjson-c-dev \
  linux-libc-dev
```

At runtime, the target needs `udev`, a running system D-Bus, and systemd for
the supplied service unit. `util-linux` supplies the external `mount` command
used for configured storage mount actions:

```sh
sudo apt install systemd dbus udev util-linux
```

Install filesystem-specific tools as needed; for the exFAT demonstration
drive, use:

```sh
sudo apt install exfatprogs
```

`jq` is optional. The demonstration script uses it for formatted registry
validation when available and falls back to basic output otherwise.

Confirm the direct development libraries and inspect runtime linkage with:

```sh
pkg-config --modversion libudev dbus-1 json-c
ldd ./hotswapd
ldd ./hsctl
```

Every entry reported by `ldd` should resolve to a path; any `not found` entry
indicates a missing runtime library.

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
- storage device-name extraction for `sdX`, `sdX1`, `mmcblk0p1`, and
  `nvme0n1p1`, plus CM5-style sysfs USB-parent resolution
- GPIO rising-edge filtering, userspace debounce, fd ownership, malformed
  events, and CM5 RP1 controller selection

### Target-hardware demonstration

On an installed CM5 target, the guided mass-storage demonstration script
prioritizes the requirements-level behaviors: systemd service health, registry
visibility, D-Bus notifications for the UI, storage identification and power
reporting, automatic mounting, GPIO safe release, repeated cycling, and an
optional controlled storage surprise-removal test.

```sh
# Short presentation: one USB mass-storage safe-release cycle.
scripts/demo-hotswapd.sh

# Storage-focused validation session: five USB mass-storage cycles.
scripts/demo-hotswapd.sh --validation

# Explicitly include the controlled unclean-storage-removal demonstration.
scripts/demo-hotswapd.sh --unclean-storage-test
```

The script uses the installed `hotswapd.service` and `hsctl`, prompts before
every physical action, refuses to recommend removal unless the GPIO path emits
`ModuleReadyForRemoval`, and saves the session, D-Bus events, and journal in an
evidence directory. Run `scripts/demo-hotswapd.sh --help` for overrides and
focused storage-test options.

This script is intentionally scoped to one mass-storage module type. The
original project criterion covering three distinct physical module types still
requires separate HID and serial (or equivalent) validation evidence.

See [docs/hotswapd-demo.md](docs/hotswapd-demo.md) for the complete
presenter runbook, expected output, safety notes, and troubleshooting steps.

For visual overviews of the system boundary, components, event loop, attach and
detach flows, storage safety behavior, registry updates, and D-Bus contract, see
[docs/hotswapd-diagrams.md](docs/hotswapd-diagrams.md).

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
sudo systemctl enable --now hotswapd.service
systemctl status hotswapd.service
busctl --system list | grep org.postmarketos.HotSwap
hsctl list
hsctl power
journalctl -u hotswapd.service -f
```

The `enable --now` command both starts the service immediately and enables it
to start automatically on future boots. This is a one-time setup step after
installation; the daemon does not need to be started manually after each boot.

## Registering a Connected Module

Use the DEVPATH shown by `hsctl list` to add a currently connected device to
the registry. Registry listing is read-only and does not require root:

```sh
hsctl registry
sudo hsctl register <devpath>
```

An existing VID/PID is rejected unless replacement is explicit. Metadata can
also be overridden while registering:

```sh
sudo hsctl register <devpath> --replace \
  --name "SanDisk Ultra" \
  --category storage \
  --description "128 GB demonstration drive"
```

Valid categories are `storage`, `hid`, `serial`, `network`, `audio`, `video`,
and `hub`. The daemon—not `hsctl`—performs the privileged update. It resolves
the hardware identity from its connected-device state, locks the registry,
writes and flushes a same-directory temporary file, atomically replaces the
original while preserving its permissions, and reloads the result. The
existing `defaults` section and any per-device action fields are retained.
Registration normally takes effect on the device's next attachment.

## Behavior Notes

### GPIO safe release (CM5 IO Board)

The default input is GPIO26, which is physical pin 37 on the 40-pin header.
Install a removable jumper or normally-closed release contact between physical
pin 37 (GPIO26) and physical pin 39 (ground). The daemon requests an internal
pull-up, rising-edge events, and a 50 ms debounce. With the contact installed,
GPIO26 is held low; opening the contact or removing the jumper lets the pull-up
drive it high and triggers safe release. Ensure the CM5 IO Board GPIO-voltage
selector is set to 3.3 V for normal HAT-header use, and never connect this input
to 5 V. For a production carrier, add an external pull-up (for example, 10 kΩ
to the selected GPIO rail) so the line has a defined level before Linux starts.

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

When the contact opens, storage filesystems are synced and normally unmounted.
Only after all cleanup succeeds does the device enter `detaching` state and emit
`ModuleReadyForRemoval`. An unmount or sync error emits `ModuleReleaseFailed`;
the module must not be removed. A detection contact alone cannot prevent early
removal, so the mechanical design should open the detection contact first and
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
- GPIO26 rising-edge detection, debounce, and ready/failed indication
- any accurate per-device USB-C PD reporting

See:

- [HANDOFF.md](HANDOFF.md)
- [docs/hotswapd-requirements-traceability.md](docs/hotswapd-requirements-traceability.md)
- [docs/hotswapd-validation-checklist.md](docs/hotswapd-validation-checklist.md)

## License

Copyright (C) 2026 Alexander Olivier

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

SPDX license identifier: `GPL-3.0-or-later`. See [LICENSE](LICENSE) for the
complete license text.
