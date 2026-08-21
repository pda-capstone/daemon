# hotswapd Maintainer Handoff

This document is the self-contained starting point for the next engineer
responsible for `hotswapd`. It describes the implementation at version 0.2.0,
how to build, deploy, operate, validate, and change it safely, and what has not
yet been proven on target hardware. It intentionally does not depend on a
separate architecture document, demo runbook, validation checklist, or QEMU
helper being present in the repository.

It assumes only the core source tree: `src/`, `include/`, `config/`, `tests/`,
and the `Makefile`. Any additional documentation or scripts may help, but they
are not required to follow this handoff.

## What You Are Taking Over

`hotswapd` is a C user-space daemon for Pocket Distro Alpha. It runs as a root
systemd service, watches USB devices through libudev, maintains an in-memory
device list, applies registry-defined storage policy, and exposes state and
events on the system D-Bus.

The repository also builds `hsctl`, the reference D-Bus client. The daemon is
deliberately not a kernel module. Root is currently used for mount, unmount,
GPIO, and system-bus operations; it does not make the implementation
kernel-space code.

The safety objective is to reduce the risk of corruption and make unsafe
removal visible. It cannot guarantee that a surprise physical removal loses no
data.

### Current status

- The daemon, CLI, systemd unit, D-Bus policy, registry, and hardware-free unit
  tests are implemented.
- The build uses `-Werror`; all six local test executables pass as of
  2026-08-21.
- `systemd-analyze verify config/hotswapd.service` passes in the current
  development environment. Its sandbox-related socket warnings are non-fatal.
- Repeated CM5 testing with three physical module types and five cycles per
  type is still **pending hardware validation**.
- Per-device USB-C PD reporting remains opportunistic. No PD values are
  reported when sysfs cannot be tied reliably to the attached USB device.

## Five-Minute Orientation

The runtime is single-threaded. [`src/main.c`](src/main.c) owns one `epoll`
loop and combines udev, D-Bus, signals, registry inotify, GPIO edge events, and
per-storage-device timerfds.

| Area | Primary files | Maintainer concern |
|---|---|---|
| Process lifecycle and event routing | [`src/main.c`](src/main.c) | Every dynamic fd must leave epoll, be closed, and have its context freed. |
| USB discovery and classification | [`src/device_monitor.c`](src/device_monitor.c), [`src/usb_classification.c`](src/usb_classification.c) | Cache identity on attach; sysfs may be gone at removal. Preserve storage-first composite-device classification. |
| Attached-device state | [`src/device_state.c`](src/device_state.c), [`include/hotswapd.h`](include/hotswapd.h) | `DEVPATH` is the key. Avoid duplicate records and stale per-device timers. |
| Registry | [`src/module_registry.c`](src/module_registry.c), [`config/modules.json`](config/modules.json) | Exact VID/PID wins. Invalid reload must retain the last valid registry. Preserve atomic-replacement support. |
| Storage handling | [`src/storage_handler.c`](src/storage_handler.c) | Match block devices and mounts back to the correct USB parent before acting. Never broaden an unmount target. |
| GPIO safe release | [`src/gpio_release.c`](src/gpio_release.c) | Treat release readiness as a protocol, not merely an edge. Multiple devices require a stable DEVPATH prefix. |
| D-Bus contract | [`src/dbus_service.c`](src/dbus_service.c), [`config/hotswapd.conf`](config/hotswapd.conf) | Keep names and signatures stable. Mutating registration remains root-only. |
| Operator CLI | [`src/hsctl/hsctl.c`](src/hsctl/hsctl.c) | Keep output and signal parsing aligned with the daemon API. |
| Service and install layout | [`config/hotswapd.service`](config/hotswapd.service), [`Makefile`](Makefile) | Continue using `Type=dbus` and foreground mode. Do not auto-enable or auto-start during install. |

### Runtime flows

Attach:

1. libudev enumerates an existing USB device or reports a new
   `DEVTYPE=usb_device`.
2. The monitor caches identity, descriptor, power, and speed fields.
3. An exact registry category is preferred; otherwise USB device and interface
   classes determine the category.
