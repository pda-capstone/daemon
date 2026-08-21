# hotswapd diagrams

These diagrams describe the implementation on the `main` branch. They show
the user-space daemon, its operating-system boundaries, its single-threaded
event loop, and the main device, storage, registry, and D-Bus interactions.
USB enumeration and hotplug monitoring use `libudev`/eudev; `libusb` is not a
dependency or interaction in this design.

The diagrams describe implemented control flow rather than serving as hardware
validation evidence. The maintainer reports recent testing on a Raspberry Pi
CM5 development kit and plans to repeat it for confirmation. Specific GPIO,
physical USB-cycle, and end-to-end results should be recorded against
[`hotswapd-validation-checklist.md`](hotswapd-validation-checklist.md) before
those individual criteria are marked confirmed.

## System context

`hotswapd` is a privileged user-space service. The Linux kernel and udev expose
hardware state; the daemon owns the cached model and cleanup policy; UI and CLI
clients consume the stable D-Bus API instead of reading transient sysfs state.

```mermaid
flowchart LR
    user[User or release mechanism]
    usb[USB modules<br/>storage, HID, serial, network, audio, video, hub]
    gpio[GPIO release contact]

    subgraph os[Linux / postmarketOS userspace]
        systemd[systemd]
        udev[systemd-udevd / eudev]
        bus[D-Bus system bus]
        daemon[hotswapd<br/>C user-space daemon]
        registry[(modules.json)]
        mounts[/Mounted filesystems/]
        cli[hsctl]
        ui[UI layer]
    end

    kernel[Linux kernel<br/>USB, sysfs, GPIO, block devices]

    user --> gpio
    user --> cli
    usb <--> kernel
    gpio --> kernel
    kernel --> udev
    kernel --> daemon
    udev -->|USB add/remove events| daemon
    systemd -->|start and restart| daemon
    daemon <-->|load, watch, atomic update| registry
    daemon <-->|mount, syncfs, unmount| mounts
    daemon <-->|methods and signals| bus
    cli <-->|inspect, monitor, register| bus
    ui <-->|state queries and notifications| bus
```

## Internal components

`main.c` owns lifecycle and coordinates all modules through one `epoll` loop.
The `hs_device` linked list is the live source of truth. Identity, power, policy,
and mount data are cached at attach time because most sysfs data can disappear
before a remove event is handled.

```mermaid
flowchart TB
    main[main.c<br/>lifecycle, callbacks, epoll dispatch]
    monitor[device_monitor.c<br/>udev enumeration and hotplug]
    classify[usb_classification.c<br/>category resolution]
    power[power_info.c<br/>legacy USB power and speed]
    registry[module_registry.c<br/>JSON parse, lookup, reload, update]
    state[(device_state.c<br/>linked list of hs_device)]
    storage[storage_handler.c<br/>discovery, mount, sync, unmount]
    gpio[gpio_release.c<br/>GPIO v2 edge and debounce]
    dbus[dbus_service.c<br/>system-bus API]
    log[log.c<br/>stderr or syslog]
    hsctl[hsctl<br/>D-Bus CLI client]

    udevfd[udev monitor fd] --> main
    signalfd[signalfd] --> main
    inotifyfd[inotify fd] --> main
    gpiofd[GPIO event fd] --> main
    timerfds[per-device timerfds] --> main
    dbusfds[libdbus watch fds] --> main

    main --> monitor
    monitor --> classify
    monitor --> power
    monitor --> registry
    main <--> state
    main --> storage
    main --> gpio
    main <--> dbus
    dbus <--> state
    dbus <--> registry
    hsctl <-->|org.postmarketos.HotSwap| dbus

    main -.-> log
    monitor -.-> log
    registry -.-> log
    storage -.-> log
    gpio -.-> log
    dbus -.-> log
```

## Single-threaded event dispatch

All runtime event sources converge on `epoll`. D-Bus watches are distinguished
from daemon-owned event contexts, while attach and sync timer contexts carry a
pointer to their associated `hs_device`.

