/*
 * storage_handler.c — Storage-specific attach/detach handling.
 *
 * Implements the idle-sync model:
 *   - On attach: asynchronously discover block nodes, optionally mount, and
 *                record mount points before starting the sync timer
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── Platform operations and helpers ────────────────────────────────────── */

static int production_discover_block_devices(const struct hs_device *dev,
                                             char paths[][PATH_MAX],
                                             size_t max_paths);
static int production_mount_device(const char *source, const char *target,
                                   const char *options);
static int production_mount_matches(const char *source, const char *target);
static int production_unmount_path(const char *target, int flags);
static int production_sync_mount(const char *target);

static const struct storage_operations default_operations = {
    .scan_mounts = storage_scan_mounts,
    .discover_block_devices = production_discover_block_devices,
    .mount_device = production_mount_device,
    .mount_matches = production_mount_matches,
    .unmount_path = production_unmount_path,
    .sync_mount = production_sync_mount,
};

static const struct storage_operations *active_operations =
    &default_operations;

void storage_set_operations(const struct storage_operations *operations) {
  active_operations = operations ? operations : &default_operations;
}

void storage_reset_operations(void) { active_operations = &default_operations; }

static void copy_string(char *destination, size_t destination_size,
                        const char *source) {
  if (!destination || destination_size == 0) {
    return;
  }
  size_t length = source ? strlen(source) : 0;
  if (length >= destination_size) {
    length = destination_size - 1;
  }
  if (length > 0) {
    memcpy(destination, source, length);
  }
  destination[length] = '\0';
}

static int path_is_safe_absolute(const char *path) {
  if (!path || path[0] != '/') {
    return 0;
  }
  const char *component = path;
  while ((component = strstr(component, "..")) != NULL) {
    int left_boundary = component == path || component[-1] == '/';
    int right_boundary = component[2] == '\0' || component[2] == '/';
    if (left_boundary && right_boundary) {
      return 0;
    }
    component += 2;
  }
  return 1;
}

static int mkdir_parents(const char *path) {
  if (!path_is_safe_absolute(path)) {
    errno = EINVAL;
    return -1;
  }

  char buffer[PATH_MAX];
  copy_string(buffer, sizeof(buffer), path);
  for (char *cursor = buffer + 1; *cursor; cursor++) {
    if (*cursor != '/') {
      continue;
    }
    *cursor = '\0';
    if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
      return -1;
    }
    *cursor = '/';
  }
  if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
    return -1;
  }
  return 0;
}

static int expand_mount_point(const char *configured, const char *source,
                              char *output, size_t output_size) {
  const char *template = configured && configured[0]
                             ? configured
                             : "/run/media/hotswapd/{device}";
  const char *device = strrchr(source, '/');
  device = device ? device + 1 : source;
  const char *token = strstr(template, "{device}");
  int written;
  if (token) {
    written = snprintf(output, output_size, "%.*s%s%s",
                       (int)(token - template), template, device,
                       token + strlen("{device}"));
  } else {
    written = snprintf(output, output_size, "%s", template);
  }
  if (written < 0 || (size_t)written >= output_size ||
      !path_is_safe_absolute(output)) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

/* ── Mount point scanning ────────────────────────────────────────────────── */

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
    memset(dev->mount_points, 0, sizeof(dev->mount_points));
    memset(dev->mount_sources, 0, sizeof(dev->mount_sources));

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

        size_t source_len = strlen(ent->mnt_fsname);
        if (source_len >= PATH_MAX) {
            source_len = PATH_MAX - 1;
        }
        memcpy(dev->mount_sources[dev->mount_count], ent->mnt_fsname,
               source_len);
        dev->mount_sources[dev->mount_count][source_len] = '\0';
        dev->mount_count++;

        LOG_INFO("storage: tracked mount point %s for %s",
                 ent->mnt_dir, dev->devpath);
    }

    endmntent(mtab);

    LOG_VERBOSE("storage: found %d mount point(s) for %s",
                dev->mount_count, dev->devpath);
    return dev->mount_count;
}

