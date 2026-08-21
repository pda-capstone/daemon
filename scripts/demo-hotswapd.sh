#!/usr/bin/env bash
#
# Guided target-hardware demonstration for hotswapd.
#
# This script exercises the installed systemd service and hsctl D-Bus client.
# It does not simulate udev or GPIO events: attach, detach, and safe-release
# actions are performed with real hardware by the presenter.
#
# SPDX-License-Identifier: GPL-3.0-only

set -Eeuo pipefail

SERVICE=${HOTSWAPD_SERVICE:-hotswapd.service}
HSCTL_BIN=${HSCTL:-hsctl}
REGISTRY=${HOTSWAPD_REGISTRY:-/etc/hotswapd/modules.json}
DBUS_NAME=org.postmarketos.HotSwap
TIMEOUT=20
CYCLES=1
SAFE_RELEASE=1
RUN_UNCLEAN_TEST=0
PREFLIGHT_ONLY=0
OUTPUT_DIR=
STORAGE_MOUNT=
MONITOR_PID=
EVENT_LOG=
REPORT_LOG=
SESSION_START=
SESSION_ID=
PASSED_CYCLES=0
ATTEMPTED_CYCLES=0

usage() {
  cat <<'EOF'
Usage: scripts/demo-hotswapd.sh [options]

Run a guided demonstration against the installed hotswapd service and real
USB mass-storage module. By default, the script guides one safe-release cycle.

Options:
  --cycles N                 Run N storage cycles (default: 1).
  --validation               Run 5 storage cycles. This is the storage-focused
                             subset of the broader three-module requirement.
  --no-safe-release          Do not wait for the GPIO ready signal. Physical
                             removal will be reported as unclean.
  --unclean-storage-test     Add an opt-in, controlled storage surprise-removal
                             test using an 8 MiB synced payload.
  --storage-mount PATH       Storage mount used by the unclean-removal test.
                             Otherwise, auto-detect /run/media/hotswapd/*.
  --timeout SECONDS          Event wait timeout (default: 20).
  --output DIRECTORY         Store logs in DIRECTORY (default: a /tmp folder).
  --preflight-only           Show service, D-Bus, registry, module, and power
                             status without prompting for hardware cycles.
  -h, --help                 Show this help.

Environment overrides:
  HOTSWAPD_SERVICE, HOTSWAPD_REGISTRY, HSCTL

Examples:
  scripts/demo-hotswapd.sh
  scripts/demo-hotswapd.sh --validation
  scripts/demo-hotswapd.sh --cycles 2
  scripts/demo-hotswapd.sh --unclean-storage-test
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

warn() {
  printf 'WARNING: %s\n' "$*" >&2
}

is_positive_integer() {
  case "$1" in
    ''|*[!0-9]*|0) return 1 ;;
    *) return 0 ;;
  esac
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --cycles)
      [ "$#" -ge 2 ] || die "--cycles requires a value"
      CYCLES=$2
      shift 2
      ;;
    --validation)
      CYCLES=5
      shift
      ;;
    --no-safe-release)
      SAFE_RELEASE=0
      shift
      ;;
    --unclean-storage-test)
      RUN_UNCLEAN_TEST=1
      shift
      ;;
    --storage-mount)
      [ "$#" -ge 2 ] || die "--storage-mount requires a path"
      STORAGE_MOUNT=$2
      shift 2
      ;;
    --timeout)
      [ "$#" -ge 2 ] || die "--timeout requires a value"
      TIMEOUT=$2
      shift 2
      ;;
    --output)
      [ "$#" -ge 2 ] || die "--output requires a directory"
      OUTPUT_DIR=$2
      shift 2
      ;;
    --preflight-only)
      PREFLIGHT_ONLY=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: $1"
      ;;
  esac
done

is_positive_integer "$CYCLES" || die "cycle count must be a positive integer"
is_positive_integer "$TIMEOUT" || die "timeout must be a positive integer"

SESSION_ID=$(date +%Y%m%d-%H%M%S)-$$
SESSION_START=$(date --iso-8601=seconds 2>/dev/null || date '+%Y-%m-%d %H:%M:%S')
if [ -z "$OUTPUT_DIR" ]; then
  OUTPUT_DIR=${TMPDIR:-/tmp}/hotswapd-demo-$SESSION_ID
fi
mkdir -p -- "$OUTPUT_DIR"
REPORT_LOG=$OUTPUT_DIR/session-$SESSION_ID.log
EVENT_LOG=$OUTPUT_DIR/events-$SESSION_ID.log
: >"$REPORT_LOG"
: >"$EVENT_LOG"

exec > >(tee -a "$REPORT_LOG") 2>&1

cleanup() {
  if [ -n "$MONITOR_PID" ] && kill -0 "$MONITOR_PID" 2>/dev/null; then
    kill "$MONITOR_PID" 2>/dev/null || true
    wait "$MONITOR_PID" 2>/dev/null || true
  fi
  MONITOR_PID=
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

heading() {
  printf '\n============================================================\n'
  printf '%s\n' "$1"
  printf '============================================================\n'
}

prompt_action() {
  local reply
  printf '\n%s\n' "$1"
  printf 'Press Enter when complete, or type s to skip: '
  IFS= read -r reply
  case "$reply" in
    s|S) return 1 ;;
    *) return 0 ;;
  esac
}

confirm_surprise_removal() {
  local reply
  printf '\n%s\n' "$1"
  printf 'Type REMOVE to continue, or anything else to cancel: '
  IFS= read -r reply
  [ "$reply" = REMOVE ]
}

line_count() {
  wc -l <"$EVENT_LOG" | tr -d ' '
}

next_event_line() {
  local count
  count=$(line_count)
  printf '%s\n' "$((count + 1))"
}

show_events_from() {
  local first_line=$1
  local last_line
  last_line=$(line_count)
  if [ "$last_line" -ge "$first_line" ]; then
    printf '\nD-Bus events observed:\n'
    sed -n "${first_line},${last_line}p" "$EVENT_LOG"
  fi
}

wait_for_event() {
  local first_line=$1
  local pattern=$2
  local deadline=$((SECONDS + TIMEOUT))
  while [ "$SECONDS" -lt "$deadline" ]; do
    if tail -n +"$first_line" "$EVENT_LOG" | grep -Eq "$pattern"; then
      return 0
    fi
    if [ -n "$MONITOR_PID" ] && ! kill -0 "$MONITOR_PID" 2>/dev/null; then
      warn "hsctl monitor exited unexpectedly"
      return 1
    fi
    sleep 1
  done
  return 1
}

event_segment_has() {
  local first_line=$1
  local pattern=$2
  tail -n +"$first_line" "$EVENT_LOG" | grep -Eq "$pattern"
}

attached_devpath_from() {
  local first_line=$1
  tail -n +"$first_line" "$EVENT_LOG" |
    sed -n 's/^\[ATTACH\] Path: \([^ ]*\) |.*/\1/p' |
    tail -n 1
}

