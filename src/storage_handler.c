/*
 * storage_handler.c — Storage-specific attach/detach handling.
 *
 * Implements the idle-sync model:
 *   - On attach: scan /proc/mounts, record mount points, start sync timer
 *   - Timer fires: check if device has been idle long enough → syncfs()
 *   - On detach: sync(), lazy unmount stale mount points, stop timer
 *
 * Sync model details:
 *   IDLE mode:    sync after idle_sync_delay_s of no write activity,
 *                 with fallback_sync_interval_s for continuously dirty
 *                 devices.
 *   PERIODIC:     sync every fallback_sync_interval_s unconditionally.
 *   MANUAL:       sync only on explicit eject or before detach.
 *   DISABLED:     no automatic syncing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/storage_handler.h"
#include "../include/log.h"

#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* Helpers */

/* Mount point scanning */

int storage_extract_disk_name(const char *blkdev, char *buf, size_t buflen)
{
    if (!blkdev || !buf || buflen == 0) {
        return -1;
    }

    const char *name = strrchr(blkdev, '/');
    name = name ? name + 1 : blkdev;

    size_t namelen = strlen(name);
    if (namelen == 0 || namelen >= buflen) {
        return -1;
    }
    memcpy(buf, name, namelen + 1);

    size_t digits_start = namelen;
    while (digits_start > 0 &&
           buf[digits_start - 1] >= '0' &&
           buf[digits_start - 1] <= '9') {
        digits_start--;
    }
    if (digits_start < namelen) {
        if (digits_start > 0 && buf[digits_start - 1] == 'p') {
            buf[digits_start - 1] = '\0';
        } else {
            buf[digits_start] = '\0';
        }
    }

    return 0;
}

int storage_resolve_usb_parent(const char *blkdev, char *buf, size_t buflen)
{
    /*
     * Strategy: resolve /sys/block/<dev> symlink → walk up the sysfs
     * tree until we find a "usb_device" devtype.
     *
     * For example:
     *   /dev/sdb1 → /sys/block/sdb/sdb1
     *   /sys/block/sdb → ../../devices/platform/usb/usb1/1-1/1-1:1.0/...
     *   Walk up to find the usb_device ancestor.
     */
    if (!blkdev || !buf || buflen == 0) {
        return -1;
    }

    char diskname[64];
    if (storage_extract_disk_name(blkdev, diskname, sizeof(diskname)) != 0) {
        return -1;
    }

    /* Read the /sys/block/<disk> symlink */
    char sysblock[PATH_MAX];
    snprintf(sysblock, sizeof(sysblock), "/sys/block/%s", diskname);

    char resolved[PATH_MAX];
    char *rp = realpath(sysblock, resolved);
    if (!rp) {
        return -1;
    }

    /* Walk up the resolved path looking for a directory that has
     * a "devtype" sysfs attribute containing "usb_device" */
    char search[PATH_MAX];
    size_t slen = strlen(resolved);
    if (slen >= sizeof(search)) {
        return -1;
    }
    memcpy(search, resolved, slen + 1);

    while (slen > 1) {
        char devtype_path[PATH_MAX];
        int n = snprintf(devtype_path, sizeof(devtype_path), "%s/devtype", search);
        if (n < 0 || (size_t)n >= sizeof(devtype_path)) {
            return -1;
        }

        FILE *f = fopen(devtype_path, "r");
        if (f) {
            char dt[32];
            if (fgets(dt, sizeof(dt), f)) {
                /* Strip newline */
                dt[strcspn(dt, "\n")] = '\0';
                if (strcmp(dt, "usb_device") == 0) {
                    fclose(f);
                    size_t cplen = strlen(search);
                    if (cplen >= buflen) {
                        cplen = buflen - 1;
                    }
                    memcpy(buf, search, cplen);
                    buf[cplen] = '\0';
                    return 0;
                }
            }
            fclose(f);
        }

        /* Go up one directory */
        char *slash = strrchr(search, '/');
        if (!slash || slash == search) {
            break;
        }
        *slash = '\0';
        slen = (size_t)(slash - search);
    }

    return -1;  /* Not USB-backed */
}

