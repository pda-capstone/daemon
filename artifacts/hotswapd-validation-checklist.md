# hotswapd Validation Checklist

This checklist separates what can be validated locally from what still needs a
systemd target and physical USB hardware.

## Local Developer Checks

Run in the repo:

```sh
make clean
make
make test
systemd-analyze verify config/hotswapd.service
```

Expected results:

- `make clean` succeeds
- `make` succeeds with `-Werror`
- `make test` builds and runs real executable tests
- `systemd-analyze verify` accepts the unit syntax

Local evidence covered by tests:

- registry parsing and malformed JSON rejection
- exact match and category-default sync policy lookup
- registry reload after rewrite
- registry live reload after atomic replacement rename
- invalid reload keeps the previous valid registry active
- device-state add/find/remove and duplicate attach handling
- legacy power parsing with `speed=480` and `speed=1.5`
- storage disk-name extraction for `sdX`, `sdX1`, `mmcblk0p1`, `nvme0n1p1`

## Target Systemd Checks

Run on a postmarketOS / systemd target:

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

Confirm:

- service starts successfully under systemd
- service is considered active after acquiring `org.postmarketos.HotSwap`
- journald captures daemon logs
- `hsctl` can query the live daemon on the system bus

## Physical Hardware Validation

These items require real USB hardware and should remain marked pending until
they are actually executed on the target platform.

### Module Types

- USB storage device
- USB HID device
- USB serial adapter or another real module type available to the team

### Repeated Cycling

For each module type:

- perform at least five attach/detach cycles
- confirm no daemon crash
- confirm no visible system instability
- confirm D-Bus attach/detach notifications continue to arrive

### Storage Safety

For a removable storage module:

- clean attach, verify mount detection if already mounted
- remove after normal idle use
- perform at least one controlled unclean detach test if safe
- verify daemon logs warn about unclean detach
- verify tracked mount points are cleaned up conservatively

### Registry Reload On Target

- update `/etc/hotswapd/modules.json` in place
- replace it using an atomic rename workflow
- confirm daemon reloads updated registry contents
- confirm malformed replacement does not discard the previous valid registry

### USB-C PD Reporting

- only mark as validated if the target hardware can tie PD sysfs data to the
  actual attached module or port
- if no reliable association is available, expected behavior is no PD data,
  not guessed PD data

## Recording Results

For each hardware validation session, capture:

- date
- target device / image identifier
- module type
- attach/detach cycle count
- whether behavior matched expectation
- log excerpts if a warning or failure occurred

If a validation item was not run, mark it as `pending hardware validation`
rather than inferring success from local tests.
