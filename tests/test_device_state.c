/* SPDX-License-Identifier: GPL-3.0-only */

#include "../include/device_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", msg);                                      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static struct hs_device *make_device(const char *devpath, unsigned int power) {
  struct hs_device *dev = calloc(1, sizeof(*dev));
  if (!dev) {
    return NULL;
  }

  snprintf(dev->devpath, sizeof(dev->devpath), "%s", devpath);
  snprintf(dev->product_name, sizeof(dev->product_name), "%s", "Test Device");
  dev->max_power_ma = power;
  dev->sync_timer_fd = -1;
  dev->state = DEV_STATE_ATTACHED;
  return dev;
}

int main(void) {
  struct hs_device *dev1 = make_device("/devices/usb1/1-1", 500);
  struct hs_device *dev2 = make_device("/devices/usb1/1-2", 250);
  struct hs_device *dup = make_device("/devices/usb1/1-1", 900);

  CHECK(dev1 != NULL && dev2 != NULL && dup != NULL, "device allocation");
  if (!dev1 || !dev2 || !dup) {
    free(dev1);
    free(dev2);
    free(dup);
    return EXIT_FAILURE;
  }

  CHECK(state_add(dev1) == 0, "state_add first device");
  CHECK(state_add(dev2) == 0, "state_add second device");
  CHECK(state_count() == 2, "state_count after add");
  CHECK(state_find("/devices/usb1/1-1") == dev1, "state_find existing");
  CHECK(state_total_power_ma() == 750, "state_total_power_ma sums bus power");

  CHECK(state_add(dup) != 0, "duplicate add rejected");
  free(dup);

  {
    struct hs_device *removed = state_remove("/devices/usb1/1-1");
    CHECK(removed == dev1, "state_remove returns removed device");
    free(removed);
  }

  CHECK(state_find("/devices/usb1/1-1") == NULL, "removed device not found");
  CHECK(state_remove("/devices/usb1/unknown") == NULL,
        "unknown device removal returns NULL");

  state_free_all();
  CHECK(state_count() == 0, "state_count reset after free");

  if (failures != 0) {
    return EXIT_FAILURE;
  }

  printf("test_device_state: ok\n");
  return EXIT_SUCCESS;
}
