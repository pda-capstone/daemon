/*
 * power_info.h — USB power information reader.
 *
 * Reads legacy USB power attributes (bMaxPower, speed, bmAttributes)
 * and optional USB-C Power Delivery data from sysfs.
 *
 * SPDX-FileCopyrightText: 2026 Alexander Olivier
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HOTSWAPD_POWER_INFO_H
#define HOTSWAPD_POWER_INFO_H

#include "hotswapd.h"

/**
 * Read legacy USB power attributes from sysfs.
 * Populates: max_power_ma, speed_mbps, self_powered.
 *
 * @param syspath  The sysfs path of the USB device
 *                 (e.g. "/sys/devices/platform/usb/usb1/1-1").
 * @param dev      Device record to populate.
 * @return 0 on success, -1 if sysfs attributes could not be read.
 */
int power_read_legacy(const char *syspath, struct hs_device *dev);

/**
 * Attempt to read USB-C Power Delivery information.
 * The implementation is conservative: it only reports PD data when it can be
 * tied to the actual attached device or port. If that relationship cannot be
 * established reliably, it leaves has_pd = 0 and returns success.
 *
 * @param dev  Device record to populate.
 * @return 0 on success (including "PD not available"), -1 on read error.
 */
int power_read_pd(struct hs_device *dev);

/**
 * Format a human-readable power summary for a device.
 *
 * @param dev  Device to summarize.
 * @param buf  Output buffer.
 * @param len  Size of output buffer.
 * @return Number of characters written (excluding NUL), or -1 on error.
 */
int power_format_string(const struct hs_device *dev, char *buf, size_t len);

/**
 * Parse a bMaxPower sysfs string like "500mA" into milliamps.
 *
 * @param str  The string from sysfs (e.g. "200mA", "500mA", "896mA").
 * @return The value in mA, or 0 if parsing fails.
 */
unsigned int power_parse_bMaxPower(const char *str);

/**
 * Parse bmAttributes and extract the self-powered bit.
 *
 * @param str  Hex string from sysfs (e.g. "a0", "0x80").
 * @return 1 if self-powered (bit 6 set), 0 if bus-powered.
 */
int power_parse_self_powered(const char *str);

#endif /* HOTSWAPD_POWER_INFO_H */