static int path_compare(const void *left, const void *right) {
    return strcmp((const char *)left, (const char *)right);
}

static int production_discover_block_devices(const struct hs_device *dev,
                                             char paths[][PATH_MAX],
                                             size_t max_paths) {
    if (!dev || !paths || max_paths == 0 || dev->syspath[0] == '\0') {
        return -1;
    }

    DIR *directory = opendir("/sys/class/block");
    if (!directory) {
        return -1;
    }

    char partitions[HOTSWAP_MAX_MOUNT_POINTS][PATH_MAX];
    char disks[HOTSWAP_MAX_MOUNT_POINTS][PATH_MAX];
    size_t partition_count = 0;
    size_t disk_count = 0;
    struct dirent *entry;

    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char device_path[PATH_MAX];
        int n = snprintf(device_path, sizeof(device_path), "/dev/%s",
                         entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(device_path) ||
            access(device_path, F_OK) != 0) {
            continue;
        }

        char parent_syspath[PATH_MAX];
        if (storage_resolve_usb_parent(device_path, parent_syspath,
                                       sizeof(parent_syspath)) != 0 ||
            strcmp(parent_syspath, dev->syspath) != 0) {
            continue;
        }

        char partition_marker[PATH_MAX];
        n = snprintf(partition_marker, sizeof(partition_marker),
                     "/sys/class/block/%s/partition", entry->d_name);
        int is_partition =
            n >= 0 && (size_t)n < sizeof(partition_marker) &&
            access(partition_marker, F_OK) == 0;
        if (is_partition && partition_count < HOTSWAP_MAX_MOUNT_POINTS) {
            copy_string(partitions[partition_count++], PATH_MAX, device_path);
        } else if (!is_partition && disk_count < HOTSWAP_MAX_MOUNT_POINTS) {
            copy_string(disks[disk_count++], PATH_MAX, device_path);
        }
    }
    closedir(directory);

    char (*selected)[PATH_MAX] = partition_count > 0 ? partitions : disks;
    size_t selected_count = partition_count > 0 ? partition_count : disk_count;
    qsort(selected, selected_count, sizeof(selected[0]), path_compare);
    if (selected_count > max_paths) {
        selected_count = max_paths;
    }
    for (size_t i = 0; i < selected_count; i++) {
        copy_string(paths[i], PATH_MAX, selected[i]);
    }
    return (int)selected_count;
}

static const char *normalized_options(const char *options) {
    if (!options) {
        return "";
    }
    while (*options == ' ' || *options == '\t') {
        options++;
    }
    if (strncmp(options, "-o", 2) == 0) {
        options += 2;
        while (*options == ' ' || *options == '\t') {
            options++;
        }
    }
    return options;
}

