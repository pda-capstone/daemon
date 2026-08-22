/* SPDX-FileCopyrightText: 2026 Alexander Olivier */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../include/usb_classification.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", msg);                                      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

int main(void) {
  const unsigned int storage_interface[] = {0x08};
  const unsigned int hid_interface[] = {0x03};
  const unsigned int composite_interfaces[] = {0x03, 0x08, 0x01};
  const unsigned int unknown_interfaces[] = {0xff};

  CHECK(usb_resolve_category(0, DEV_CAT_UNKNOWN, 0x00, storage_interface, 1) ==
            DEV_CAT_STORAGE,
        "device class 00 with storage interface resolves to storage");
  CHECK(usb_resolve_category(1, DEV_CAT_HID, 0x08, storage_interface, 1) ==
            DEV_CAT_HID,
        "registry classification overrides inferred storage");
  CHECK(usb_resolve_category(0, DEV_CAT_UNKNOWN, 0x00, unknown_interfaces, 1) ==
            DEV_CAT_UNKNOWN,
        "unknown device remains unknown");
  CHECK(usb_resolve_category(0, DEV_CAT_UNKNOWN, 0x03, NULL, 0) == DEV_CAT_HID,
        "device-level HID remains HID");
  CHECK(usb_resolve_category(0, DEV_CAT_UNKNOWN, 0x00, hid_interface, 1) ==
            DEV_CAT_HID,
        "interface-level HID resolves to HID");
  CHECK(usb_resolve_category(0, DEV_CAT_UNKNOWN, 0x00, composite_interfaces,
                             3) == DEV_CAT_STORAGE,
        "composite devices use documented storage-first priority");
  CHECK(usb_resolve_category(0, DEV_CAT_UNKNOWN, 0x08, hid_interface, 1) ==
            DEV_CAT_STORAGE,
        "recognized device class precedes interface classes");

  if (failures != 0) {
    return EXIT_FAILURE;
  }
  printf("test_usb_classification: ok\n");
  return EXIT_SUCCESS;
}