4. The device is added to in-memory state.
5. Storage devices scan existing mounts and, when configured, poll for their
   block nodes for up to 10 seconds before invoking `mount` directly.
6. The daemon emits `ModuleAttached`, followed by `PowerChanged`.

Safe release:

1. A configured GPIO rising edge selects one device, or all devices below the
   configured DEVPATH prefix.
2. Storage mounts are refreshed, flushed with `syncfs()`, and normally
   unmounted while the hardware is still present.
3. Only complete success changes the state to `detaching` and emits
   `ModuleReadyForRemoval`.
4. A failure emits `ModuleReleaseFailed`; the user must not remove the module.
5. The later udev removal is then reported with `was_unclean=false`.

Surprise removal:

1. The remove event is matched against cached state by DEVPATH.
2. The detach is marked unclean unless the module was already `detaching`.
3. Storage timers stop. Still-matching stale mounts are lazily detached; the
   implementation does not pretend that post-removal syncing can recover data.
4. Cached state is removed and `ModuleDetached(..., true)` plus
   `PowerChanged` are emitted.

## Stable External Contracts

Treat the D-Bus identity and the original method/signal signatures as public
API. Do not silently rename or change them.

```text
Bus name:  org.postmarketos.HotSwap
Object:    /org/postmarketos/HotSwap
Interface: org.postmarketos.HotSwap
```

### Signals

| Name | Signature | Fields |
|---|---|---|
| `ModuleAttached` | `sssssuu` | devpath, vendor ID, product ID, name, category, max power mA, speed Mbps |
| `ModuleDetached` | `ssb` | devpath, name, was unclean |
| `PowerChanged` | `uu` | total bus power mA, device count |
| `ModuleReadyForRemoval` | `ss` | devpath, name |
| `ModuleReleaseFailed` | `ss` | devpath or selector, reason |

`speed_mbps` is a D-Bus `UINT32`. Fractional sysfs speeds such as `1.5` are
rounded to the nearest integer for this API.

### Methods

| Name | Signature | Access |
|---|---|---|
| `ListModules` | `() -> a(ssssu)` | All clients |
| `GetModuleInfo` | `(s) -> a{sv}` | All clients |
| `GetTotalPowerDraw` | `() -> u` | All clients |
| `ListRegistry` | `() -> a(sssss)` | All clients |
| `RegisterModule` | `(sbsss) -> sssssb` | Root only |

`GetModuleInfo` currently exposes `devpath`, `syspath`, `vendor_id`,
`product_id`, `vendor_name`, `product_name`, `serial`, `category`, `state`,
`max_power_ma`, `speed_mbps`, `self_powered`, `has_pd`, and `mount_count`. When
`has_pd` is true it also exposes `pd_voltage_uv`, `pd_current_ua`, and
`pd_power_role`. UI clients should call `ListModules` after startup or
reconnect, then subscribe to signals; they should never depend on detach-time
sysfs reads.

## Build and Local Verification

Required development packages provide GCC, Make, `pkg-config`, libudev,
libdbus-1, json-c, and Linux userspace headers. On Debian or Raspberry Pi OS,
install them with:

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

The target also needs systemd, D-Bus, udev/eudev, `mount` from util-linux, and
the userspace helper for each filesystem it will mount. Use the equivalent
development and runtime packages supplied by the target distribution when it
is not Debian-based.

Run before handing off a change:

```sh
make clean
make
make test
systemd-analyze verify config/hotswapd.service
```

The hardware-free tests cover:

- registry parsing, lookup, reload, invalid-reload retention, registration, and
  atomic replacement;
- state add/find/remove and duplicate or unknown events;
- USB power and fractional speed parsing;
- storage device-name and USB-parent resolution plus attach/detach operations;
- GPIO controller selection, edge filtering, debounce, malformed events, and
  fd ownership;
- USB descriptor and composite-interface classification.

There is no automated end-to-end system-bus, live-udev, real-filesystem, or
physical-GPIO test. Passing `make test` must not be reported as hardware proof.

