## Hot-Swap Daemon Architecture

Companion diagrams for this architecture are collected in
[`hotswapd-diagrams.md`](hotswapd-diagrams.md).

The hot-swap daemon (`hotswapd`) is a long-running postmarketOS service that
monitors USB peripheral attach/detach events, keeps an in-memory view of
attached modules, applies storage cleanup policy, and publishes state changes
to clients over the D-Bus system bus. The daemon is written in C and is intended
to run on Alpine Linux/postmarketOS with `musl`, systemd, `eudev`, `libdbus`,
and `json-c`.

The reliability goal is conservative device handling: all USB identity and
power fields are cached when a device is added, because sysfs attributes may no
longer be readable by the time the kernel emits a remove event. Storage devices
are treated specially so that sudden removal still results in `sync()`, lazy
unmount cleanup, a D-Bus detach notification, and a user-visible unclean-detach
flag.

### Daemon Decomposition

`src/main.c` owns process lifecycle and the central `epoll` loop. It parses
`-f`, `-c <path>`, `-v`, and `-vv`. When started manually without `-f`, it
daemonizes; when started by systemd, the installed unit runs it in foreground
mode with `-f`. It initializes logging, writes `/run/hotswapd.pid`, blocks
`SIGINT`, `SIGTERM`, and `SIGHUP`, and handles those signals through
`signalfd`.

The daemon integrates these event sources into one single-threaded loop:

| Source | File descriptor | Handler |
|---|---:|---|
| USB udev monitor | `udev_monitor_get_fd()` | `monitor_process_event()` |
| D-Bus system bus | libdbus watches | `dbus_watch_handle()` + `dbus_service_dispatch()` |
| Registry reloads | `inotify_init1()` fd | `registry_handle_inotify_event()` |
| Signals | `signalfd()` fd | terminate or reload registry |
| GPIO release contact | GPIO line-event fd | flush/unmount and mark the selected module ready |
| Storage sync timers | per-device `timerfd` | `storage_handle_sync_timer()` |

The daemon is split into small modules:

| Module | Responsibility |
|---|---|
| `device_monitor` | Enumerates existing USB devices and listens for `add`, `remove`, `bind`, `unbind`, and `change` udev events. |
| `device_state` | Stores tracked `struct hs_device` records in a linked list and exposes lookup, iteration, count, and bus-power aggregation. |
| `module_registry` | Loads `/etc/hotswapd/modules.json`, parses module metadata/defaults with `json-c`, and reloads via inotify or `SIGHUP`. |
| `dbus_service` | Owns the `org.postmarketos.HotSwap` system-bus name, emits signals, and handles query methods. |
| `power_info` | Reads legacy USB power attributes from sysfs and only reports USB-C PD data when a reliable per-device association exists. |
| `storage_handler` | Tracks mounted filesystems for USB storage devices, manages sync timers, and performs detach cleanup. |
| `gpio_release` | Watches a pulled-up, rising-edge, debounced GPIO line through the Linux GPIO v2 character-device API. |
| `log` | Sends logs to syslog in daemon mode and stderr in foreground mode. |
| `hsctl` | CLI client for `list`, `info`, `power`, and live `monitor` commands over D-Bus. |

The core shared record is `struct hs_device` from `include/hotswapd.h`. It
contains cached udev/sysfs identity, category, state, USB power information,
optional USB-C PD fields, storage mount points, sync policy state, attach time,
and the linked-list pointer used by `device_state`.

### Module Attach Flow

At startup, the daemon creates the D-Bus service, builds the epoll set, creates
the udev monitor, and then enumerates existing USB devices before entering the
main event loop. Enumeration and hotplug both use the same attach callback.

Attach flow:

1. `device_monitor` receives or enumerates a udev device with subsystem
   `usb` and `DEVTYPE=usb_device`.
2. Root USB hubs are skipped so the daemon tracks removable peripherals rather
   than host-controller infrastructure.
3. A new `struct hs_device` is allocated and populated from udev properties and
   sysfs attributes:
   `DEVPATH`, syspath, vendor/product IDs, vendor/product names, serial,
   USB device class, `bMaxPower`, `speed`, and `bmAttributes`.
4. An exact `(vendor_id, product_id)` registry category has highest
   precedence. Otherwise a recognized device-level USB class is used, followed
   by interface-level classes from `ID_USB_INTERFACES` and child USB interface
   sysfs records. Composite interfaces use a fixed safety priority: storage,
   network, serial, video, audio, HID, then hub.
5. The device is added to `device_state`. Duplicate `DEVPATH` values are
   rejected.
