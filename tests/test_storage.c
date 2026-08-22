/* SPDX-FileCopyrightText: 2026 Alexander Olivier */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../include/storage_handler.h"
#include "../include/storage_handler_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static int fake_mounted;
static int fake_mount_failure;
static int fake_mount_calls;
static int fake_unmount_calls;
static int fake_unmount_failure;
static int fake_sync_failure;
static int fake_sync_calls;
static int fake_unmount_flags;
static char fake_last_target[PATH_MAX];
static char fake_last_options[256];

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", msg);                                      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static void copy_value(char *destination, size_t size, const char *source) {
  snprintf(destination, size, "%s", source);
}

static int fake_scan_mounts(struct hs_device *dev) {
  dev->mount_count = 0;
  memset(dev->mount_points, 0, sizeof(dev->mount_points));
  memset(dev->mount_sources, 0, sizeof(dev->mount_sources));
  if (fake_mounted) {
    copy_value(dev->mount_sources[0], PATH_MAX, "/dev/fake1");
    copy_value(dev->mount_points[0], PATH_MAX, "/mnt/fake1");
    dev->mount_count = 1;
  }
  return dev->mount_count;
}

static int fake_discover(const struct hs_device *dev, char paths[][PATH_MAX],
                         size_t max_paths) {
  (void)dev;
  if (max_paths == 0) {
    return 0;
  }
  copy_value(paths[0], PATH_MAX, "/dev/fake1");
  return 1;
}

static int fake_mount(const char *source, const char *target,
                      const char *options) {
  fake_mount_calls++;
  copy_value(fake_last_target, sizeof(fake_last_target), target);
  copy_value(fake_last_options, sizeof(fake_last_options), options);
  CHECK(strcmp(source, "/dev/fake1") == 0, "mount uses discovered device");
  if (fake_mount_failure) {
    errno = EIO;
    return -1;
  }
  fake_mounted = 1;
  return 0;
}

static int fake_mount_matches(const char *source, const char *target) {
  return fake_mounted && strcmp(source, "/dev/fake1") == 0 &&
         strcmp(target, "/mnt/fake1") == 0;
}

static int fake_unmount(const char *target, int flags) {
  fake_unmount_calls++;
  fake_unmount_flags = flags;
  copy_value(fake_last_target, sizeof(fake_last_target), target);
  if (fake_unmount_failure) {
    errno = EBUSY;
    return -1;
  }
  fake_mounted = 0;
  return 0;
}

static int fake_sync(const char *target) {
  CHECK(strcmp(target, "/mnt/fake1") == 0,
        "sync operates on selected device mount");
  fake_sync_calls++;
  if (fake_sync_failure) {
    errno = EIO;
    return -1;
  }
  return 0;
}

static const struct storage_operations fake_operations = {
    .scan_mounts = fake_scan_mounts,
    .discover_block_devices = fake_discover,
    .mount_device = fake_mount,
    .mount_matches = fake_mount_matches,
    .unmount_path = fake_unmount,
    .sync_mount = fake_sync,
};

static void reset_fake(void) {
  fake_mounted = 0;
  fake_mount_failure = 0;
  fake_mount_calls = 0;
  fake_unmount_calls = 0;
  fake_unmount_failure = 0;
  fake_sync_failure = 0;
  fake_sync_calls = 0;
  fake_unmount_flags = -1;
  fake_last_target[0] = '\0';
  fake_last_options[0] = '\0';
  storage_set_operations(&fake_operations);
}

static struct hs_device make_storage_device(void) {
  struct hs_device dev;
  memset(&dev, 0, sizeof(dev));
  copy_value(dev.devpath, sizeof(dev.devpath), "/devices/usb1/1-1");
  copy_value(dev.syspath, sizeof(dev.syspath),
             "/sys/devices/platform/usb/usb1/1-1");
  dev.category = DEV_CAT_STORAGE;
  dev.state = DEV_STATE_ATTACHED;
  dev.attach_timer_fd = -1;
  dev.sync_timer_fd = -1;
  dev.sync_policy = SYNC_MODE_DISABLED;
  return dev;
}