current_module_count() {
  "$HSCTL_BIN" list | awk 'NR > 2 && $1 ~ /^\// { count++ } END { print count + 0 }'
}

only_current_devpath() {
  local count
  count=$(current_module_count)
  if [ "$count" -eq 1 ]; then
    "$HSCTL_BIN" list | awk 'NR > 2 && $1 ~ /^\// { print $1; exit }'
  fi
}

show_live_state() {
  local devpath=${1:-}
  printf '\nCurrent module registry state:\n'
  "$HSCTL_BIN" list
  printf '\nCurrent power accounting:\n'
  "$HSCTL_BIN" power
  if [ -n "$devpath" ]; then
    printf '\nDetailed module properties:\n'
    "$HSCTL_BIN" info "$devpath" || warn "could not query $devpath"
  fi
}

module_property() {
  local devpath=$1
  local property=$2
  "$HSCTL_BIN" info "$devpath" |
    awk -F: -v property="$property" '
      $1 ~ "^[[:space:]]*" property "[[:space:]]*$" {
        value=$2
        sub(/^[[:space:]]+/, "", value)
        sub(/[[:space:]]+$/, "", value)
        print value
        exit
      }'
}

wait_for_mounted_storage() {
  local devpath=$1
  local deadline=$((SECONDS + TIMEOUT))
  local category mount_count
  while [ "$SECONDS" -lt "$deadline" ]; do
    category=$(module_property "$devpath" category 2>/dev/null || true)
    mount_count=$(module_property "$devpath" mount_count 2>/dev/null || true)
    if [ "$category" = storage ] &&
       case "$mount_count" in ''|*[!0-9]*) false ;; *) [ "$mount_count" -gt 0 ] ;; esac; then
      return 0
    fi
    sleep 1
  done
  return 1
}

start_monitor() {
  require_command stdbuf
  stdbuf -oL -eL "$HSCTL_BIN" monitor >"$EVENT_LOG" 2>&1 &
  MONITOR_PID=$!
  sleep 1
  kill -0 "$MONITOR_PID" 2>/dev/null || {
    sed -n '1,120p' "$EVENT_LOG"
    die "hsctl monitor did not remain running"
  }
}

stop_monitor() {
  cleanup
}