```mermaid
flowchart TD
    wait[epoll_wait]
    ready{Ready source}
    wait --> ready

    ready -->|udev fd| udev[Process one USB event]
    ready -->|signalfd| signal{Signal}
    ready -->|inotify fd| reload[Reload registry]
    ready -->|GPIO fd| gpio[Debounce and prepare selected module]
    ready -->|attach timerfd| attach[Retry storage discovery or mount]
    ready -->|sync timerfd| sync[Run storage sync policy]
    ready -->|libdbus watch| dbus[Handle watch and dispatch methods]

    signal -->|SIGINT or SIGTERM| stop[Leave loop and clean up]
    signal -->|SIGHUP| reload
    reload --> valid{Parse valid?}
    valid -->|yes| swap[Swap active registry and update monitor pointer]
    valid -->|no| retain[Retain previous valid registry]

    udev --> wait
    gpio --> wait
    attach --> wait
    sync --> wait
    dbus --> wait
    swap --> wait
    retain --> wait
```

## Attach and classification sequence

Startup enumeration and live `add` events share the same attach callback. An
exact registry category wins; otherwise descriptor classification is used.
Storage receives additional asynchronous handling, but every accepted device
is announced immediately after its initial attach processing.

```mermaid
sequenceDiagram
    autonumber
    participant K as Kernel / sysfs
    participant U as udev
    participant M as device_monitor
    participant R as module_registry
    participant P as power_info
    participant Main as main
    participant S as device_state
    participant H as storage_handler
    participant B as D-Bus clients

    K->>U: USB device add uevent
    U->>M: add for usb_device
    M->>M: Skip root hub if applicable
    M->>K: Read and cache identity and descriptor data
    M->>R: Lookup exact VID/PID
    R-->>M: Exact metadata or no match
    M->>M: Resolve category and copy exact/default policy
    M->>P: Read bMaxPower, speed, bmAttributes
    P->>K: Read device sysfs attributes
    P-->>M: Legacy power/speed#59; PD absent unless reliably associated
    M->>Main: on_device_attach(hs_device)
    Main->>S: Add keyed by DEVPATH

    alt Duplicate DEVPATH
        S-->>Main: Reject
        Main->>Main: Free new record#59; emit nothing
    else New device
        opt Category is storage
            Main->>H: Scan mounts and begin bounded block discovery
            H-->>Main: Optional attach timerfd and/or sync timerfd
            Main->>Main: Register returned timerfds with epoll
        end
        Main-->>B: ModuleAttached(sssssuu)
        Main->>S: Calculate bus-powered total and count
        Main-->>B: PowerChanged(uu)
    end
```

## Category and policy resolution

Classification and policy selection are related but separate. A registry
category overrides USB descriptors. Per-device actions and sync policy override
category defaults; compile-time storage timings are used only when configured
values are absent or invalid.

```mermaid
flowchart TD
    start[New USB device]
    exact{Exact VID/PID registry entry?}
    regcat[Use registry category]
    devclass{Recognized device-level USB class?}
    classcat[Use device-class category]
    interfaces[Inspect ID_USB_INTERFACES and child interfaces]
    best[Choose highest-priority recognized interface category]
    unknown[Use unknown]
    policy[Resolve attach action, detach action, and sync policy independently]
    exactpolicy{Exact entry supplies this field group?}
    copyexact[Copy the exact-entry value]
    defaults{Category default supplies it?}
    defaultpolicy[Copy the category-default value]
    fallback[Use an empty action or compile-time sync defaults]
    done[Cache category and policy in hs_device]

    start --> exact
    exact -->|yes| regcat --> policy
    exact -->|no| devclass
    devclass -->|yes| classcat --> policy
    devclass -->|no| interfaces --> best
    best -->|recognized| policy
    best -->|none| unknown --> policy
    policy --> exactpolicy
    exactpolicy -->|yes| copyexact --> done
    exactpolicy -->|no| defaults
    defaults -->|yes| defaultpolicy --> done
    defaults -->|no| fallback --> done
```

For composite devices, interface-category priority is:
`storage > network > serial > video > audio > HID > hub`. This intentionally
keeps storage cleanup active when a device exposes several functions.

## Storage attach activity

The daemon never mounts a device merely because it is classified as storage.
Mounting requires an explicit exact-entry or category-default `mount` action.
Discovery polls every 200 ms for at most 10 seconds because block nodes and
partitions can appear after the USB-device event.