6. If the category is `storage`, `storage_on_attach()` immediately scans
   `/proc/mounts`, then uses a 200 ms `timerfd` for a bounded 10-second block
   discovery window. Block nodes are matched to the exact USB sysfs ancestor.
   A configured `mount` action invokes `mount(8)` without a shell, records the
   resulting mount, and starts a sync `timerfd` when policy requires one.
7. `dbus_service` emits `ModuleAttached` and then `PowerChanged`.

Storage is never mounted unless the exact module or its category defaults
explicitly configure `"on_attach": { "action": "mount" }`. An optional
`mount_point` supports a `{device}` token; otherwise the default target is
`/run/media/hotswapd/{device}`. Existing matching mounts are recorded rather
than mounted again.

### Unclean Detach Handling

Detach handling is driven by udev `remove` events. At remove time the kernel may
have already removed most sysfs files, so detach processing uses the cached
device record from `device_state`.

Detach flow:

1. `device_monitor` receives a `remove` event and looks up the cached device by
   `DEVPATH`.
2. The detach is considered unclean unless the device state was already
   `DEV_STATE_DETACHING`. A successful GPIO safe-release request flushes and
   unmounts storage, transitions matching devices into `DETACHING`, and emits
   `ModuleReadyForRemoval` before physical removal.
3. `main` removes any storage sync timer from epoll.
4. For storage devices, `storage_on_detach()` stops the timer, calls
   system-wide `sync()`, and lazily unmounts each tracked mount point with
   `umount2(..., MNT_DETACH)`.
5. The device is removed from the linked-list state and freed.
6. `dbus_service` emits `ModuleDetached(devpath, name, was_unclean)` and
   `PowerChanged(total_draw_ma, device_count)`.

Lazy unmount is intentional for surprise removal. It immediately detaches stale
mount points from the namespace while allowing any already-open handles to drain
through normal kernel behavior. If the device was removed before the most recent
dirty pages were flushed, the daemon can only reduce the damage window; it
cannot guarantee that all data reached the device.

Storage sync policy is represented by `enum sync_mode`:

| Mode | Behavior |
|---|---|
| `idle` | Intended default. Sync after the device has been idle for `idle_sync_delay` seconds, with a fallback interval for sustained write activity. |
| `periodic` | Sync mounted filesystems at a fixed interval. |
| `manual` | No automatic timer; sync only during explicit/manual flows or detach cleanup. |
| `disabled` | No automatic sync timer. Detach cleanup still calls `sync()`. |

The implementation currently has the timer and policy plumbing in place, but it
does not yet include a write-activity watcher that marks `dev->dirty`. Without
that signal, idle timers will usually find the device clean and skip `syncfs()`;
periodic mode remains the more direct automatic sync mode.

### Module Registry Format

The registry is a JSON file loaded from `/etc/hotswapd/modules.json` by
default, or from a custom path passed with `-c`. The installed example lives at
`config/modules.json`.

Top-level schema:

