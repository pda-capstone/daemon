/*
 * hotswapd.h — Core types and constants for the USB hot-swap daemon.
 *
 * This header defines the shared data structures used across all daemon
 * components. The design keeps privilege-sensitive operations (mount, umount,
 * sync) in dedicated handler modules rather than exposing generic execution
 * hooks, so that future privilege separation can restrict the root-running
 * core to a narrow set of syscalls.
 *
 * SPDX-FileCopyrightText: 2026 Alexander Olivier
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HOTSWAPD_H
#define HOTSWAPD_H

#include <limits.h> /* PATH_MAX */
#include <stdint.h>
#include <time.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define HOTSWAPD_VERSION_MAJOR 0
#define HOTSWAPD_VERSION_MINOR 2
#define HOTSWAPD_VERSION_PATCH 0
#define HOTSWAPD_VERSION_STRING "0.2.0"

/* ── D-Bus identifiers ───────────────────────────────────────────────────── */

#define HOTSWAP_DBUS_BUS_NAME "org.postmarketos.HotSwap"
#define HOTSWAP_DBUS_OBJECT_PATH "/org/postmarketos/HotSwap"
#define HOTSWAP_DBUS_INTERFACE "org.postmarketos.HotSwap"

/* ── Paths ───────────────────────────────────────────────────────────────── */

#define HOTSWAP_DEFAULT_REGISTRY_PATH "/etc/hotswapd/modules.json"
#define HOTSWAP_PID_FILE "/run/hotswapd.pid"

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define HOTSWAP_MAX_MOUNT_POINTS 8
#define HOTSWAP_MAX_DEVPATH 256
#define HOTSWAP_MAX_ID 8
#define HOTSWAP_MAX_NAME 128
#define HOTSWAP_MAX_SERIAL 256
#define HOTSWAP_MAX_ROLE 16

/* ── Device categories ───────────────────────────────────────────────────── */

enum device_category {
  DEV_CAT_UNKNOWN = 0,
  DEV_CAT_STORAGE,
  DEV_CAT_HID,
  DEV_CAT_SERIAL,
  DEV_CAT_NETWORK,
  DEV_CAT_AUDIO,
  DEV_CAT_VIDEO,
  DEV_CAT_HUB,
  DEV_CAT_COUNT /* sentinel — number of categories */
};

/* ── Device states ───────────────────────────────────────────────────────── */

enum device_state {
  DEV_STATE_ATTACHED = 0,
  DEV_STATE_DETACHING, /* clean detach requested but not yet complete */
  DEV_STATE_DETACHED
};

/* ── Sync modes (per-device storage policy) ──────────────────────────────── */

enum sync_mode {
  SYNC_MODE_IDLE = 0, /* sync after idle_sync_delay of no writes     */
  SYNC_MODE_PERIODIC, /* sync every fallback_sync_interval           */
  SYNC_MODE_MANUAL,   /* sync only on explicit request / before eject*/
  SYNC_MODE_DISABLED  /* no automatic syncing                        */
};

/* ── Registry actions copied into each attached device ──────────────────── */

struct module_action {
  int has_action;
  char action[32];            /* "mount", "unmount", or "none"        */
  char options[256];          /* mount options, e.g. "flush,noatime"   */
  char mount_point[PATH_MAX]; /* optional; supports a {device} token    */
};

/* ── Core device record ──────────────────────────────────────────────────── */

struct hs_device {
  /* Identity — cached at add time (sysfs is gone on remove) */
  char devpath[HOTSWAP_MAX_DEVPATH];   /* udev DEVPATH — unique key    */
  char syspath[PATH_MAX];              /* /sys/devices/...             */
  char vendor_id[HOTSWAP_MAX_ID];      /* e.g. "0781"                  */
  char product_id[HOTSWAP_MAX_ID];     /* e.g. "5567"                  */
  char vendor_name[HOTSWAP_MAX_NAME];  /* e.g. "SanDisk"               */
  char product_name[HOTSWAP_MAX_NAME]; /* e.g. "Cruzer_Blade"          */
  char serial[HOTSWAP_MAX_SERIAL];     /* USB serial string            */

  enum device_category category;
  enum device_state state;

  /* Legacy USB power info (always available) */
  unsigned int max_power_ma; /* from bMaxPower                         */
  unsigned int speed_mbps;   /* from speed                             */
  int self_powered;          /* from bmAttributes bit 6                */

  /* USB-C Power Delivery info (optional — zeroed if unavailable) */
  int has_pd;
  unsigned int pd_voltage_uv; /* negotiated voltage in µV              */
  unsigned int pd_current_ua; /* negotiated current in µA              */
  char pd_power_role[HOTSWAP_MAX_ROLE]; /* "source" or "sink"   */

  /* Storage-specific fields */
  char mount_points[HOTSWAP_MAX_MOUNT_POINTS][PATH_MAX];
  char mount_sources[HOTSWAP_MAX_MOUNT_POINTS][PATH_MAX];
  int mount_count;
  struct module_action on_attach_action;
  struct module_action on_detach_action;
  int attach_timer_fd; /* bounded block discovery timer, or -1  */
  unsigned int attach_attempts;
  int sync_timer_fd; /* timerfd for periodic/idle sync, -1 if none */
  enum sync_mode sync_policy;
  int idle_sync_delay_s;
  int fallback_sync_interval_s;
  int dirty;                  /* set when write activity detected       */
  struct timespec last_write; /* timestamp of last detected write       */

  /* Timestamps */
  struct timespec attached_at;

  /* Linked list pointer */
  struct hs_device *next;
};

/* ── Utility functions ───────────────────────────────────────────────────── */

/**
 * Convert a device_category enum to a human-readable string.
 */
const char *category_to_string(enum device_category cat);

/**
 * Parse a category string (from JSON registry) to enum.
 * Returns DEV_CAT_UNKNOWN for unrecognized strings.
 */
enum device_category category_from_string(const char *str);

/**
 * Convert a device_state enum to a human-readable string.
 */
const char *state_to_string(enum device_state st);

#endif /* HOTSWAPD_H */