```mermaid
flowchart TD
    begin[Storage USB record accepted]
    scan[Scan /proc/mounts for block devices with this USB ancestor]
    mounted{Matching mount found?}
    action{Explicit on_attach action is mount?}
    discover[Discover matching block nodes through sysfs ancestry]
    found{Block node found?}
    target[Expand safe absolute mount target]
    mount[Invoke mount without a shell]
    rescan[Rescan and cache source/target pairs]
    pending{Still pending and under 10 seconds?}
    timer[Wait for next 200 ms attach timer]
    finish[Close attach timer]
    syncmode{Mounted and sync mode automatic?}
    synctimer[Start idle or periodic sync timer]
    tracked[Continue tracking device]

    begin --> scan --> mounted
    mounted -->|yes| finish
    mounted -->|no| action
    action -->|no| pending
    action -->|yes| discover --> found
    found -->|no| pending
    found -->|yes| target --> mount --> rescan --> mounted
    pending -->|yes| timer --> scan
    pending -->|no or error| finish
    finish --> syncmode
    syncmode -->|yes| synctimer --> tracked
    syncmode -->|no| tracked
```

## Safe release and physical detach

The GPIO contact is a preparation request, not the physical detach event. The
daemon emits readiness only after every selected storage mount has been synced
and normally unmounted. A later udev `remove` event completes the state change.

```mermaid
sequenceDiagram
    autonumber
    actor User
    participant G as GPIO release contact
    participant Main as hotswapd
    participant S as device_state
    participant H as storage_handler
    participant FS as Mounted filesystem
    participant UI as UI / hsctl monitor
    participant U as udev

    User->>G: Open contact before removing module
    G->>Main: Debounced rising-edge event
    Main->>S: Select by configured DEVPATH prefix

    alt No match, or ambiguous without a prefix
        Main-->>UI: ModuleReleaseFailed(ss)
    else One or more selected modules
        loop Each selected module
            alt Storage category
                Main->>H: storage_prepare_release
                H->>H: Stop attach and sync timers
                H->>FS: Refresh mounts, syncfs, normal unmount
                alt Any refresh, sync, or unmount fails
                    H-->>Main: Failure
                    Main-->>UI: ModuleReleaseFailed(ss)
                    Main->>H: Restore automatic sync timer when possible
                else Cleanup succeeds
                    H-->>Main: Success
                    Main->>S: State = DETACHING
                    Main-->>UI: ModuleReadyForRemoval(ss)
                end
            else Non-storage category
                Main->>S: State = DETACHING
                Main-->>UI: ModuleReadyForRemoval(ss)
            end
        end
    end

    User->>U: Physically remove only after ready indication
    U->>Main: remove(DEVPATH)
    Main->>S: Find cached DETACHING record
    Main->>H: Final detach cleanup
    Main->>S: Remove and free record
    Main-->>UI: ModuleDetached(ssb), was_unclean=false
    Main-->>UI: PowerChanged(uu)
```

## Surprise-removal cleanup

On an unprepared remove event, sysfs and the physical device may already be
gone. Cleanup therefore uses cached identity and exact mount source/target
pairs. The daemon refuses to unmount a path if it no longer matches the cached
source, and uses lazy unmount only for still-matching stale mounts.

```mermaid
sequenceDiagram
    autonumber
    participant U as udev
    participant M as device_monitor
    participant S as device_state
    participant Main as main
    participant H as storage_handler
    participant FS as Mount namespace
    participant UI as UI / hsctl monitor

    U->>M: remove(DEVPATH)
    M->>S: Find cached record
    S-->>M: State is ATTACHED
    M->>Main: on_device_detach(DEVPATH, unclean=true)
    Main->>H: Remove timer contexts and stop timers
    loop Each cached mount
        H->>FS: Verify cached source still matches target
        alt Still the same mount
            H->>FS: umount2(target, MNT_DETACH)
        else Changed or unrelated mount
            H->>H: Refuse to unmount it
        end
    end
    Main->>S: Remove cached record
    Main-->>UI: ModuleDetached(ssb), was_unclean=true
    Main-->>UI: PowerChanged(uu)
```