int storage_scan_mounts(struct hs_device *dev)
{
    if (!dev) {
        return -1;
    }

    dev->mount_count = 0;

    FILE *mtab = setmntent("/proc/mounts", "r");
    if (!mtab) {
        LOG_ERR("storage: failed to open /proc/mounts: %s", strerror(errno));
        return -1;
    }

    struct mntent *ent;
    while ((ent = getmntent(mtab)) != NULL) {
        if (dev->mount_count >= HOTSWAP_MAX_MOUNT_POINTS) {
            break;
        }

        /* Only interested in block devices */
        if (strncmp(ent->mnt_fsname, "/dev/", 5) != 0) {
            continue;
        }

        /* Check if this block device belongs to our USB device */
        char parent_syspath[PATH_MAX];
        if (storage_resolve_usb_parent(ent->mnt_fsname,
                                       parent_syspath,
                                       sizeof(parent_syspath)) != 0) {
            continue;  /* Not USB or can't resolve */
        }

        /* Compare the resolved parent syspath with our device's syspath */
        if (strcmp(parent_syspath, dev->syspath) != 0) {
            continue;  /* Belongs to a different USB device */
        }

        /* Match! Record this mount point */
        size_t mplen = strlen(ent->mnt_dir);
        if (mplen >= PATH_MAX) {
            mplen = PATH_MAX - 1;
        }
        memcpy(dev->mount_points[dev->mount_count], ent->mnt_dir, mplen);
        dev->mount_points[dev->mount_count][mplen] = '\0';
        dev->mount_count++;

        LOG_INFO("storage: tracked mount point %s for %s",
                 ent->mnt_dir, dev->devpath);
    }

    endmntent(mtab);

    LOG_VERBOSE("storage: found %d mount point(s) for %s",
                dev->mount_count, dev->devpath);
    return dev->mount_count;
}

/* Sync timer management */

int storage_start_sync_timer(struct hs_device *dev,
                             int idle_delay_s, int fallback_interval_s)
{
    if (!dev) {
        return -1;
    }

    if (dev->sync_policy == SYNC_MODE_DISABLED ||
        dev->sync_policy == SYNC_MODE_MANUAL) {
        LOG_VERBOSE("storage: sync timer not started for %s (mode=%d)",
                    dev->devpath, dev->sync_policy);
        return 0;
    }

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        LOG_ERR("storage: timerfd_create failed: %s", strerror(errno));
        return -1;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));

    if (dev->sync_policy == SYNC_MODE_PERIODIC) {
        /* Periodic: fire every fallback_interval_s */
        its.it_value.tv_sec = fallback_interval_s;
        its.it_interval.tv_sec = fallback_interval_s;
    } else {
        /* IDLE mode: initial timer is the idle delay.
         * We re-arm based on write activity in storage_handle_sync_timer. */
        its.it_value.tv_sec = idle_delay_s;
        its.it_interval.tv_sec = 0;  /* one-shot; re-armed on each expiry */
    }

    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        LOG_ERR("storage: timerfd_settime failed: %s", strerror(errno));
        close(tfd);
        return -1;
    }

    dev->sync_timer_fd = tfd;
    dev->dirty = 0;

    LOG_VERBOSE("storage: sync timer started for %s (mode=%d, idle=%ds, fallback=%ds)",
                dev->devpath, dev->sync_policy, idle_delay_s, fallback_interval_s);
    return tfd;
}

void storage_stop_sync_timer(struct hs_device *dev)
{
    if (!dev || dev->sync_timer_fd < 0) {
        return;
    }

    close(dev->sync_timer_fd);
    dev->sync_timer_fd = -1;

    LOG_VERBOSE("storage: sync timer stopped for %s", dev->devpath);
}

