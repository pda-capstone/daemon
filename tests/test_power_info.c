/* SPDX-License-Identifier: GPL-3.0-only */

#include "../include/power_info.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", msg);                                      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static int write_attr(const char *dir, const char *name, const char *value) {
  char path[PATH_MAX];
  FILE *f;

  snprintf(path, sizeof(path), "%s/%s", dir, name);
  f = fopen(path, "w");
  if (!f) {
    return -1;
  }
  fputs(value, f);
  fclose(f);
  return 0;
}

int main(void) {
  char tmpl[] = "/tmp/hotswapd-power-XXXXXX";
  char *tmpdir = mkdtemp(tmpl);
  struct hs_device dev;
  struct hs_device missing;
  struct hs_device malformed;

  CHECK(tmpdir != NULL, "mkdtemp for power test dir");
  if (!tmpdir) {
    return EXIT_FAILURE;
  }

  CHECK(write_attr(tmpdir, "bMaxPower", "500mA\n") == 0, "write bMaxPower");
  CHECK(write_attr(tmpdir, "speed", "1.5\n") == 0, "write fractional speed");
  CHECK(write_attr(tmpdir, "bmAttributes", "0x80\n") == 0,
        "write bmAttributes");

  memset(&dev, 0, sizeof(dev));
  CHECK(power_read_legacy(tmpdir, &dev) == 0, "power_read_legacy succeeds");
  CHECK(dev.max_power_ma == 500, "bMaxPower parsed");
  CHECK(dev.speed_mbps == 2, "fractional speed rounded");
  CHECK(dev.self_powered == 0, "bus-powered parsed");

  CHECK(write_attr(tmpdir, "speed", "480\n") == 0, "overwrite speed");
  memset(&dev, 0, sizeof(dev));
  CHECK(power_read_legacy(tmpdir, &dev) == 0, "power_read_legacy reread");
  CHECK(dev.speed_mbps == 480, "whole-number speed parsed");

  memset(&missing, 0, sizeof(missing));
  CHECK(power_read_legacy("/tmp", &missing) == 0,
        "missing sysfs attrs are tolerated");
  CHECK(missing.max_power_ma == 0 && missing.speed_mbps == 0 &&
            missing.self_powered == 0,
        "missing attributes zero fields");

  CHECK(write_attr(tmpdir, "speed", "bogus\n") == 0,
        "overwrite malformed speed");
  CHECK(write_attr(tmpdir, "bMaxPower", "oops\n") == 0,
        "overwrite malformed power");
  memset(&malformed, 0, sizeof(malformed));
  CHECK(power_read_legacy(tmpdir, &malformed) == 0,
        "malformed attrs do not hard fail");
  CHECK(malformed.max_power_ma == 0, "malformed power becomes zero");
  CHECK(malformed.speed_mbps == 0, "malformed speed becomes zero");

  {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/bMaxPower", tmpdir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/speed", tmpdir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/bmAttributes", tmpdir);
    unlink(path);
  }
  rmdir(tmpdir);

  if (failures != 0) {
    return EXIT_FAILURE;
  }

  printf("test_power_info: ok\n");
  return EXIT_SUCCESS;
}