## Installation and Deployment

The supplied unit assumes a systemd target and installs the daemon as root:

```sh
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now hotswapd.service
systemctl status hotswapd.service
```

Run `enable --now` once after installation. It starts the daemon immediately
and enables the systemd service so that the daemon starts automatically on
future boots; it does not need to be started manually after each boot.

Installed files:

```text
/usr/sbin/hotswapd
/usr/bin/hsctl
/etc/hotswapd/modules.json
/etc/dbus-1/system.d/hotswapd.conf
/usr/lib/systemd/system/hotswapd.service
/usr/share/man/man8/hotswapd.8
```

Important: `make install` copies `config/modules.json` over the installed
registry. Back up `/etc/hotswapd/modules.json` before reinstalling on a target
that has locally registered modules. Installation does not enable or start the
service by itself. The Makefile's path variables and `DESTDIR` behavior are
part of the packaging interface and should be preserved.

The systemd unit must remain `Type=dbus`, name
`org.postmarketos.HotSwap`, and start `/usr/sbin/hotswapd -f`. Do not add
`sd_notify()` or change the unit to `Type=notify` without an intentional design
change.

## Configuration and Registry Operations

The default registry is `/etc/hotswapd/modules.json`; use `hotswapd -c PATH`
for a development override. A typical registry is:

```json
{
  "modules": [
    {
      "vendor_id": "0781",
      "product_id": "5567",
      "name": "Demonstration storage",
      "category": "storage",
      "description": "Known removable USB storage",
      "on_attach": {
        "action": "mount",
        "options": "flush,noatime",
        "mount_point": "/run/media/hotswapd/{device}"
      },
      "on_detach": {
        "action": "unmount"
      }
    }
  ],
  "defaults": {
    "storage": {
      "on_attach": {
        "action": "mount",
        "options": "noatime",
        "mount_point": "/run/media/hotswapd/{device}"
      },
      "on_detach": {
        "action": "unmount"
      },
      "sync_policy": {
        "mode": "idle",
        "idle_sync_delay": 5,
        "fallback_sync_interval": 60
      }
    }
  }
}
```

- Required module keys: `vendor_id`, `product_id`.
- Optional keys: `name`, `description`, `category`, `on_attach`, `on_detach`,
  `sync_policy`.
- Categories: `storage`, `hid`, `serial`, `network`, `audio`, `video`, `hub`.
- Sync modes: `idle`, `periodic`, `manual`, `disabled`.
- A mount point must be absolute. `{device}` expands to the discovered block
  device basename.
- Storage is mounted only when an exact module or category default explicitly
  configures an `on_attach` `mount` action.
- Exact module definitions win over category defaults; compile-time values are
  the last fallback.
- A malformed initial registry prevents daemon startup. A malformed live
  reload logs an error and leaves the previous valid registry active.
- Reload is automatic through inotify and supports normal editor atomic
  replacement. `sudo systemctl reload` is not defined; send `SIGHUP` directly
  or restart the service when a manual fallback is needed.

Common operations:

```sh
hsctl registry
hsctl list
hsctl info <devpath>
sudo hsctl register <devpath>
sudo hsctl register <devpath> --replace \
  --name "Display name" \
  --category storage \
  --description "Team-owned module"
sudo kill -HUP "$(cat /run/hotswapd.pid)"
```

Registration must refer to a currently attached DEVPATH. The daemon resolves
the caller UID, locks the registry, writes and flushes a same-directory
temporary file, atomically renames it, and reloads it. A new or changed category
normally applies on the module's next attachment because existing device
records retain their cached policy.

### Daemon options and GPIO safe-release configuration

| Option | Meaning |
|---|---|
| `-f` | Stay in the foreground; required by the supplied systemd unit. |
| `-c PATH` | Use a non-default registry. |
| `-G CHIP`, `--gpio-chip CHIP` | Use `auto` or a specific `/dev/gpiochipN`. |
| `-L N`, `--gpio-line N` | Select a GPIO line offset; default 26. |
| `-P PREFIX`, `--release-devpath-prefix PREFIX` | Select devices below a stable USB DEVPATH prefix. |
| `--no-gpio-release` | Disable GPIO monitoring. |
| `-v`, `-vv` | Enable verbose or debug logging. |
| `-h`, `--help` | Print usage. |