static void check_disk_name(const char *blkdev, const char *expected) {
  char buf[64];
  int result = storage_extract_disk_name(blkdev, buf, sizeof(buf));
  CHECK(result == 0, blkdev);
  CHECK(result == 0 && strcmp(buf, expected) == 0, expected);
}

static int test_path(char *output, size_t output_size, const char *root,
                     const char *suffix) {
  int written = snprintf(output, output_size, "%s%s", root, suffix);
  return written >= 0 && (size_t)written < output_size ? 0 : -1;
}

static int make_test_directory(const char *root, const char *suffix) {
  char path[PATH_MAX];
  return test_path(path, sizeof(path), root, suffix) == 0 &&
                 mkdir(path, 0700) == 0
             ? 0
             : -1;
}

static int write_test_file(const char *root, const char *suffix,
                           const char *contents) {
  char path[PATH_MAX];
  if (test_path(path, sizeof(path), root, suffix) != 0) {
    return -1;
  }
  FILE *file = fopen(path, "w");
  if (!file) {
    return -1;
  }
  int write_result = fputs(contents, file);
  int close_result = fclose(file);
  return write_result >= 0 && close_result == 0 ? 0 : -1;
}

static void remove_test_path(const char *root, const char *suffix,
                             int directory) {
  char path[PATH_MAX];
  if (test_path(path, sizeof(path), root, suffix) == 0) {
    if (directory) {
      (void)rmdir(path);
    } else {
      (void)unlink(path);
    }
  }
}

static void test_usb_parent_uevent_resolution(void) {
  char root[] = "/tmp/hotswapd-storage-test-XXXXXX";
  char *temporary_root = mkdtemp(root);
  CHECK(temporary_root != NULL, "create temporary sysfs hierarchy");
  if (!temporary_root) {
    return;
  }

  static const char *directories[] = {
      "/usb4",
      "/usb4/4-1",
      "/usb4/4-1/4-1:1.0",
      "/usb4/4-1/4-1:1.0/host0",
      "/usb4/4-1/4-1:1.0/host0/target0:0:0",
      "/usb4/4-1/4-1:1.0/host0/target0:0:0/0:0:0:0",
      "/usb4/4-1/4-1:1.0/host0/target0:0:0/0:0:0:0/block",
      "/usb4/4-1/4-1:1.0/host0/target0:0:0/0:0:0:0/block/sda",
  };
  const size_t directory_count = sizeof(directories) / sizeof(directories[0]);
  int setup_ok = 1;
  for (size_t index = 0; index < directory_count; index++) {
    if (make_test_directory(root, directories[index]) != 0) {
      setup_ok = 0;
      break;
    }
  }

  setup_ok =
      setup_ok && write_test_file(root, "/usb4/uevent",
                                  "MAJOR=189\nDEVTYPE=usb_device\n") == 0;
  setup_ok =
      setup_ok && write_test_file(root, "/usb4/4-1/uevent",
                                  "BUSNUM=004\nDEVNUM=002\n"
                                  "DEVTYPE=usb_device\nDRIVER=usb\n") == 0;
  setup_ok = setup_ok && write_test_file(root, "/usb4/4-1/4-1:1.0/uevent",
                                         "DEVTYPE=usb_interface\n") == 0;
  CHECK(setup_ok, "populate temporary sysfs hierarchy");

  if (setup_ok) {
    char block_path[PATH_MAX];
    char expected_parent[PATH_MAX];
    char resolved_parent[PATH_MAX];
    CHECK(test_path(block_path, sizeof(block_path), root,
                    directories[directory_count - 1]) == 0,
          "construct fake block sysfs path");
    CHECK(test_path(expected_parent, sizeof(expected_parent), root,
                    "/usb4/4-1") == 0,
          "construct expected USB parent path");

    CHECK(storage_test_resolve_usb_parent_path(block_path, resolved_parent,
                                               sizeof(resolved_parent)) == 0,
          "resolve DEVTYPE from a sysfs uevent file");
    CHECK(strcmp(resolved_parent, expected_parent) == 0,
          "select the closest USB device ancestor");

    errno = 0;
    char too_small[2];
    CHECK(storage_test_resolve_usb_parent_path(block_path, too_small,
                                               sizeof(too_small)) == -1 &&
              errno == ENAMETOOLONG,
          "reject a truncated USB parent path");

    remove_test_path(root, "/usb4/4-1/uevent", 0);
    remove_test_path(root, "/usb4/uevent", 0);
    errno = 0;
    CHECK(storage_test_resolve_usb_parent_path(block_path, resolved_parent,
                                               sizeof(resolved_parent)) == -1 &&
              errno == ENODEV,
          "reject a hierarchy without a USB device ancestor");
  }

  remove_test_path(root, "/usb4/4-1/4-1:1.0/uevent", 0);
  remove_test_path(root, "/usb4/4-1/uevent", 0);
  remove_test_path(root, "/usb4/uevent", 0);
  for (size_t index = directory_count; index > 0; index--) {
    remove_test_path(root, directories[index - 1], 1);
  }
  (void)rmdir(root);
}

