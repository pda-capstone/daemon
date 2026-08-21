/*
 * usb_classification.c — USB class-to-category resolution.
 *
 * SPDX-FileCopyrightText: 2026 Alexander Olivier
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../include/usb_classification.h"

static enum device_category category_from_usb_class(unsigned int usb_class) {
  switch (usb_class) {
  case 0x01:
    return DEV_CAT_AUDIO;
  case 0x02:
  case 0x0a:
    return DEV_CAT_SERIAL;
  case 0x03:
    return DEV_CAT_HID;
  case 0x08:
    return DEV_CAT_STORAGE;
  case 0x09:
    return DEV_CAT_HUB;
  case 0x0e:
    return DEV_CAT_VIDEO;
  case 0xe0:
    return DEV_CAT_NETWORK;
  default:
    return DEV_CAT_UNKNOWN;
  }
}

static int category_priority(enum device_category category) {
  switch (category) {
  case DEV_CAT_STORAGE:
    return 7;
  case DEV_CAT_NETWORK:
    return 6;
  case DEV_CAT_SERIAL:
    return 5;
  case DEV_CAT_VIDEO:
    return 4;
  case DEV_CAT_AUDIO:
    return 3;
  case DEV_CAT_HID:
    return 2;
  case DEV_CAT_HUB:
    return 1;
  case DEV_CAT_UNKNOWN:
  case DEV_CAT_COUNT:
    return 0;
  }
  return 0;
}

enum device_category usb_resolve_category(
    int has_registry_category, enum device_category registry_category,
    unsigned int device_class, const unsigned int *interface_classes,
    size_t interface_class_count) {
  if (has_registry_category) {
    return registry_category;
  }

  enum device_category category = category_from_usb_class(device_class);
  if (category != DEV_CAT_UNKNOWN) {
    return category;
  }

  enum device_category best = DEV_CAT_UNKNOWN;
  int best_priority = 0;
  for (size_t i = 0; interface_classes && i < interface_class_count; i++) {
    enum device_category candidate =
        category_from_usb_class(interface_classes[i]);
    int priority = category_priority(candidate);
    if (priority > best_priority) {
      best = candidate;
      best_priority = priority;
    }
  }

  return best;
}
