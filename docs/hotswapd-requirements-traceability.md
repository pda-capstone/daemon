# hotswapd Requirements Traceability

This document maps the PDA hot-swap daemon requirements to the current
implementation, automated tests, and remaining hardware validation needs.

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
| C implementation | [src/main.c](../src/main.c#L1), [src/device_monitor.c](../src/device_monitor.c#L1), [src/dbus_service.c](../src/dbus_service.c#L1) | `make` | Implemented | Entire daemon and CLI are C code. |
| User-space daemon | [src/main.c](../src/main.c#L198), [config/hotswapd.service](../config/hotswapd.service#L1) | `make`, target install workflow | Implemented | No kernel modules or kernel-space hot-swap logic. |
| systemd-managed service | [config/hotswapd.service](../config/hotswapd.service#L1), [Makefile](../Makefile#L87) | `systemd-analyze verify config/hotswapd.service` | Implemented | Runs foreground with `ExecStart=/usr/sbin/hotswapd -f`. |
| D-Bus identity and notifications | [include/hotswapd.h](../include/hotswapd.h#L27), [src/dbus_service.c](../src/dbus_service.c#L379), [src/hsctl/hsctl.c](../src/hsctl/hsctl.c#L232) | `make`, target `hsctl monitor` and `busctl` workflow | Implemented | `ModuleAttached` now uses `DBUS_TYPE_UINT32` for `speed_mbps`. |
| USB attach/detach awareness | [src/device_monitor.c](../src/device_monitor.c#L182), [src/main.c](../src/main.c#L90) | `make`, hardware validation checklist | Implemented | Initial enumeration plus live udev events. |
| Registry-driven metadata and policy | [src/module_registry.c](../src/module_registry.c#L241), [src/device_monitor.c](../src/device_monitor.c#L173), [config/modules.json](../config/modules.json#L1) | [tests/test_registry.c](../tests/test_registry.c#L1), `make test` | Implemented | Exact match wins; category default policy is fallback; compile-time defaults are last resort. |
| Storage cleanup | [src/storage_handler.c](../src/storage_handler.c#L347), [src/main.c](../src/main.c#L133) | [tests/test_storage.c](../tests/test_storage.c#L1), `make test` | Implemented | Cleanup logic exists; real-device detach safety still needs hardware validation. |
| Conservative power reporting | [src/power_info.c](../src/power_info.c#L100) | [tests/test_power_info.c](../tests/test_power_info.c#L1), `make test` | Implemented | Legacy USB power and integer speed parsing are covered locally. |
| Conservative USB-C PD reporting | [src/power_info.c](../src/power_info.c#L153) | `make` | Implemented | Current behavior prefers no PD data over incorrect attribution. |
| Hardware-free testability where possible | [Makefile](../Makefile#L71), [tests/test_device_state.c](../tests/test_device_state.c#L1), [tests/test_power_info.c](../tests/test_power_info.c#L1), [tests/test_registry.c](../tests/test_registry.c#L1), [tests/test_storage.c](../tests/test_storage.c#L1) | `make test` | Implemented | Current local tests avoid live USB hardware and live system bus dependencies. |
| Honest hardware-validation claims | [README.md](../README.md), [docs/hotswapd-validation-checklist.md](hotswapd-validation-checklist.md) | Documentation review | Implemented | Repeated attach/detach and CM5 validation remain explicitly pending. |

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
