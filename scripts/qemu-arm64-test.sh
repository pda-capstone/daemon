#!/usr/bin/env bash
#
# Boot an ARM64 Linux guest under QEMU and run hotswapd's tests in it.
#
# The guest OS must be installed once with the "install" command. The
# "provision" command installs build dependencies, and subsequent "test"
# invocations are unattended when SSH key authentication is configured.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

VM_DIR=${QEMU_VM_DIR:-"$HOME/qemu/hotswapd"}
VM_DISK=${QEMU_VM_DISK:-"$VM_DIR/debian-arm64.qcow2"}
USB_DISK=${QEMU_USB_DISK:-"$VM_DIR/usb-stick.raw"}
EFI_FIRMWARE=${QEMU_EFI_FIRMWARE:-"/opt/homebrew/share/qemu/edk2-aarch64-code.fd"}
QEMU_BIN=${QEMU_SYSTEM_AARCH64:-"qemu-system-aarch64"}
QEMU_IMG_BIN=${QEMU_IMG:-"qemu-img"}
SSH_PORT=${QEMU_SSH_PORT:-2222}
GUEST_USER=${QEMU_GUEST_USER:-"$USER"}
SSH_IDENTITY=${QEMU_SSH_IDENTITY:-}
MONITOR_SOCKET=${QEMU_MONITOR_SOCKET:-"$VM_DIR/monitor.sock"}
PID_FILE=${QEMU_PID_FILE:-"$VM_DIR/qemu.pid"}
REMOTE_DIR=${QEMU_REMOTE_DIR:-"/tmp/hotswapd-qemu-test"}
SSH_CONTROL_PATH=${QEMU_SSH_CONTROL_PATH:-"/tmp/hotswapd-qemu-ssh-$UID-$$"}

COMMAND=test
KEEP_RUNNING=0
REUSE_RUNNING=0
FORCE_INSTALL=0
ISO_PATH=
STARTED_QEMU=0

usage() {
  cat <<'EOF'
Usage:
  scripts/qemu-arm64-test.sh [test] [options]
  scripts/qemu-arm64-test.sh provision [options]
  scripts/qemu-arm64-test.sh install [ARM64_ISO] [options]

Commands:
  install       Create missing disks and open the graphical Debian installer.
  provision     Boot the guest and install hotswapd's build dependencies.
  test          Boot the guest, copy this source tree, build, and run make test.

Options:
  --user USER       SSH user in the guest (default: current host user).
  --identity FILE   SSH private key to use.
  --port PORT       Forwarded host SSH port (default: 2222).
  --keep-running    Leave a VM started by this script running after completion.
  --reuse-running   Use a VM already listening on the selected SSH port.
  --force-install   Boot the installer even when the VM disk already exists.
  -h, --help        Show this help.

Environment overrides:
  QEMU_VM_DIR, QEMU_VM_DISK, QEMU_USB_DISK, QEMU_EFI_FIRMWARE,
  QEMU_SYSTEM_AARCH64, QEMU_IMG, QEMU_GUEST_USER, QEMU_SSH_IDENTITY,
  QEMU_SSH_PORT, QEMU_REMOTE_DIR

Examples:
  scripts/qemu-arm64-test.sh install ~/Downloads/debian-arm64-netinst.iso
  scripts/qemu-arm64-test.sh provision --user pda
  scripts/qemu-arm64-test.sh test --user pda --identity ~/.ssh/id_ed25519
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    install|provision|test)
      COMMAND=$1
      shift
      if [ "$COMMAND" = install ] && [ "$#" -gt 0 ] && [ "${1#-}" = "$1" ]; then
        ISO_PATH=$1
        shift
      fi
      ;;
    --user)
      [ "$#" -ge 2 ] || die "--user requires a value"
      GUEST_USER=$2
      shift 2
      ;;
    --identity)
      [ "$#" -ge 2 ] || die "--identity requires a value"
      SSH_IDENTITY=$2
      shift 2
      ;;
    --port)
      [ "$#" -ge 2 ] || die "--port requires a value"
      SSH_PORT=$2
      shift 2
      ;;
    --keep-running)
      KEEP_RUNNING=1
      shift
      ;;
    --reuse-running)
      REUSE_RUNNING=1
      shift
      ;;
    --force-install)
      FORCE_INSTALL=1
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

command -v "$QEMU_BIN" >/dev/null 2>&1 ||
  die "$QEMU_BIN is not installed or not on PATH"