static int production_mount_device(const char *source, const char *target,
                                   const char *options) {
    if (!source || strncmp(source, "/dev/", 5) != 0 ||
        !path_is_safe_absolute(target)) {
        errno = EINVAL;
        return -1;
    }
    if (mkdir_parents(target) != 0) {
        return -1;
    }

    const char *mount_options = normalized_options(options);
    for (const char *cursor = mount_options; *cursor; cursor++) {
        if (*cursor == ' ' || *cursor == '\t' || *cursor == '\n') {
            errno = EINVAL;
            return -1;
        }
    }

    pid_t child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        if (mount_options[0] != '\0') {
            execl("/bin/mount", "mount", "-o", mount_options, source,
                  target, (char *)NULL);
            execl("/usr/bin/mount", "mount", "-o", mount_options, source,
                  target, (char *)NULL);
        } else {
            execl("/bin/mount", "mount", source, target, (char *)NULL);
            execl("/usr/bin/mount", "mount", source, target, (char *)NULL);
        }
        _exit(127);
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int production_mount_matches(const char *source, const char *target) {
    FILE *mtab = setmntent("/proc/mounts", "r");
    if (!mtab) {
        return 0;
    }
    int matches = 0;
    struct mntent *entry;
    while ((entry = getmntent(mtab)) != NULL) {
        if (strcmp(entry->mnt_fsname, source) == 0 &&
            strcmp(entry->mnt_dir, target) == 0) {
            matches = 1;
            break;
        }
    }
    endmntent(mtab);
    return matches;
}

static int production_unmount_path(const char *target, int flags) {
    return umount2(target, flags);
}

static int production_sync_mount(const char *target) {
    int fd = open(target, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    int result = syncfs(fd);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return result;
}

/* ── Sync timer management ───────────────────────────────────────────────── */

int storage_start_sync_timer(struct hs_device *dev,
                             int idle_delay_s,
                             int fallback_interval_s)
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

    /* Perform syncfs on each mounted filesystem. */
    for (int i = 0; i < dev->mount_count; i++) {
        if (active_operations->sync_mount(dev->mount_points[i]) < 0) {
            LOG_WARN("storage: syncfs failed for %s: %s",
                     dev->mount_points[i], strerror(errno));
        } else {
            LOG_DEBUG("storage: syncfs completed for %s",
                      dev->mount_points[i]);
        }
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

/* ── Attach / detach handlers ────────────────────────────────────────────── */

int storage_process_attach_once(struct hs_device *dev) {
    if (!dev) {
        return STORAGE_ATTACH_ERROR;
    }

    int mounted = active_operations->scan_mounts(dev);
    if (mounted < 0) {
        return STORAGE_ATTACH_ERROR;
    }
    if (mounted > 0) {
        return STORAGE_ATTACH_COMPLETE;
    }

    if (!dev->on_attach_action.has_action ||
        strcmp(dev->on_attach_action.action, "mount") != 0) {
        return STORAGE_ATTACH_PENDING;
    }

    char devices[HOTSWAP_MAX_MOUNT_POINTS][PATH_MAX];
    int device_count = active_operations->discover_block_devices(
        dev, devices, HOTSWAP_MAX_MOUNT_POINTS);
    if (device_count < 0) {
        return STORAGE_ATTACH_ERROR;
    }
    if (device_count == 0) {
        return STORAGE_ATTACH_PENDING;
    }

    /* Discovery runs every 200 ms, but external mount attempts are limited
     * to once per second so an unformatted device does not spawn mount(8) in
     * a tight loop while a partition table or filesystem is being created. */
    unsigned int mount_attempt_stride =
        1000U / STORAGE_ATTACH_DISCOVERY_INTERVAL_MS;
    if (dev->attach_attempts > 0 && mount_attempt_stride > 0 &&
        dev->attach_attempts % mount_attempt_stride != 0) {
        return STORAGE_ATTACH_PENDING;
    }

    int fixed_mount_point =
        dev->on_attach_action.mount_point[0] != '\0' &&
        strstr(dev->on_attach_action.mount_point, "{device}") == NULL;
    for (int i = 0; i < device_count; i++) {
        char target[PATH_MAX];
        if (expand_mount_point(dev->on_attach_action.mount_point, devices[i],
                               target, sizeof(target)) != 0) {
            LOG_WARN("storage: invalid mount point for %s: %s", devices[i],
                     strerror(errno));
            return STORAGE_ATTACH_ERROR;
        }

        if (active_operations->mount_matches(devices[i], target)) {
            LOG_VERBOSE("storage: %s is already mounted at %s", devices[i],
                        target);
        } else if (active_operations->mount_device(
                       devices[i], target,
                       dev->on_attach_action.options) != 0) {
            LOG_DEBUG("storage: mount attempt for %s at %s failed: %s",
                      devices[i], target, strerror(errno));
            if (fixed_mount_point) {
                break;
            }
            continue;
        } else {
            LOG_INFO("storage: mounted %s at %s", devices[i], target);
        }

        mounted = active_operations->scan_mounts(dev);
        if (mounted > 0) {
            return STORAGE_ATTACH_COMPLETE;
        }
        if (mounted < 0) {
            return STORAGE_ATTACH_ERROR;
        }
        if (fixed_mount_point) {
            break;
        }
    }

    return STORAGE_ATTACH_PENDING;
}

int storage_finish_attach(struct hs_device *dev) {
    if (!dev) {
        return -1;
    }
    if (dev->attach_timer_fd >= 0) {
        close(dev->attach_timer_fd);
        dev->attach_timer_fd = -1;
    }
    if (dev->mount_count > 0 && dev->sync_timer_fd < 0 &&
        dev->sync_policy != SYNC_MODE_DISABLED &&
        dev->sync_policy != SYNC_MODE_MANUAL) {
        if (storage_start_sync_timer(dev, dev->idle_sync_delay_s,
                                     dev->fallback_sync_interval_s) < 0) {
            return -1;
        }
    }
    return 0;
}

void storage_cancel_attach(struct hs_device *dev) {
    if (!dev || dev->attach_timer_fd < 0) {
        return;
    }
    close(dev->attach_timer_fd);
    dev->attach_timer_fd = -1;
}

int storage_handle_attach_timer(struct hs_device *dev) {
    if (!dev || dev->attach_timer_fd < 0) {
        return STORAGE_ATTACH_ERROR;
    }

    uint64_t expirations;
    ssize_t count = read(dev->attach_timer_fd, &expirations,
                         sizeof(expirations));
    if (count != (ssize_t)sizeof(expirations) && errno != EAGAIN) {
        LOG_WARN("storage: attach discovery timer read failed: %s",
                 strerror(errno));
        return STORAGE_ATTACH_ERROR;
    }

    dev->attach_attempts++;
    int result = storage_process_attach_once(dev);
    if (result != STORAGE_ATTACH_PENDING) {
        return result;
    }
    if (dev->attach_attempts >= STORAGE_ATTACH_MAX_ATTEMPTS) {
        if (dev->on_attach_action.has_action &&
            strcmp(dev->on_attach_action.action, "mount") == 0) {
            LOG_WARN("storage: timed out after %d ms waiting to mount %s",
                     STORAGE_ATTACH_DISCOVERY_TIMEOUT_MS, dev->devpath);
        } else {
            LOG_VERBOSE("storage: no mount appeared for %s within %d ms",
                        dev->devpath, STORAGE_ATTACH_DISCOVERY_TIMEOUT_MS);
        }
        return STORAGE_ATTACH_COMPLETE;
    }
    return STORAGE_ATTACH_PENDING;
}

int storage_on_attach(struct hs_device *dev) {
    if (!dev) {
        return -1;
    }

    LOG_INFO("storage: processing attach for %s (%s %s)", dev->devpath,
             dev->vendor_name, dev->product_name);
    dev->attach_attempts = 0;

    int result = storage_process_attach_once(dev);
    if (result == STORAGE_ATTACH_COMPLETE) {
        return storage_finish_attach(dev);
    }
    if (result == STORAGE_ATTACH_ERROR) {
        LOG_WARN("storage: initial attach processing failed for %s",
                 dev->devpath);
    }

    dev->attach_timer_fd =
        timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (dev->attach_timer_fd < 0) {
        LOG_ERR("storage: attach timerfd_create failed: %s", strerror(errno));
        return -1;
    }

    struct itimerspec timer;
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_sec = STORAGE_ATTACH_DISCOVERY_INTERVAL_MS / 1000;
    timer.it_value.tv_nsec =
        (STORAGE_ATTACH_DISCOVERY_INTERVAL_MS % 1000) * 1000000L;
    timer.it_interval = timer.it_value;
    if (timerfd_settime(dev->attach_timer_fd, 0, &timer, NULL) != 0) {
        LOG_ERR("storage: attach timerfd_settime failed: %s", strerror(errno));
        storage_cancel_attach(dev);
        return -1;
    }
    return 0;
}

int storage_on_detach(struct hs_device *dev, int *was_unclean) {
    if (!dev) {
        return -1;
    }

    int unclean = dev->state != DEV_STATE_DETACHING;
    if (was_unclean) {
        *was_unclean = unclean;
    }

    LOG_INFO("storage: processing %s detach for %s (%s %s)",
             unclean ? "UNCLEAN" : "clean", dev->devpath, dev->vendor_name,
             dev->product_name);

    storage_cancel_attach(dev);
    storage_stop_sync_timer(dev);

    for (int i = 0; i < dev->mount_count; i++) {
        const char *source = dev->mount_sources[i];
        const char *target = dev->mount_points[i];
        if (source[0] == '\0' || target[0] == '\0') {
            continue;
        }
        if (!active_operations->mount_matches(source, target)) {
            LOG_WARN("storage: refusing to unmount changed or unrelated mount "
                     "%s (expected source %s)", target, source);
            continue;
        }

        int flags = unclean ? MNT_DETACH : 0;
        if (!unclean && active_operations->sync_mount(target) != 0) {
            LOG_WARN("storage: syncfs failed before unmounting %s: %s", target,
                     strerror(errno));
        }

        LOG_INFO("storage: %sunmounting %s", unclean ? "lazily " : "",
                 target);
        if (active_operations->unmount_path(target, flags) != 0) {
            if (errno == EINVAL || errno == ENOENT) {
                LOG_VERBOSE("storage: %s is already unmounted", target);
            } else {
                LOG_WARN("storage: unmount of %s failed: %s", target,
                         strerror(errno));
            }
        }

        if (unclean) {
            LOG_WARN("WARNING: %s was removed without unmounting %s. "
                     "Unsynced data may be lost.", dev->devpath, target);
        }
    }

    dev->mount_count = 0;
    memset(dev->mount_points, 0, sizeof(dev->mount_points));
    memset(dev->mount_sources, 0, sizeof(dev->mount_sources));
    return 0;
}

int storage_prepare_release(struct hs_device *dev) {
    if (!dev) {
        errno = EINVAL;
        return -1;
    }

    LOG_INFO("storage: preparing %s for physical removal", dev->devpath);
    storage_cancel_attach(dev);
    storage_stop_sync_timer(dev);

    /* Refresh the cache while sysfs and the block device are still present.
     * This catches mounts created after the initial discovery window. */
    if (active_operations->scan_mounts(dev) < 0) {
        LOG_WARN("storage: failed to refresh mounts for %s", dev->devpath);
        return -1;
    }

    int failed = 0;
    for (int i = 0; i < dev->mount_count; i++) {
        const char *source = dev->mount_sources[i];
        const char *target = dev->mount_points[i];
        if (source[0] == '\0' || target[0] == '\0') {
            continue;
        }
        if (!active_operations->mount_matches(source, target)) {
            LOG_WARN("storage: mount changed while preparing release: %s",
                     target);
            failed = 1;
            continue;
        }
        if (active_operations->sync_mount(target) != 0) {
            LOG_WARN("storage: syncfs failed while preparing %s: %s", target,
                     strerror(errno));
            failed = 1;
            continue;
        }
        if (active_operations->unmount_path(target, 0) != 0) {
            LOG_WARN("storage: orderly unmount of %s failed: %s", target,
                     strerror(errno));
            failed = 1;
            continue;
        }
        LOG_INFO("storage: safely unmounted %s", target);
    }

    if (failed) {
        /* Re-scan so a retry acts on only the mounts that remain. */
        active_operations->scan_mounts(dev);
        return -1;
    }

    dev->mount_count = 0;
    memset(dev->mount_points, 0, sizeof(dev->mount_points));
    memset(dev->mount_sources, 0, sizeof(dev->mount_sources));
    return 0;
}
