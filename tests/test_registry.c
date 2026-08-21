/* SPDX-FileCopyrightText: 2026 Alexander Olivier */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../include/module_registry.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", msg);                                      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static int write_file(const char *path, const char *contents) {
  FILE *f = fopen(path, "w");
  if (!f) {
    return -1;
  }
  fputs(contents, f);
  fclose(f);
  return 0;
}

static int check_module_name(const struct module_registry *reg,
                             const char *vendor_id, const char *product_id,
                             const char *expected_name) {
  const struct module_info *info = registry_lookup(reg, vendor_id, product_id);
  if (!info) {
    return 0;
  }
  return strcmp(info->name, expected_name) == 0;
}

int main(void) {
  char tmpl[] = "/tmp/hotswapd-registry-XXXXXX";
  char path[PATH_MAX];
  char tmp_path[PATH_MAX];
  struct module_registry *reg;
  const struct module_info *info;
  const struct module_sync_policy *policy;

  char *tmpdir = mkdtemp(tmpl);
  CHECK(tmpdir != NULL, "mkdtemp for registry test dir");
  if (!tmpdir) {
    return EXIT_FAILURE;
  }

  snprintf(path, sizeof(path), "%s/modules.json", tmpdir);
  CHECK(write_file(path, "{\n"
                         "  \"modules\": [\n"
                         "    {\n"
                         "      \"vendor_id\": \"0781\",\n"
                         "      \"product_id\": \"5567\",\n"
                         "      \"name\": \"SanDisk Cruzer Blade\",\n"
                         "      \"category\": \"storage\",\n"
                         "      \"on_attach\": {\n"
                         "        \"action\": \"mount\",\n"
                         "        \"options\": \"flush,noatime\",\n"
                         "        \"mount_point\": \"/mnt/{device}\"\n"
                         "      },\n"
                         "      \"sync_policy\": {\n"
                         "        \"mode\": \"periodic\",\n"
                         "        \"idle_sync_delay\": 7,\n"
                         "        \"fallback_sync_interval\": 42\n"
                         "      }\n"
                         "    }\n"
                         "  ],\n"
                         "  \"defaults\": {\n"
                         "    \"storage\": {\n"
                         "      \"sync_policy\": {\n"
                         "        \"mode\": \"idle\",\n"
                         "        \"idle_sync_delay\": 5,\n"
                         "        \"fallback_sync_interval\": 60\n"
                         "      }\n"
                         "    }\n"
                         "  }\n"
                         "}\n") == 0,
        "write valid registry");

  reg = registry_load(path);
  CHECK(reg != NULL, "registry_load valid file");
  if (reg) {
    info = registry_lookup(reg, "0781", "5567");
    CHECK(info != NULL, "exact module lookup");
    CHECK(info && strcmp(info->name, "SanDisk Cruzer Blade") == 0,
          "module name loaded");
    CHECK(info && info->has_sync_policy == 1, "module sync policy marked");
    CHECK(info && info->on_attach.has_action == 1,
          "module attach action marked");
    CHECK(info && strcmp(info->on_attach.action, "mount") == 0,
          "module mount action loaded");
    CHECK(info && strcmp(info->on_attach.options, "flush,noatime") == 0,
          "module mount options loaded");
    CHECK(info && strcmp(info->on_attach.mount_point, "/mnt/{device}") == 0,
          "module mount point loaded");
    CHECK(info && info->sync_policy.mode == SYNC_MODE_PERIODIC,
          "exact module sync mode");

    policy = registry_default_sync(reg, DEV_CAT_STORAGE);
    CHECK(policy != NULL, "category default lookup");
    CHECK(policy && policy->mode == SYNC_MODE_IDLE, "default sync mode");
    CHECK(policy && policy->idle_sync_delay_s == 5, "default idle delay");

    {
      struct hs_device dev;
      memset(&dev, 0, sizeof(dev));
      memcpy(dev.vendor_id, "abcd", 5);
      memcpy(dev.product_id, "1234", 5);
      memcpy(dev.product_name, "Connected Test Drive", 21);
      dev.category = DEV_CAT_STORAGE;

      CHECK(chmod(path, 0640) == 0, "set registry permissions");

      int was_replaced = -1;
      CHECK(registry_register_device(reg, &dev, NULL, "Test description", 0,
                                     &was_replaced) == 0,
            "register connected device");
      CHECK(was_replaced == 0, "new registration reports added");
      CHECK(registry_count(reg) == 2, "registration increases registry count");
      info = registry_lookup(reg, "abcd", "1234");
      CHECK(info != NULL, "registered module can be looked up");
      CHECK(info && strcmp(info->name, "Connected Test Drive") == 0,
            "registration uses connected product name");
      CHECK(info && strcmp(info->description, "Test description") == 0,
            "registration stores description override");
      CHECK(registry_get(reg, 1) == info, "registry indexed access");

      struct stat status;
      CHECK(stat(path, &status) == 0, "stat updated registry");
      CHECK((status.st_mode & 07777) == 0640,
            "atomic replacement preserves registry permissions");
      policy = registry_default_sync(reg, DEV_CAT_STORAGE);
      CHECK(policy && policy->idle_sync_delay_s == 5,
            "registration preserves defaults section");

      errno = 0;
      CHECK(registry_register_device(reg, &dev, NULL, NULL, 0, NULL) != 0 &&
                errno == EEXIST,
            "duplicate registration requires replace");

      CHECK(registry_register_device(reg, &dev, "Updated Test Drive", NULL, 1,
                                     &was_replaced) == 0,
            "replace existing registration");
      CHECK(was_replaced == 1, "replacement reports updated");
      info = registry_lookup(reg, "abcd", "1234");
      CHECK(info && strcmp(info->name, "Updated Test Drive") == 0,
            "replacement updates name");
      CHECK(info && strcmp(info->description, "Test description") == 0,
            "replacement without description preserves description");
      CHECK(registry_count(reg) == 2,
            "replacement does not duplicate registry entry");

      dev.category = DEV_CAT_UNKNOWN;
      errno = 0;
      CHECK(registry_register_device(reg, &dev, NULL, NULL, 1, NULL) != 0 &&
                errno == EINVAL,
            "unknown category is rejected");

      memset(&dev, 0, sizeof(dev));
      memcpy(dev.vendor_id, "0781", 5);
      memcpy(dev.product_id, "5567", 5);
      memcpy(dev.product_name, "Renamed Existing Drive", 23);
      dev.category = DEV_CAT_STORAGE;
      CHECK(registry_register_device(reg, &dev, NULL, NULL, 1, NULL) == 0,
            "replace definition containing policy fields");
      info = registry_lookup(reg, "0781", "5567");
      CHECK(info && info->on_attach.has_action == 1 &&
                strcmp(info->on_attach.action, "mount") == 0,
            "replacement preserves per-device action");
      CHECK(info && info->has_sync_policy == 1 &&
                info->sync_policy.mode == SYNC_MODE_PERIODIC,
            "replacement preserves per-device sync policy");
    }

    CHECK(write_file(path, "{\n"
                           "  \"modules\": [\n"
                           "    {\n"
                           "      \"vendor_id\": \"0781\",\n"
                           "      \"product_id\": \"5567\",\n"
                           "      \"name\": \"Updated Module\",\n"
                           "      \"category\": \"storage\"\n"
                           "    }\n"
                           "  ],\n"
                           "  \"defaults\": {\n"
                           "    \"storage\": {\n"
                           "      \"sync_policy\": {\n"
                           "        \"mode\": \"manual\"\n"
                           "      }\n"
                           "    }\n"
                           "  }\n"
                           "}\n") == 0,
          "rewrite registry in place");
    CHECK(registry_reload(reg) == 0, "registry_reload after rewrite");
    CHECK(check_module_name(reg, "0781", "5567", "Updated Module"),
          "registry_reload picks up rewritten file");

    {
      int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
      CHECK(n >= 0 && (size_t)n < sizeof(tmp_path),
            "tmp registry path fits buffer");
    }
    CHECK(write_file(tmp_path,
                     "{\n"
                     "  \"modules\": [\n"
                     "    {\n"
                     "      \"vendor_id\": \"1a86\",\n"
                     "      \"product_id\": \"7523\",\n"
                     "      \"name\": \"Atomic Replacement Module\",\n"
                     "      \"category\": \"serial\"\n"
                     "    }\n"
                     "  ],\n"
                     "  \"defaults\": {}\n"
                     "}\n") == 0,
          "write replacement registry");
    CHECK(rename(tmp_path, path) == 0, "atomic registry replacement rename");
    CHECK(registry_handle_inotify_event(reg) == 0,
          "registry_handle_inotify_event after rename");
    CHECK(check_module_name(reg, "1a86", "7523", "Atomic Replacement Module"),
          "atomic replacement reload applied");

    CHECK(write_file(path, "{ invalid json }\n") == 0,
          "write invalid registry for reload");
    CHECK(registry_reload(reg) != 0, "invalid reload rejected");
    CHECK(check_module_name(reg, "1a86", "7523", "Atomic Replacement Module"),
          "previous valid registry remains active after bad reload");

    registry_free(reg);
  }

  CHECK(write_file(path, "{ not valid json }\n") == 0, "write malformed json");
  reg = registry_load(path);
  CHECK(reg == NULL, "malformed registry rejected");

  unlink(path);
  rmdir(tmpdir);

  if (failures != 0) {
    return EXIT_FAILURE;
  }

  printf("test_registry: ok\n");
  return EXIT_SUCCESS;
}