capture_journal() {
  local journal_log=$OUTPUT_DIR/journal-$SESSION_ID.log
  local kernel_log=$OUTPUT_DIR/kernel-$SESSION_ID.log
  journalctl -u "$SERVICE" --since "$SESSION_START" --no-pager \
    >"$journal_log" 2>&1 || warn "could not capture the service journal"
  journalctl -k --since "$SESSION_START" --no-pager \
    >"$kernel_log" 2>&1 || warn "could not capture the kernel journal"
  printf 'Service journal: %s\n' "$journal_log"
  printf 'Kernel journal: %s\n' "$kernel_log"
}

show_registry() {
  printf '\nConfigured module registry: %s\n' "$REGISTRY"
  if [ ! -r "$REGISTRY" ]; then
    warn "registry is not readable"
    return
  fi

  if command -v jq >/dev/null 2>&1; then
    jq -e . "$REGISTRY" >/dev/null || die "registry is not valid JSON"
    jq -r '.modules[]? |
      "  \(.vendor_id):\(.product_id)  \(.category)  \(.name)"' "$REGISTRY"
    printf '  Defaults: '
    jq -r '.defaults | keys | join(", ")' "$REGISTRY"
  else
    warn "jq is unavailable; displaying registry identifiers without JSON validation"
    grep -E '"(vendor_id|product_id|name|category)"' "$REGISTRY" || true
  fi
}

preflight() {
  heading "1. Service and IPC preflight"

  [ "$(uname -s)" = Linux ] || die "this demonstration requires Linux"
  require_command systemctl
  require_command busctl
  require_command journalctl
  require_command awk
  require_command grep
  require_command sed
  require_command tail
  require_command tee
  command -v "$HSCTL_BIN" >/dev/null 2>&1 ||
    die "hsctl executable not found: $HSCTL_BIN"

  if ! systemctl is-active --quiet "$SERVICE"; then
    printf '%s is not active; starting it with sudo.\n' "$SERVICE"
    require_command sudo
    sudo systemctl start "$SERVICE"
  fi
  systemctl is-active --quiet "$SERVICE" || die "$SERVICE failed to start"

  printf 'Service active: yes\n'
  printf 'Target: %s\n' "$(uname -a)"
  if systemctl is-enabled --quiet "$SERVICE"; then
    printf 'Starts automatically at boot: yes\n'
  else
    warn "$SERVICE is active but is not enabled at boot"
  fi

  if busctl --system list --no-pager --no-legend |
      awk -v name="$DBUS_NAME" '$1 == name { found=1 } END { exit !found }'; then
    printf 'D-Bus name acquired: %s\n' "$DBUS_NAME"
  else
    die "$DBUS_NAME is not present on the system bus"
  fi

  show_registry
  show_live_state
}