int storage_handle_sync_timer(struct hs_device *dev)
{
    if (!dev || dev->sync_timer_fd < 0) {
        return -1;
    }

    /* Read/drain the timerfd */
    uint64_t expirations;
    ssize_t n = read(dev->sync_timer_fd, &expirations, sizeof(expirations));
    if (n != sizeof(expirations)) {
        if (errno != EAGAIN) {
            LOG_ERR("storage: timerfd read failed: %s", strerror(errno));
        }
        return -1;
    }

    if (dev->mount_count == 0) {
        /* No mount points — nothing to sync */
        return 0;
    }

    if (dev->sync_policy == SYNC_MODE_IDLE) {
        /*
         * Idle-sync logic:
         * If the device has been idle (no writes) for idle_sync_delay,
         * perform syncfs.  If still dirty, re-arm with the fallback interval.
         */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        if (!dev->dirty) {
            /* Not dirty — nothing to sync, re-arm for idle check */
            struct itimerspec its;
            memset(&its, 0, sizeof(its));
            its.it_value.tv_sec = STORAGE_DEFAULT_IDLE_SYNC_DELAY_S;
            timerfd_settime(dev->sync_timer_fd, 0, &its, NULL);
            return 0;
        }

        /* Device is dirty — sync it */
        LOG_VERBOSE("storage: idle sync triggered for %s", dev->devpath);

    } else if (dev->sync_policy == SYNC_MODE_PERIODIC) {
        LOG_VERBOSE("storage: periodic sync for %s", dev->devpath);
    }

    /* Perform syncfs on each mounted filesystem */
    for (int i = 0; i < dev->mount_count; i++) {
        int fd = open(dev->mount_points[i], O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            LOG_WARN("storage: can't open %s for syncfs: %s",
                     dev->mount_points[i], strerror(errno));
            continue;
        }

        if (syncfs(fd) < 0) {
            LOG_WARN("storage: syncfs failed for %s: %s",
                     dev->mount_points[i], strerror(errno));
        } else {
            LOG_DEBUG("storage: syncfs completed for %s",
                      dev->mount_points[i]);
        }

        close(fd);
    }

    dev->dirty = 0;

    /* Re-arm timer for IDLE mode */
    if (dev->sync_policy == SYNC_MODE_IDLE) {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S;
        timerfd_settime(dev->sync_timer_fd, 0, &its, NULL);
    }

    return 0;
}

/* Attach / detach handlers */

int storage_on_attach(struct hs_device *dev)
{
    if (!dev) {
        return -1;
    }

    LOG_INFO("storage: processing attach for %s (%s %s)",
             dev->devpath, dev->vendor_name, dev->product_name);

    /*
     * Brief delay to let the kernel create block device nodes and partitions.
     * The kernel needs time to read the partition table and create /dev/sd*
     * entries.  500ms is generous for most devices.
     *
     * Note: this blocks the main event loop briefly.  For a more sophisticated
     * approach, we could defer this work via a timer — but 500ms is acceptable
     * for a daemon that processes events at human timescales.
     */
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 500000000L };
    nanosleep(&delay, NULL);

    /* Scan for mount points */
    storage_scan_mounts(dev);

    /* Start sync timer if we have mount points and sync is enabled */
    if (dev->mount_count > 0 &&
        dev->sync_policy != SYNC_MODE_DISABLED &&
        dev->sync_policy != SYNC_MODE_MANUAL) {
        storage_start_sync_timer(dev,
                                 dev->idle_sync_delay_s,
                                 dev->fallback_sync_interval_s);
    }

    return 0;
}

int storage_on_detach(struct hs_device *dev, int *was_unclean)
{
    if (!dev) {
        return -1;
    }

    if (was_unclean) {
        *was_unclean = (dev->state != DEV_STATE_DETACHING) ? 1 : 0;
    }

    int unclean = (dev->state != DEV_STATE_DETACHING) ? 1 : 0;

    LOG_INFO("storage: processing %s detach for %s (%s %s)",
             unclean ? "UNCLEAN" : "clean",
             dev->devpath, dev->vendor_name, dev->product_name);

    /* Stop sync timer first */
    storage_stop_sync_timer(dev);

    /* Sync all dirty buffers system-wide */
    sync();
    LOG_VERBOSE("storage: sync() completed");

    /* Lazy unmount all tracked mount points */
    for (int i = 0; i < dev->mount_count; i++) {
        if (dev->mount_points[i][0] == '\0') {
            continue;
        }

        LOG_INFO("storage: lazy unmounting %s", dev->mount_points[i]);

        /*
         * MNT_DETACH (lazy unmount): detaches the filesystem from the
         * mount tree immediately, making it invisible to new accesses.
         * Existing open file handles can still use it until they close.
         *
         * This is the safest option when the device is already gone —
         * a regular umount would fail with EBUSY or EIO.
         */
        if (umount2(dev->mount_points[i], MNT_DETACH) < 0) {
            if (errno == EINVAL || errno == ENOENT) {
                /* Already unmounted or doesn't exist — fine */
                LOG_VERBOSE("storage: %s already unmounted",
                            dev->mount_points[i]);
            } else {
                LOG_WARN("storage: umount2(%s, MNT_DETACH) failed: %s",
                         dev->mount_points[i], strerror(errno));
            }
        }

        if (unclean) {
            LOG_WARN("WARNING: %s was removed without unmounting %s. "
                     "Data written since last sync may be lost.",
                     dev->devpath, dev->mount_points[i]);
        }
    }

    dev->mount_count = 0;
    return 0;
}