The defaults are GPIO26, physical header pin 37, an internal pull-up, a rising
edge, and 50 ms debounce. The intended normally-closed contact connects pin 37
to ground at physical pin 39. Use 3.3 V GPIO levels and never drive the input
with 5 V.

GPIO availability is deliberately non-fatal. With one attached module, no
prefix is needed. With multiple attached devices, configure the stable USB
topology portion of DEVPATH using `--release-devpath-prefix`; otherwise a
release event is rejected as ambiguous. Put persistent options in a deliberate
systemd unit override and document the carrier-board wiring alongside it.

## Operator Runbook

### Confirm service and IPC health

```sh
systemctl status hotswapd.service
busctl --system list | grep org.postmarketos.HotSwap
hsctl list
hsctl power
journalctl -u hotswapd.service -n 100 --no-pager
```

Use `hsctl monitor` during attach, detach, and safe-release tests. For storage,
also verify the real mount table rather than relying only on `mount_count`.

### Safe physical removal

1. Start `hsctl monitor` or the UI event display.
2. Open the configured release contact.
3. Wait for `ModuleReadyForRemoval` for the intended DEVPATH.
4. If `ModuleReleaseFailed` appears, leave the device connected, close users of
   the filesystem, and retry.
5. Remove the device only after the ready event.

The contact cannot physically enforce the delay. A production enclosure should
make early removal difficult and should provide a visible ready/failure state.

### Target validation

Create an evidence directory before starting and capture the daemon journal and
D-Bus event stream. The following is one portable approach that does not rely
on repository helper scripts:

```sh
evidence_dir="hotswapd-validation-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$evidence_dir"
journalctl -u hotswapd.service --since now -f \
  >"$evidence_dir/journal.log" 2>&1 &
journal_pid=$!
hsctl monitor >"$evidence_dir/dbus-events.log" 2>&1 &
monitor_pid=$!
```

After testing, stop only those two capture processes and save the final state:

```sh
kill "$journal_pid" "$monitor_pid"
systemctl status hotswapd.service --no-pager \
  >"$evidence_dir/service-status.txt"
hsctl list >"$evidence_dir/final-modules.txt"
hsctl power >"$evidence_dir/final-power.txt"
```

Use this minimum matrix and fill every result with pass, fail plus evidence, or
`pending hardware validation`:

| Module | Cycles required | Attach signal | Detach signal | Category correct | No daemon/system instability | Result |
|---|---:|---|---|---|---|---|
| USB storage | 5 |  |  |  |  | Pending |
| USB HID | 5 |  |  |  |  | Pending |
| USB serial or another third type | 5 |  |  |  |  | Pending |

For every cycle, record the cycle number, DEVPATH, timestamp, expected result,
actual result, and relevant evidence-file line. For storage, verify the real
mount table after attach, perform safe release and wait for
`ModuleReadyForRemoval`, and confirm the mount is gone before unplugging. If it
is safe to do so with disposable data, run one controlled surprise-removal
test and confirm `ModuleDetached(..., true)` and conservative stale-mount
cleanup. Never run that test with valuable data.

Also validate service startup after boot, registry reload by both in-place edit
and atomic replacement, retention of the prior registry after malformed JSON,
GPIO edge and debounce behavior on the real carrier, and UI response to both
ready and failed release signals. Keep USB-C PD marked pending unless the
reported values can be tied to the actual attached module or port.

## Troubleshooting