static void test_attach_without_action(void) {
  reset_fake();
  struct hs_device dev = make_storage_device();
  CHECK(storage_process_attach_once(&dev) == STORAGE_ATTACH_PENDING,
        "unconfigured attach waits for an external automount");
  CHECK(fake_mount_calls == 0, "unconfigured attach does not mount");
}

static void test_configured_mount(void) {
  reset_fake();
  struct hs_device dev = make_storage_device();
  dev.on_attach_action.has_action = 1;
  copy_value(dev.on_attach_action.action, sizeof(dev.on_attach_action.action),
             "mount");
  copy_value(dev.on_attach_action.options, sizeof(dev.on_attach_action.options),
             "flush,noatime");
  copy_value(dev.on_attach_action.mount_point,
             sizeof(dev.on_attach_action.mount_point), "/mnt/{device}");

  CHECK(storage_process_attach_once(&dev) == STORAGE_ATTACH_COMPLETE,
        "configured mount completes");
  CHECK(fake_mount_calls == 1, "configured mount executes once");
  CHECK(strcmp(fake_last_target, "/mnt/fake1") == 0,
        "mount-point device token expands");
  CHECK(strcmp(fake_last_options, "flush,noatime") == 0,
        "mount options are passed through");
  CHECK(dev.mount_count == 1, "successful mount updates stored mount state");
}

static void test_already_mounted(void) {
  reset_fake();
  fake_mounted = 1;
  struct hs_device dev = make_storage_device();
  dev.on_attach_action.has_action = 1;
  copy_value(dev.on_attach_action.action, sizeof(dev.on_attach_action.action),
             "mount");
  CHECK(storage_process_attach_once(&dev) == STORAGE_ATTACH_COMPLETE,
        "already-mounted storage completes");
  CHECK(fake_mount_calls == 0, "already-mounted storage is not mounted twice");
  CHECK(dev.mount_count == 1, "already-mounted state is recorded");
}

static void test_mount_failure(void) {
  reset_fake();
  fake_mount_failure = 1;
  struct hs_device dev = make_storage_device();
  dev.on_attach_action.has_action = 1;
  copy_value(dev.on_attach_action.action, sizeof(dev.on_attach_action.action),
             "mount");
  CHECK(storage_process_attach_once(&dev) == STORAGE_ATTACH_PENDING,
        "mount failure remains retryable");
  CHECK(fake_mount_calls == 1, "failed mount was attempted");
  CHECK(dev.mount_count == 0, "failed mount does not invent state");
}

static void seed_detach_mounts(struct hs_device *dev) {
  dev->mount_count = 2;
  copy_value(dev->mount_sources[0], PATH_MAX, "/dev/fake1");
  copy_value(dev->mount_points[0], PATH_MAX, "/mnt/fake1");
  copy_value(dev->mount_sources[1], PATH_MAX, "/dev/unrelated1");
  copy_value(dev->mount_points[1], PATH_MAX, "/mnt/unrelated");
}