```json
{
  "modules": [
    {
      "vendor_id": "0781",
      "product_id": "5567",
      "name": "SanDisk Cruzer Blade",
      "category": "storage",
      "description": "USB 2.0 flash drive",
      "on_attach": {
        "action": "mount",
        "options": "flush,noatime",
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

Required module fields are `vendor_id` and `product_id`. Optional fields include
`name`, `description`, `category`, `on_attach`, `on_detach`, and `sync_policy`.
Mount actions accept `options` as a comma-separated mount option string and an
optional absolute `mount_point`; `{device}` expands to the discovered block
device basename so multiple volumes do not share a target.
Recognized categories are `storage`, `hid`, `serial`, `network`, `audio`,
`video`, `hub`, and `unknown`. Recognized sync modes are `idle`, `periodic`,
`manual`, and `disabled`.

Registry reload behavior:

1. `registry_load()` parses the initial JSON file and watches the parent
   directory so that normal editor atomic replacement is observable.
2. A readable inotify fd or `SIGHUP` causes reload processing.
3. Reload reparses the file and only swaps in the new module array/defaults on
   success, so malformed JSON leaves the previous valid registry active.
4. Existing `hs_device` records keep their already-cached category and sync
   policy. New attach events use the new registry contents.

### Communication and IPC

The daemon uses the D-Bus system bus instead of a private socket so system UI,
shell tools, and future services can share one permissioned, introspectable IPC
mechanism. The D-Bus policy file is `config/hotswapd.conf` and should be
installed to `/etc/dbus-1/system.d/hotswapd.conf`.

Identifiers:

| Item | Value |
|---|---|
| Bus name | `org.postmarketos.HotSwap` |
| Object path | `/org/postmarketos/HotSwap` |
| Interface | `org.postmarketos.HotSwap` |

Signals:

| Signal | Signature | Parameters |
|---|---|---|
| `ModuleAttached` | `sssssuu` | `devpath`, `vendor_id`, `product_id`, `name`, `category`, `max_power_ma`, `speed_mbps` |
| `ModuleDetached` | `ssb` | `devpath`, `name`, `was_unclean` |
| `PowerChanged` | `uu` | `total_draw_ma`, `device_count` |
| `ModuleReadyForRemoval` | `ss` | `devpath`, `name` |
| `ModuleReleaseFailed` | `ss` | `devpath_or_selector`, `reason` |

Methods:

| Method | In | Out | Description |
|---|---|---|---|
| `ListModules` | none | `a(ssssu)` | Array of `devpath`, `name`, `category`, `state`, `power_ma`. |
| `GetModuleInfo` | `s` | `a{sv}` | Full property dictionary for one tracked `DEVPATH`. |
| `GetTotalPowerDraw` | none | `u` | Sum of `max_power_ma` for bus-powered tracked devices. |

`hsctl` is the reference client:

| Command | D-Bus operation |
|---|---|
| `hsctl list` | Calls `ListModules`. |
| `hsctl info <devpath>` | Calls `GetModuleInfo`. |
| `hsctl power` | Calls `GetTotalPowerDraw`. |
| `hsctl monitor` | Subscribes to `org.postmarketos.HotSwap` signals. |

## Integration Points

The daemon depends on kernel USB/sysfs events, udev device metadata, D-Bus
system-bus policy, and privileged mount cleanup. The main integration boundary
is intentionally narrow: the daemon owns device observation and cleanup, while
UI clients observe state through D-Bus and avoid direct sysfs parsing.

### OS ↔ Hot-Swap Daemon

Runtime dependencies:

| Dependency | Purpose |
|---|---|
| Linux USB stack and sysfs | Provides `/sys` USB attributes and uevents. |
| `eudev`/libudev | Enumerates existing USB devices and receives hotplug events. |
| D-Bus system bus | Hosts `org.postmarketos.HotSwap`. |
| `json-c` | Parses the module registry. |
| `/proc/mounts` | Lets the storage handler identify currently mounted filesystems. |
| `timerfd`, `signalfd`, `inotify`, `epoll` | Single-threaded event loop primitives. |
| root or equivalent privileges | Required for D-Bus name ownership under the provided policy and for `umount2()`. |

## Validation Status

Local automated validation currently covers build correctness, registry
behavior, legacy power parsing, and storage-name resolution. Repeated physical
attach/detach cycling across three real module types, storage surprise-removal
behavior, and any per-device USB-C PD observation remain pending hardware
validation on the target platform.

Kernel capabilities/configuration expected on the target image:

| Kernel facility | Why it matters |
|---|---|
| USB host/peripheral support for the target hardware | Required for physical module attach/detach events. |
| sysfs and procfs | Required for USB attributes, block-device parent resolution, and mount scanning. |
| uevent support | Required by udev/libudev monitoring. |
| USB mass storage and relevant HID/serial/network class drivers | Required for the module categories the daemon reports. |
| USB Type-C class and power-supply class, if available | Enables optional USB-C PD fields. |

Service ordering under systemd should ensure that `hotswapd` starts after the
system bus and udev are available. The installed `config/hotswapd.service`
unit uses `Type=dbus` with `BusName=org.postmarketos.HotSwap`,
`After=systemd-udevd.service dbus.service`, and
`Wants=systemd-udevd.service`. The daemon should continue to run as root unless
a future design grants a dedicated `hotswap` user the necessary capabilities.

### Daemon ↔ UI (IPC)

UI clients should treat D-Bus as the stable interface. They should subscribe to
`ModuleAttached`, `ModuleDetached`, and `PowerChanged` for live updates, and use
`ListModules` or `GetModuleInfo` to recover state after startup or reconnect.

Recommended UI behavior:

1. On startup, call `ListModules` to build the current module list.
2. Subscribe to `org.postmarketos.HotSwap` signals on the system bus.
3. On `ModuleAttached`, add or refresh the module row keyed by `devpath`.
4. On `ModuleDetached`, remove the module row and surface a warning if
   `was_unclean` is true.
5. On `PowerChanged`, update aggregate bus-power display.
6. Use `GetModuleInfo(devpath)` for detail views that need serial, syspath,
   USB-C PD, or mount-count fields.

The UI should not rely on sysfs files remaining present after detach. Any
detach-time information it needs should come from the daemon's cached D-Bus
payloads.
