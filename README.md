# hotswapd

`hotswapd` is a C user-space daemon for Pocket Distro Alpha. It monitors USB
attach/detach events with `libudev`, tracks known module types from a JSON
registry, publishes state on the D-Bus system bus, and performs conservative
storage cleanup for removable devices.

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

- Storage cleanup is conservative. On detach, the daemon stops sync timers,
  calls `sync()`, and lazily unmounts tracked mount points when appropriate.
- The daemon minimizes data-loss risk, but it cannot guarantee zero data loss
  after sudden physical removal.
- Registry defaults apply when no exact `(vendor_id, product_id)` match exists.
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
- any accurate per-device USB-C PD reporting