run_hardware_cycle() {
  local label=$1
  local cycle=$2
  local attach_line detach_line release_line devpath

  heading "Module: $label — cycle $cycle of $CYCLES"
  ATTEMPTED_CYCLES=$((ATTEMPTED_CYCLES + 1))

  attach_line=$(next_event_line)
  if ! prompt_action "Install the jumper or close the release contact to hold GPIO26 low, then connect the $label module."; then
    printf 'Cycle skipped.\n'
    return 2
  fi

  if ! wait_for_event "$attach_line" '^\[ATTACH\]'; then
    warn "no ModuleAttached signal arrived within $TIMEOUT seconds"
    show_live_state
    return 1
  fi
  show_events_from "$attach_line"

  devpath=$(attached_devpath_from "$attach_line")
  if [ -z "$devpath" ]; then
    devpath=$(only_current_devpath || true)
  fi
  if [ -z "$devpath" ]; then
    warn "could not identify the attached storage DEVPATH"
    return 1
  fi
  if ! wait_for_mounted_storage "$devpath"; then
    warn "the attached module was not classified and mounted as storage within $TIMEOUT seconds"
    return 1
  fi
  printf 'Storage classification and automatic mount: PASS\n'
  show_live_state "$devpath"

  if [ "$SAFE_RELEASE" -eq 1 ]; then
    if [ "$(current_module_count)" -ne 1 ]; then
      warn "more than one module is tracked; GPIO release needs a configured DEVPATH prefix"
    fi

    release_line=$(next_event_line)
    if ! prompt_action \
      "Open the GPIO release contact or remove the jumper between physical pins 37 and 39. Do not remove the module until READY is reported."; then
      printf 'Safe-release portion skipped; leave the module connected.\n'
      return 2
    fi

    if ! wait_for_event "$release_line" '^\[(READY |FAILED)\]'; then
      warn "no ready/failed signal arrived within $TIMEOUT seconds; DO NOT remove the module"
      show_events_from "$release_line"
      return 1
    fi
    show_events_from "$release_line"

    if event_segment_has "$release_line" '^\[FAILED\]'; then
      warn "safe release failed; DO NOT remove the module"
      return 1
    fi
    if ! event_segment_has "$release_line" '^\[READY \]'; then
      warn "a READY signal was not observed; DO NOT remove the module"
      return 1
    fi
    if [ "$(module_property "$devpath" mount_count 2>/dev/null || true)" != 0 ]; then
      warn "the module still reports a mounted filesystem; DO NOT remove it"
      return 1
    fi
    printf 'Tracked filesystems safely unmounted: yes\n'

    detach_line=$(next_event_line)
    if ! prompt_action "READY was observed. Physically remove the $label module now."; then
      printf 'Removal skipped; the module remains in detaching state.\n'
      return 2
    fi
  else
    warn "safe release is disabled; removing mounted storage can cause data loss"
    detach_line=$(next_event_line)
    if ! confirm_surprise_removal \
      "The daemon should classify this as unclean. Physically remove the $label module only after confirming below."; then
      printf 'Surprise removal cancelled.\n'
      return 2
    fi
  fi

  if ! wait_for_event "$detach_line" '^\[DETACH\]'; then
    warn "no ModuleDetached signal arrived within $TIMEOUT seconds"
    return 1
  fi
  show_events_from "$detach_line"

  if [ "$SAFE_RELEASE" -eq 1 ]; then
    if ! event_segment_has "$detach_line" 'Clean detach: YES'; then
      warn "detach was not reported clean"
      return 1
    fi
  elif ! event_segment_has "$detach_line" 'NO \(UNCLEAN'; then
    warn "detach was not reported unclean as expected"
    return 1
  fi

  systemctl is-active --quiet "$SERVICE" || die "$SERVICE stopped during cycling"
  PASSED_CYCLES=$((PASSED_CYCLES + 1))
  printf 'Cycle result: PASS (daemon remains active)\n'
  return 0
}

