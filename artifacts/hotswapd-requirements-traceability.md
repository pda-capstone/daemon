# hotswapd Requirements Traceability

This document maps the PDA hot-swap daemon requirements from [AGENTS.md](/home/aolivier/hotswapd/AGENTS.md)
to the current implementation, automated tests, and remaining hardware
validation needs.

## Summary

The current project satisfies the core shape required by the PDA guidance:

- C user-space daemon: implemented
- systemd-managed: implemented
- D-Bus-notifying: implemented
- USB attach/detach aware: implemented
- registry-driven: implemented
- storage-cleanup capable: implemented
- conservative about power and USB-C PD reporting: implemented
- testable without hardware where practical: implemented
- honest about hardware validation requirements: documented here and in the
  validation checklist

The remaining gap is not architecture or API shape. It is target-hardware
evidence: repeated real-module validation on CM5-class hardware is still
pending.

## Traceability Table

| Requirement | Implementation Evidence | Test / Verification Evidence | Status | Notes |
|---|---|---|---|---|
| C implementation | [src/main.c](/home/aolivier/hotswapd/src/main.c:1), [src/device_monitor.c](/home/aolivier/hotswapd/src/device_monitor.c:1), [src/dbus_service.c](/home/aolivier/hotswapd/src/dbus_service.c:1) | `make` | Implemented | Entire daemon and CLI are C code. |
| User-space daemon | [src/main.c](/home/aolivier/hotswapd/src/main.c:198), [config/hotswapd.service](/home/aolivier/hotswapd/config/hotswapd.service:1) | `make`, target install workflow | Implemented | No kernel modules or kernel-space hot-swap logic. |
| systemd-managed service | [config/hotswapd.service](/home/aolivier/hotswapd/config/hotswapd.service:1), [Makefile](/home/aolivier/hotswapd/Makefile:87) | `systemd-analyze verify config/hotswapd.service` | Implemented | Runs foreground with `ExecStart=/usr/sbin/hotswapd -f`. |
| D-Bus identity and notifications | [include/hotswapd.h](/home/aolivier/hotswapd/include/hotswapd.h:27), [src/dbus_service.c](/home/aolivier/hotswapd/src/dbus_service.c:379), [src/hsctl/hsctl.c](/home/aolivier/hotswapd/src/hsctl/hsctl.c:232) | `make`, target `hsctl monitor` and `busctl` workflow | Implemented | `ModuleAttached` now uses `DBUS_TYPE_UINT32` for `speed_mbps`. |
| USB attach/detach awareness | [src/device_monitor.c](/home/aolivier/hotswapd/src/device_monitor.c:182), [src/main.c](/home/aolivier/hotswapd/src/main.c:90) | `make`, hardware validation checklist | Implemented | Initial enumeration plus live udev events. |
| Registry-driven metadata and policy | [src/module_registry.c](/home/aolivier/hotswapd/src/module_registry.c:241), [src/device_monitor.c](/home/aolivier/hotswapd/src/device_monitor.c:173), [config/modules.json](/home/aolivier/hotswapd/config/modules.json:1) | [tests/test_registry.c](/home/aolivier/hotswapd/tests/test_registry.c:1), `make test` | Implemented | Exact match wins; category default policy is fallback; compile-time defaults are last resort. |
| Storage cleanup | [src/storage_handler.c](/home/aolivier/hotswapd/src/storage_handler.c:347), [src/main.c](/home/aolivier/hotswapd/src/main.c:133) | [tests/test_storage.c](/home/aolivier/hotswapd/tests/test_storage.c:1), `make test` | Implemented | Cleanup logic exists; real-device detach safety still needs hardware validation. |
| Conservative power reporting | [src/power_info.c](/home/aolivier/hotswapd/src/power_info.c:100) | [tests/test_power_info.c](/home/aolivier/hotswapd/tests/test_power_info.c:1), `make test` | Implemented | Legacy USB power and integer speed parsing are covered locally. |
| Conservative USB-C PD reporting | [src/power_info.c](/home/aolivier/hotswapd/src/power_info.c:153) | `make` | Implemented | Current behavior prefers no PD data over incorrect attribution. |
| Hardware-free testability where possible | [Makefile](/home/aolivier/hotswapd/Makefile:71), [tests/test_device_state.c](/home/aolivier/hotswapd/tests/test_device_state.c:1), [tests/test_power_info.c](/home/aolivier/hotswapd/tests/test_power_info.c:1), [tests/test_registry.c](/home/aolivier/hotswapd/tests/test_registry.c:1), [tests/test_storage.c](/home/aolivier/hotswapd/tests/test_storage.c:1) | `make test` | Implemented | Current local tests avoid live USB hardware and live system bus dependencies. |
| Honest hardware-validation claims | [README.md](/home/aolivier/hotswapd/README.md:89), [artifacts/hotswapd-validation-checklist.md](/home/aolivier/hotswapd/artifacts/hotswapd-validation-checklist.md:1) | Documentation review | Implemented | Repeated attach/detach and CM5 validation remain explicitly pending. |

## Current Local Proof

The following can be demonstrated on a normal Linux development host:

- clean compile with `-Werror`
- executable unit tests for registry parsing and reload behavior
- device-state behavior
- legacy power parsing, including low-speed rounding
- storage device-name extraction
- systemd unit syntax verification, subject to local environment limitations

## Required Target-Hardware Proof Still Pending

The following are requirements-level checks that cannot be honestly marked
complete from local automated tests alone:

- at least three physical module types
- at least five attach/detach cycles per module type
- real storage unclean-detach behavior
- no daemon crashes or visible system instability during repeated cycling
- target-image installation and service startup on CM5/postmarketOS
- any accurate per-device USB-C PD observation on real hardware