command -v ssh >/dev/null 2>&1 || die "ssh is not installed"
command -v ssh-keyscan >/dev/null 2>&1 || die "ssh-keyscan is not installed"
command -v nc >/dev/null 2>&1 || die "nc is not installed"
[ -f "$EFI_FIRMWARE" ] ||
  die "ARM64 UEFI firmware not found: $EFI_FIRMWARE"

mkdir -p "$VM_DIR"

ACCEL=tcg
CPU=max
if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ]; then
  ACCEL=hvf
  CPU=host
elif [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ] &&
     [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
  ACCEL=kvm
  CPU=host
fi

create_disks() {
  command -v "$QEMU_IMG_BIN" >/dev/null 2>&1 ||
    die "$QEMU_IMG_BIN is not installed or not on PATH"

  if [ ! -e "$VM_DISK" ]; then
    "$QEMU_IMG_BIN" create -f qcow2 "$VM_DISK" 24G
  fi
  if [ ! -e "$USB_DISK" ]; then
    "$QEMU_IMG_BIN" create -f raw "$USB_DISK" 512M
  fi
}

install_guest() {
  if [ -e "$VM_DISK" ] && [ "$FORCE_INSTALL" -eq 0 ]; then
    die "VM disk already exists: $VM_DISK
Debian may already be installed. Run the test or provision command instead.
To intentionally boot the installer against this disk, pass --force-install."
  fi

  create_disks
  [ -n "$ISO_PATH" ] ||
    die "provide the ARM64 installer ISO: $0 install /path/to/debian-arm64.iso"
  [ -f "$ISO_PATH" ] || die "installer ISO not found: $ISO_PATH"

  exec "$QEMU_BIN" \
    -name hotswapd-arm64-install \
    -machine "virt,accel=$ACCEL" \
    -cpu "$CPU" \
    -smp 4 \
    -m 4096 \
    -bios "$EFI_FIRMWARE" \
    -device virtio-gpu-pci,addr=1 \
    -display cocoa \
    -device qemu-xhci,id=xhci,addr=2 \
    -device usb-kbd,bus=xhci.0 \
    -device usb-tablet,bus=xhci.0 \
    -drive "if=none,file=$VM_DISK,format=qcow2,id=os" \
    -device virtio-blk-pci,drive=os,addr=3 \
    -device virtio-scsi-pci,id=scsi,addr=4 \
    -drive "if=none,file=$ISO_PATH,format=raw,media=cdrom,readonly=on,id=install" \
    -device scsi-cd,drive=install,bus=scsi.0 \
    -device virtio-net-pci,netdev=net0,addr=5 \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22"
}

if [ "$COMMAND" = install ]; then
  install_guest
fi

[ -f "$VM_DISK" ] ||
  die "VM disk not found: $VM_DISK (run the install command first)"
if [ ! -e "$USB_DISK" ]; then
  command -v "$QEMU_IMG_BIN" >/dev/null 2>&1 ||
    die "$QEMU_IMG_BIN is not installed or not on PATH"
  "$QEMU_IMG_BIN" create -f raw "$USB_DISK" 512M
fi

SSH_ARGS=(
  -p "$SSH_PORT"
  -o ConnectTimeout=10
  -o ControlMaster=auto
  -o ControlPersist=60
  -o "ControlPath=$SSH_CONTROL_PATH"
  -o ServerAliveInterval=15
  -o StrictHostKeyChecking=accept-new
)
if [ -n "$SSH_IDENTITY" ]; then
  [ -f "$SSH_IDENTITY" ] || die "SSH identity not found: $SSH_IDENTITY"
  SSH_ARGS+=(-i "$SSH_IDENTITY")
fi
SSH_DEST="$GUEST_USER@127.0.0.1"

port_is_in_use() {
  nc -z 127.0.0.1 "$SSH_PORT" >/dev/null 2>&1
}

guest_ssh_is_ready() {
  ssh-keyscan -T 2 -t ed25519 -p "$SSH_PORT" 127.0.0.1 >/dev/null 2>&1
}

monitor_command() {
  [ -S "$MONITOR_SOCKET" ] || return 0
  printf '%s\n' "$1" | nc -w 1 -U "$MONITOR_SOCKET" >/dev/null 2>&1 || true
}

cleanup() {
  status=$?
  trap - EXIT INT TERM

  if [ -S "$SSH_CONTROL_PATH" ]; then
    ssh "${SSH_ARGS[@]}" -O exit "$SSH_DEST" >/dev/null 2>&1 || true
  fi

  if [ "$STARTED_QEMU" -eq 1 ] && [ "$KEEP_RUNNING" -eq 0 ]; then
    printf 'Stopping QEMU...\n'
    monitor_command system_powerdown

    attempts=0
    while [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null &&
          [ "$attempts" -lt 20 ]; do
      sleep 1
      attempts=$((attempts + 1))
    done

    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
      kill "$(cat "$PID_FILE")" 2>/dev/null || true
    fi
  fi

  exit "$status"
}
trap cleanup EXIT INT TERM

start_guest() {
  if port_is_in_use; then
    if [ "$REUSE_RUNNING" -eq 1 ]; then
      printf 'Reusing the VM listening on SSH port %s.\n' "$SSH_PORT"
      return
    fi
    die "port $SSH_PORT is already in use; use --reuse-running if it is this VM"
  fi

  rm -f "$MONITOR_SOCKET" "$PID_FILE"
  "$QEMU_BIN" \
    -name hotswapd-arm64-test \
    -machine "virt,accel=$ACCEL" \
    -cpu "$CPU" \
    -smp 4 \
    -m 4096 \
    -bios "$EFI_FIRMWARE" \
    -device virtio-gpu-pci,addr=1 \
    -display none \
    -serial none \
    -parallel none \
    -device qemu-xhci,id=xhci,addr=2 \
    -drive "if=none,file=$VM_DISK,format=qcow2,id=os" \
    -device virtio-blk-pci,drive=os,addr=3 \
    -device virtio-scsi-pci,id=scsi,addr=4 \
    -drive "if=none,file=$USB_DISK,format=raw,id=stick" \
    -device virtio-net-pci,netdev=net0,addr=5 \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22" \
    -monitor "unix:$MONITOR_SOCKET,server=on,wait=off" \
    -pidfile "$PID_FILE" \
    -daemonize
  STARTED_QEMU=1

  printf 'Waiting for guest SSH on port %s' "$SSH_PORT"
  deadline=$(($(date +%s) + 90))
  until guest_ssh_is_ready; do
    if [ "$(date +%s)" -ge "$deadline" ]; then
      printf '\n' >&2
      die "guest SSH did not become available within 90 seconds"
    fi
    printf '.'
    sleep 1
  done
  printf ' ready.\n'
  sleep 2
}

start_guest

if [ "$COMMAND" = provision ]; then
  printf 'Installing build dependencies in the ARM64 guest...\n'
  ssh -tt "${SSH_ARGS[@]}" "$SSH_DEST" \
    "sudo apt-get update && sudo apt-get install -y build-essential pkg-config libudev-dev libdbus-1-dev libjson-c-dev dbus systemd usbutils"
  printf 'Guest provisioning completed.\n'
  exit 0
fi

printf 'Checking guest architecture and build dependencies...\n'
ssh "${SSH_ARGS[@]}" "$SSH_DEST" \
  "test \"\$(uname -m)\" = aarch64 &&
   command -v gcc >/dev/null &&
   command -v pkg-config >/dev/null &&
   pkg-config --exists libudev dbus-1 json-c" ||
  die "guest is not ARM64 or lacks dependencies; run '$0 provision --user $GUEST_USER'"

printf 'Copying a clean source snapshot into the guest...\n'
ssh "${SSH_ARGS[@]}" "$SSH_DEST" \
  "mkdir -p '$REMOTE_DIR' && find '$REMOTE_DIR' -mindepth 1 -delete"
COPYFILE_DISABLE=1 tar \
  --no-xattrs \
  --no-mac-metadata \
  --exclude=.git \
  --exclude='*.o' \
  --exclude='tests/test_registry' \
  --exclude='tests/test_device_state' \
  --exclude='tests/test_power_info' \
  --exclude='tests/test_storage' \
  -czf - -C "$PROJECT_DIR" . |
  ssh "${SSH_ARGS[@]}" "$SSH_DEST" "tar -xzf - -C '$REMOTE_DIR'"

printf 'Building and testing on ARM64...\n'
ssh "${SSH_ARGS[@]}" "$SSH_DEST" \
  "set -eu
   cd '$REMOTE_DIR'
   make clean
   make -j2
   make test
   printf '\\nGuest architecture: '
   uname -m
   file hotswapd hsctl"

printf '\nARM64 QEMU build and tests passed.\n'