find_storage_mount() {
  local candidate
  local matches=()

  if [ -n "$STORAGE_MOUNT" ]; then
    if mountpoint -q -- "$STORAGE_MOUNT"; then
      printf '%s\n' "$STORAGE_MOUNT"
      return 0
    fi
    return 1
  fi

  shopt -s nullglob
  for candidate in /run/media/hotswapd/*; do
    if [ -d "$candidate" ] && mountpoint -q -- "$candidate"; then
      matches+=("$candidate")
    fi
  done
  shopt -u nullglob

  if [ "${#matches[@]}" -eq 1 ]; then
    printf '%s\n' "${matches[0]}"
    return 0
  fi
  return 1
}

wait_for_storage_mount() {
  local deadline=$((SECONDS + TIMEOUT))
  local found
  while [ "$SECONDS" -lt "$deadline" ]; do
    if found=$(find_storage_mount); then
      printf '%s\n' "$found"
      return 0
    fi
    sleep 1
  done
  return 1
}

run_unclean_storage_test() {
  local attach_line detach_line release_line storage_path test_name test_dir

  heading "Controlled storage surprise-removal demonstration"
  require_command dd
  require_command mountpoint
  require_command sha256sum
  require_command sync

  printf '%s\n' \
    'This optional test writes and explicitly syncs an 8 MiB payload, then' \
    'asks for removal without using safe release. It demonstrates detection' \
    'and conservative cleanup; it cannot prove zero data loss for every yank.'

  attach_line=$(next_event_line)
  if ! prompt_action "Install the jumper or close the release contact, then connect one storage module and wait for it to mount."; then
    printf 'Unclean-removal test skipped.\n'
    return 2
  fi
  wait_for_event "$attach_line" '^\[ATTACH\]' ||
    warn "no new attach signal was observed; checking current mounts"

  if ! storage_path=$(wait_for_storage_mount); then
    warn "could not find exactly one hotswapd storage mount; use --storage-mount PATH"
    return 1
  fi
  printf 'Storage mount: %s\n' "$storage_path"

  test_name=hotswapd-demo-$SESSION_ID
  test_dir=$storage_path/$test_name
  if [ -e "$test_dir" ]; then
    die "refusing to overwrite existing test path: $test_dir"
  fi
  mkdir -- "$test_dir"
  dd if=/dev/urandom of="$test_dir/payload.bin" bs=1M count=8 status=none
  (
    cd -- "$test_dir"
    sha256sum payload.bin >payload.sha256
  )
  sync
  printf 'Created and synced %s/payload.bin\n' "$test_dir"

  detach_line=$(next_event_line)
  if ! confirm_surprise_removal \
    "Controlled surprise-removal step: remove the storage module WITHOUT opening the GPIO release contact."; then
    warn "test data remains at $test_dir"
    return 2
  fi
  if ! wait_for_event "$detach_line" '^\[DETACH\]'; then
    warn "no detach signal arrived within $TIMEOUT seconds"
    return 1
  fi
  show_events_from "$detach_line"
  if ! event_segment_has "$detach_line" 'NO \(UNCLEAN'; then
    warn "the removal was not classified as unclean"
    return 1
  fi
  systemctl is-active --quiet "$SERVICE" || die "$SERVICE stopped after surprise removal"

  attach_line=$(next_event_line)
  if ! prompt_action "Close the release contact or reinstall the jumper, then reconnect the storage module so the synced payload can be verified."; then
    warn "verification skipped; test data remains in $test_name"
    return 2
  fi
  wait_for_event "$attach_line" '^\[ATTACH\]' ||
    warn "no attach signal observed while waiting for remount"
  if ! storage_path=$(wait_for_storage_mount); then
    warn "storage did not remount within $TIMEOUT seconds"
    return 1
  fi
  test_dir=$storage_path/$test_name
  if [ ! -d "$test_dir" ]; then
    warn "the demonstration directory was not found after reattach"
    return 1
  fi
  (
    cd -- "$test_dir"
    sha256sum -c payload.sha256
  )
  printf 'Synced payload verification: PASS\n'

  rm -f -- "$test_dir/payload.bin" "$test_dir/payload.sha256"
  rmdir -- "$test_dir"
  sync
  printf 'Removed the demonstration payload and synced the filesystem.\n'

  if [ "$SAFE_RELEASE" -eq 1 ]; then
    release_line=$(next_event_line)
    if prompt_action \
      "Open the GPIO release contact or remove the jumper to leave the storage device safely unmounted."; then
      if wait_for_event "$release_line" '^\[(READY |FAILED)\]' &&
          event_segment_has "$release_line" '^\[READY \]'; then
        show_events_from "$release_line"
        printf 'Storage is ready for removal.\n'
      else
        warn "READY was not observed; leave the storage device connected"
      fi
    fi
  else
    warn "storage remains attached; unmount it safely before removal"
  fi
}

heading "hotswapd target-hardware demonstration"
printf 'Session: %s\n' "$SESSION_ID"
printf 'Evidence directory: %s\n' "$OUTPUT_DIR"
printf 'Requirements focus: attach/detach, registry, D-Bus UI events, power, and storage safety.\n'

preflight

if [ "$PREFLIGHT_ONLY" -eq 1 ]; then
  capture_journal
  heading "Preflight complete"
  printf 'Session log: %s\n' "$REPORT_LOG"
  exit 0
fi

heading "2. Live D-Bus event monitor"
start_monitor
printf 'hsctl monitor is running; subsequent hardware events will be captured.\n'
printf 'For safe-release cycles, keep only the demonstrated module connected unless\n'
printf 'the service has a release DEVPATH prefix configured.\n'
printf 'Begin each cycle with the USB mass-storage module disconnected and the release contact closed.\n'

for ((cycle = 1; cycle <= CYCLES; cycle++)); do
  if run_hardware_cycle "USB mass storage" "$cycle"; then
    :
  else
    result=$?
    if [ "$result" -eq 2 ]; then
      warn "USB mass-storage cycle $cycle was skipped"
    else
      warn "USB mass-storage cycle $cycle did not pass"
    fi
  fi
done

if [ "$RUN_UNCLEAN_TEST" -eq 1 ]; then
  if ! run_unclean_storage_test; then
    warn "controlled storage surprise-removal test was incomplete or failed"
  fi
fi

stop_monitor
capture_journal

heading "Demonstration summary"
printf 'Hardware cycles passed: %d/%d\n' "$PASSED_CYCLES" "$ATTEMPTED_CYCLES"
printf 'Daemon still active: '
if systemctl is-active --quiet "$SERVICE"; then
  printf 'yes\n'
else
  printf 'NO\n'
fi
if [ "$RUN_UNCLEAN_TEST" -eq 1 ]; then
  printf 'Controlled unclean-removal test: requested; see session events above.\n'
else
  printf 'Controlled unclean-removal test: not requested.\n'
fi
printf 'Event log: %s\n' "$EVENT_LOG"
printf 'Session log: %s\n' "$REPORT_LOG"
printf '\nFor requirements-level validation, use --validation and retain this evidence directory.\n'
