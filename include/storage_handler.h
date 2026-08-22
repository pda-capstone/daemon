/*
 * storage_handler.h — Storage-specific attach/detach handling.
 *
 * Manages mount point tracking, the idle-sync / periodic-sync model,
 * and graceful cleanup on device removal.
 *
 * Sync model:
 *   - IDLE mode (default): sync after idle_sync_delay of no write
 *     activity, with a fallback_sync_interval for continuously dirty
 *     devices.
 *   - PERIODIC mode: sync at a fixed interval regardless of activity.
 *   - MANUAL mode: sync only on explicit eject or before detach.
 *   - DISABLED mode: no automatic syncing (not recommended for
 *     removable storage).
 *
 * SPDX-FileCopyrightText: 2026 Alexander Olivier
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HOTSWAPD_STORAGE_HANDLER_H
#define HOTSWAPD_STORAGE_HANDLER_H

#include "hotswapd.h"

/* ── Default timing constants ────────────────────────────────────────────── */

#define STORAGE_DEFAULT_IDLE_SYNC_DELAY_S 5
#define STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S 60
#define STORAGE_ATTACH_DISCOVERY_INTERVAL_MS 200
#define STORAGE_ATTACH_DISCOVERY_TIMEOUT_MS 10000
#define STORAGE_ATTACH_MAX_ATTEMPTS                                            \
    (STORAGE_ATTACH_DISCOVERY_TIMEOUT_MS / STORAGE_ATTACH_DISCOVERY_INTERVAL_MS)

enum storage_attach_result {
    STORAGE_ATTACH_ERROR = -1,
    STORAGE_ATTACH_PENDING = 0,
    STORAGE_ATTACH_COMPLETE = 1
};

/*
 * Narrow platform boundary used by rootless unit tests. Production callers
 * use the built-in sysfs, procfs, mount, syncfs, and umount2 operations.
 */
struct storage_operations {
    int (*scan_mounts)(struct hs_device *dev);
    int (*discover_block_devices)(const struct hs_device *dev,
                                  char paths[][PATH_MAX], size_t max_paths);
    int (*mount_device)(const char *source, const char *target,
                        const char *options);
    int (*mount_matches)(const char *source, const char *target);
    int (*unmount_path)(const char *target, int flags);
    int (*sync_mount)(const char *target);
};

/* ── Attach / detach handlers ────────────────────────────────────────────── */

/**
 * Post-attach processing for a storage device.
 *
 * Scans existing mounts immediately, then starts a short timerfd-driven,
 * bounded discovery window when block nodes or an automount are not ready.
 *
 * @param dev  The storage device record (already added to state).
 * @return 0 on success, -1 on error.
 */
int storage_on_attach(struct hs_device *dev);

/** Process one non-blocking attach discovery/mount attempt. */
int storage_process_attach_once(struct hs_device *dev);

/** Handle one attach-discovery timer expiry. */
int storage_handle_attach_timer(struct hs_device *dev);

/** Close the discovery timer and start sync tracking when applicable. */
int storage_finish_attach(struct hs_device *dev);

/** Cancel pending discovery without starting new work. */
void storage_cancel_attach(struct hs_device *dev);

/**
 * Detach cleanup for a storage device.
 *
 * 1. Stops pending discovery and sync timers.
 * 2. On an orderly detach, calls syncfs() and normally unmounts each verified
 *    tracked filesystem.
 * 3. On surprise removal, skips unsafe post-removal sync and lazily unmounts
 *    only mount entries whose source still matches cached state.
 * 4. Sets *was_unclean if the device was removed without a preceding
 *    DETACHING state.
 *
 * @param dev          The storage device record.
 * @param was_unclean  Output: set to 1 if detach was unclean.
 * @return 0 on success (cleanup completed), -1 on error.
 */
int storage_on_detach(struct hs_device *dev, int *was_unclean);

/**
 * Flush and normally unmount all filesystems for a still-connected module.
 * This is called when its physical release contact opens, before the USB
 * connector is removed.
 *
 * @return 0 only when every current mount is safely unmounted; -1 otherwise.
 */
int storage_prepare_release(struct hs_device *dev);

/* ── Mount point tracking ────────────────────────────────────────────────── */

/**
 * Scan /proc/mounts and populate dev->mount_points with any filesystems
 * mounted from partitions belonging to this USB device.
 *
 * @return Number of mount points found.
 */
int storage_scan_mounts(struct hs_device *dev);

/* ── Sync timer management ───────────────────────────────────────────────── */

/**
 * Create a timerfd for the device's sync policy and store it in
 * dev->sync_timer_fd.
 *
 * @param dev               Device record.
 * @param idle_delay_s      Seconds of idle before sync (for IDLE mode).
 * @param fallback_interval_s  Periodic fallback interval (for IDLE mode).
 * @return The timerfd (>= 0), or -1 on error.
 */
int storage_start_sync_timer(struct hs_device *dev, int idle_delay_s,
                             int fallback_interval_s);

/**
 * Stop and close the sync timer for a device.
 */
void storage_stop_sync_timer(struct hs_device *dev);

/**
 * Handle a sync timer expiry.  Reads the timerfd, performs syncfs()
 * on the device's mounted filesystems if appropriate, and re-arms
 * the timer according to the sync policy.
 *
 * @param dev  The device whose sync timer fired.
 * @return 0 on success, -1 on error.
 */
int storage_handle_sync_timer(struct hs_device *dev);

/* ── Utilities ───────────────────────────────────────────────────────────── */

/**
 * Extract the base disk name from a block device path.
 *
 * Examples:
 *   /dev/sdb1 -> sdb
 *   /dev/mmcblk0p1 -> mmcblk0
 *   /dev/nvme0n1p1 -> nvme0n1
 *
 * @return 0 on success, -1 on invalid input.
 */
int storage_extract_disk_name(const char *blkdev, char *buf, size_t buflen);

/**
 * Resolve a block device node (e.g. /dev/sdb1) to its parent USB
 * device's sysfs path.
 *
 * @param blkdev   Block device path (e.g. "/dev/sdb1").
 * @param buf      Output buffer for the parent USB sysfs path.
 * @param buflen   Size of output buffer.
 * @return 0 on success, -1 if the device is not USB-backed.
 */
int storage_resolve_usb_parent(const char *blkdev, char *buf, size_t buflen);

/** Override/reset the storage platform boundary for rootless tests. */
void storage_set_operations(const struct storage_operations *operations);
void storage_reset_operations(void);

#endif /* HOTSWAPD_STORAGE_HANDLER_H */
