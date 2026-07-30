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
 * SPDX-License-Identifier: MIT
 */

#ifndef HOTSWAPD_STORAGE_HANDLER_H
#define HOTSWAPD_STORAGE_HANDLER_H

#include "hotswapd.h"

/* Default timing constants */

#define STORAGE_DEFAULT_IDLE_SYNC_DELAY_S 5
#define STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S 60

/* Attach / detach handlers */

/**
 * Post-attach processing for a storage device.
 *
 * 1. Waits briefly for the kernel to create block device nodes.
 * 2. Scans /proc/mounts for partitions belonging to this device.
 * 3. Records mount points in the device record.
 * 4. Starts the sync timer according to the device's sync policy.
 *
 * @param dev  The storage device record (already added to state).
 * @return 0 on success, -1 on error.
 */
int storage_on_attach(struct hs_device *dev);

/**
 * Detach cleanup for a storage device.
 *
 * 1. Calls sync() to flush all dirty buffers system-wide.
 * 2. Performs lazy unmount (MNT_DETACH) on each tracked mount point.
 * 3. Stops the sync timer.
 * 4. Sets *was_unclean if the device was removed without a preceding
 *    DETACHING state.
 *
 * @param dev          The storage device record.
 * @param was_unclean  Output: set to 1 if detach was unclean.
 * @return 0 on success (cleanup completed), -1 on error.
 */
int storage_on_detach(struct hs_device *dev, int *was_unclean);

/* Mount point tracking */

/**
 * Scan /proc/mounts and populate dev->mount_points with any filesystems
 * mounted from partitions belonging to this USB device.
 *
 * @return Number of mount points found.
 */
int storage_scan_mounts(struct hs_device *dev);

/* Sync timer management */

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

/* Utilities */

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

#endif /* HOTSWAPD_STORAGE_HANDLER_H */