Surprise-removal cleanup cannot recover writes that did not reach the physical
device. It reduces stale mount and daemon-state risk; it does not guarantee no
data loss.

## Device lifecycle state

The state model deliberately has no software-only removal transition: a udev
remove event is authoritative for physical removal. Although `DETACHED` exists
in the enum, the current removal path deletes and frees the record instead of
retaining an observable detached record.

```mermaid
stateDiagram-v2
    [*] --> ATTACHED: accepted add or startup enumeration
    ATTACHED --> ATTACHED: ordinary queries and timer events
    ATTACHED --> DETACHING: GPIO preparation succeeds
    DETACHING --> DETACHING: repeated successful release request
    ATTACHED --> [*]: udev remove / unclean / free record
    DETACHING --> [*]: udev remove / clean / free record

    note right of ATTACHED
      Device is present and tracked.
      Storage may be mounted and syncing.
    end note

    note right of DETACHING
      Selected storage is synced and unmounted.
      Wait for physical removal.
    end note
```

If release preparation fails, the record remains `ATTACHED`; the daemon emits
`ModuleReleaseFailed` and attempts to restore automatic sync handling.

## Registry query, registration, and reload

Read-only registry listing is available to ordinary D-Bus clients. Mutation is
performed inside the root-owned daemon, and `RegisterModule` additionally
checks that the caller UID is root and the requested DEVPATH is currently
tracked.

```mermaid
sequenceDiagram
    autonumber
    actor Admin
    participant C as hsctl
    participant B as D-Bus system bus
    participant D as dbus_service
    participant S as device_state
    participant R as module_registry
    participant F as modules.json
    participant I as inotify

    Admin->>C: sudo hsctl register DEVPATH [overrides]
    C->>B: RegisterModule(sbsss)
    B->>D: Method call with sender identity
    D->>D: Require UID 0 and validate arguments
    D->>S: Find currently connected DEVPATH
    S-->>D: Cached identity and inferred category
    D->>R: Register or explicitly replace VID/PID
    R->>F: Acquire sidecar lock
    R->>F: Read current JSON and preserve unrelated fields
    R->>F: Write, flush, verify temporary JSON
    R->>F: Atomic rename and directory fsync
    R->>R: Reload active registry
    R-->>D: Registered metadata and replaced flag
    D-->>B: sssssb result
    B-->>C: Display result
    Note over C,R: New metadata normally applies on the next attachment.
    F-->>I: Directory change notification
    I-->>R: Reload event may follow the explicit reload
```

External edits and `SIGHUP` use the same guarded reload behavior:

```mermaid
flowchart LR
    edit[Atomic editor replacement] --> inotify[inotify directory event]
    sighup[SIGHUP] --> parse[Parse candidate JSON]
    inotify --> parse
    parse --> valid{Valid registry?}
    valid -->|yes| swap[Replace module array and defaults]
    valid -->|no| keep[Keep previous valid registry]
    swap --> future[Future attaches use new metadata]
    keep --> future
    current[Already attached hs_device records] --> cached[Keep cached category and policy]
```

## D-Bus contract

All messages use bus name and interface `org.postmarketos.HotSwap` at object
path `/org/postmarketos/HotSwap`.

```mermaid
flowchart LR
    clients["UI, hsctl, other clients"]
    service["hotswapd D-Bus service"]
    root["root hsctl"]

    clients -->|"ListModules -> a(ssssu)"| service
    clients -->|"GetModuleInfo(s) -> a#123;sv#125;"| service
    clients -->|"GetTotalPowerDraw -> u"| service
    clients -->|"ListRegistry -> a(sssss)"| service
    root -->|"RegisterModule(sbsss) -> sssssb"| service

    service -->|"ModuleAttached sssssuu"| clients
    service -->|"ModuleDetached ssb"| clients
    service -->|"PowerChanged uu"| clients
    service -->|"ModuleReadyForRemoval ss"| clients
    service -->|"ModuleReleaseFailed ss"| clients
```

Normal clients may call the four read-only methods and receive signals under
the supplied D-Bus policy. Registration is restricted to root. A reconnecting
UI should call `ListModules`, subscribe to signals, and use `GetModuleInfo` for
details rather than depending on sysfs after removal.