| Symptom | Checks and likely cause |
|---|---|
| Service exits immediately | Inspect the journal. A missing or malformed initial registry, unavailable system bus, failure to acquire the fixed bus name, or udev initialization failure is fatal. |
| `hsctl` cannot connect or gets access denied | Confirm the system bus, installed D-Bus policy, service ownership of the bus name, and `daemon-reload`/D-Bus policy reload after installation. `RegisterModule` additionally requires root. |
| GPIO warning on startup | Expected on development machines and boards without a usable GPIO v2 device. Use `--no-gpio-release` when intentional. |
| Release event says selection is ambiguous | More than one module is attached and no DEVPATH prefix is configured. Set a topology-specific `--release-devpath-prefix`. |
| `ModuleReleaseFailed` reports unmount or sync failure | A process may hold the filesystem busy, the mount may have changed, or I/O has failed. Inspect `findmnt`, `fuser`/`lsof`, and the journal; do not remove the device until a later ready event. |
| Storage does not mount | Confirm category and registry action, filesystem helpers, the resolved block node, absolute mount point, permissions, and the 10-second discovery timeout in the journal. The daemon intentionally does not mount storage without explicit policy. |
| Registry edit has no effect | Check parse errors in the journal and `hsctl registry`. Invalid reloads retain the previous data. Already attached modules retain cached category/actions until reattach. |
| No USB-C PD fields | This is expected unless the kernel exposes a reliable relationship between the USB device and its Type-C/PD data. Do not replace this with a first-global-port guess. |
| Power total looks conservative | `GetTotalPowerDraw` sums declared `bMaxPower` for bus-powered tracked devices; it is not a live current meter. |

## Change Guardrails

- Keep `hotswapd` in user space and keep systemd as the service manager.
- Preserve all D-Bus identifiers and public signatures. Additive extensions
  must also update `hsctl`, policy, documentation, and tests.
- Cache every detach-time field during attach.
- Prefer no PD data over incorrect per-device attribution.
- Do not use a shell for mount actions or add arbitrary registry-defined
  command execution.
- Verify mount source and USB ancestry before mount, sync, or unmount actions.
- Stop timers and remove their epoll contexts before freeing a device.
- Keep malformed-registry reload transactional.
- Preserve `-Werror`, install path overrides, and the rule that install does
  not start the service.
- Preserve the project's GPL-3.0-or-later SPDX identifiers and top-level license
  notice when adding or moving source files.
- Describe storage cleanup as risk reduction, never as a no-data-loss
  guarantee.

For a D-Bus change, update at least
[`include/hotswapd.h`](include/hotswapd.h),
[`src/dbus_service.c`](src/dbus_service.c),
[`src/hsctl/hsctl.c`](src/hsctl/hsctl.c),
[`config/hotswapd.conf`](config/hotswapd.conf) when permissions change, the
public API documentation, and this handoff. For a registry or storage-policy
change, update the example registry, parser tests, storage tests, and target
validation expectations.

## Known Gaps and Recommended Next Work

1. Complete and archive CM5 hardware validation for storage, HID, and serial
   modules with at least five cycles each.
2. Validate GPIO26 selection, edge behavior, and ready/failure UI indication on
   the actual carrier and mechanical release design.
3. Add an automated D-Bus contract test for method replies and all signal
   signatures.
4. Decide whether to implement real write-activity observation. The current
   idle-sync plumbing initializes `dirty` as false and has no watcher that sets
   it, so idle mode generally skips timer-driven `syncfs()`; periodic mode is
   the direct automatic-sync choice until that changes.
5. Validate filesystem-specific mount behavior and controlled surprise removal
   on the target image.
6. Add accurate per-device PD support only if the target kernel exposes a
   defensible USB-to-Type-C-port relationship.
7. Consider packaging behavior that preserves a locally modified installed
   registry across upgrades; the current raw `make install` overwrites it.

## Handoff Completion Checklist

Before transferring ownership or cutting a release:

- Record the exact commit, target image, kernel, hardware revision, and module
  inventory used for validation.
- Run the local verification commands and save failures as well as successes.
- Back up the installed registry before deployment.
- Verify service ownership of the D-Bus name and all `hsctl` queries.
- Exercise safe release and confirm the UI waits for the ready signal.
- Complete the physical cycle matrix in this document.
- Attach the validation evidence directory and relevant journal excerpts to the
  release or handoff record.
- List remaining target-only items as pending hardware validation.