static void test_clean_detach_scope(void) {
  reset_fake();
  fake_mounted = 1;
  struct hs_device dev = make_storage_device();
  dev.state = DEV_STATE_DETACHING;
  seed_detach_mounts(&dev);
  int was_unclean = 1;
  CHECK(storage_on_detach(&dev, &was_unclean) == 0, "clean detach completes");
  CHECK(was_unclean == 0, "clean detach is reported clean");
  CHECK(fake_sync_calls == 1, "clean detach syncs selected filesystem");
  CHECK(fake_unmount_calls == 1,
        "detach does not unmount unrelated filesystem");
  CHECK(fake_unmount_flags == 0, "clean detach uses orderly unmount");
  CHECK(strcmp(fake_last_target, "/mnt/fake1") == 0,
        "clean detach targets selected mount");
}

static void test_unclean_detach_scope(void) {
  reset_fake();
  fake_mounted = 1;
  struct hs_device dev = make_storage_device();
  seed_detach_mounts(&dev);
  int was_unclean = 0;
  CHECK(storage_on_detach(&dev, &was_unclean) == 0, "unclean detach completes");
  CHECK(was_unclean == 1, "physical removal is reported unclean");
  CHECK(fake_sync_calls == 0, "unclean detach does not sync a missing device");
  CHECK(fake_unmount_calls == 1,
        "unclean detach only cleans selected stale mount");
  CHECK(fake_unmount_flags == MNT_DETACH, "unclean detach uses lazy unmount");
}

static void test_gpio_release_preparation(void) {
  reset_fake();
  fake_mounted = 1;
  struct hs_device dev = make_storage_device();
  CHECK(storage_prepare_release(&dev) == 0,
        "safe release preparation succeeds");
  CHECK(fake_sync_calls == 1, "safe release flushes the filesystem");
  CHECK(fake_unmount_calls == 1, "safe release unmounts the filesystem");
  CHECK(fake_unmount_flags == 0, "safe release never uses lazy unmount");
  CHECK(dev.mount_count == 0, "safe release clears the mount cache");
}

static void test_gpio_release_refuses_failed_unmount(void) {
  reset_fake();
  fake_mounted = 1;
  fake_unmount_failure = 1;
  struct hs_device dev = make_storage_device();
  CHECK(storage_prepare_release(&dev) == -1,
        "failed unmount makes release preparation fail");
  CHECK(fake_sync_calls == 1, "failed release still attempted a flush");
  CHECK(dev.mount_count == 1, "failed release retains the mounted device");
}

static void test_gpio_release_refuses_failed_sync(void) {
  reset_fake();
  fake_mounted = 1;
  fake_sync_failure = 1;
  struct hs_device dev = make_storage_device();
  CHECK(storage_prepare_release(&dev) == -1,
        "failed sync makes release preparation fail");
  CHECK(fake_unmount_calls == 0,
        "release does not unmount after a failed explicit sync");
  CHECK(dev.mount_count == 1, "failed sync retains the mounted device");
}

int main(void) {
  check_disk_name("/dev/sdb", "sdb");
  check_disk_name("/dev/sdb1", "sdb");
  check_disk_name("/dev/mmcblk0p1", "mmcblk0");
  check_disk_name("/dev/nvme0n1p1", "nvme0n1");
  test_usb_parent_uevent_resolution();

  test_attach_without_action();
  test_configured_mount();
  test_already_mounted();
  test_mount_failure();
  test_clean_detach_scope();
  test_unclean_detach_scope();
  test_gpio_release_preparation();
  test_gpio_release_refuses_failed_unmount();
  test_gpio_release_refuses_failed_sync();
  storage_reset_operations();

  if (failures != 0) {
    return EXIT_FAILURE;
  }
  printf("test_storage: ok\n");
  return EXIT_SUCCESS;
}
