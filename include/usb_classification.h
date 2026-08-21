/*
 * usb_classification.h — USB class-to-category resolution.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef HOTSWAPD_USB_CLASSIFICATION_H
#define HOTSWAPD_USB_CLASSIFICATION_H

#include "hotswapd.h"

#include <stddef.h>

/**
 * Resolve a device category with this precedence:
 *
 * 1. An exact registry match, including an explicit "unknown" category.
 * 2. A recognized device-level USB class.
 * 3. Recognized interface-level USB classes.
 * 4. DEV_CAT_UNKNOWN.
 *
 * Composite devices use a fixed, safety-oriented interface priority:
 * storage, network, serial, video, audio, HID, then hub. This ensures a
 * device exposing storage plus another function receives storage cleanup.
 */
enum device_category usb_resolve_category(
    int has_registry_category, enum device_category registry_category,
    unsigned int device_class, const unsigned int *interface_classes,
    size_t interface_class_count);

#endif /* HOTSWAPD_USB_CLASSIFICATION_H */
