/*
 * power_info.c — USB power information reader.
 *
 * Reads legacy USB power attributes from sysfs files under the device's
 * syspath, and optionally reads USB-C Power Delivery data from
 * /sys/class/typec/ and /sys/class/power_supply/.
 *
 * SPDX-FileCopyrightText: 2026 Alexander Olivier
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../include/power_info.h"
#include "../include/log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/**
 * Read a sysfs attribute file into a buffer, stripping the trailing newline.
 * Returns the number of characters read (excluding NUL), or -1 on error.
 */
static int read_sysfs_attr(const char *dir, const char *attr, char *buf,
                           size_t buflen) {
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, attr);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    char *ret = fgets(buf, (int)buflen, f);
    fclose(f);

    if (!ret) {
        buf[0] = '\0';
        return -1;
    }

    /* Strip trailing newline */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    return (int)len;
}

/* ── Public parsers ──────────────────────────────────────────────────────── */

unsigned int power_parse_bMaxPower(const char *str) {
    if (!str) {
        return 0;
    }

    /* Format: "500mA" or "2mA" or "896mA" */
    unsigned int val = 0;
    while (*str && isdigit((unsigned char)*str)) {
        val = val * 10 + (unsigned int)(*str - '0');
        str++;
    }
    /* The "mA" suffix is expected but not required for parsing */
    return val;
}

int power_parse_self_powered(const char *str) {
    if (!str) {
        return 0;
    }

    /* Skip optional "0x" prefix */
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }

    unsigned long val = strtoul(str, NULL, 16);

    /*
     * USB spec: bmAttributes bit 6 = Self Powered.
     * Bit 7 is always 1 (reserved).
     * 0x40 = bit 6 set = self-powered.
     * e.g. 0xe0 = 1110_0000 → bit 6 = 1 → self-powered.
     *      0x80 = 1000_0000 → bit 6 = 0 → bus-powered.
     *      0xa0 = 1010_0000 → bit 5 = remote wakeup, bit 6 = 0 → bus-powered.
     */
    return (val & 0x40) ? 1 : 0;
}

/* ── Legacy USB power ────────────────────────────────────────────────────── */

int power_read_legacy(const char *syspath, struct hs_device *dev) {
    if (!syspath || !dev) {
        return -1;
    }

    char buf[64];

    /* bMaxPower — e.g. "500mA" */
    if (read_sysfs_attr(syspath, "bMaxPower", buf, sizeof(buf)) >= 0) {
        dev->max_power_ma = power_parse_bMaxPower(buf);
    } else {
        LOG_DEBUG("power: bMaxPower not readable for %s", syspath);
        dev->max_power_ma = 0;
    }

    /*
     * speed — sysfs reports Mbps as text and may include fractional values
     * such as "1.5" for low-speed USB. The public API exposes integer Mbps,
     * so round to the nearest whole Mbps instead of truncating silently.
     */
    if (read_sysfs_attr(syspath, "speed", buf, sizeof(buf)) >= 0) {
        char *end = NULL;
        errno = 0;
        double speed = strtod(buf, &end);
        if (end != buf && errno == 0 && speed >= 0.0) {
            dev->speed_mbps = (unsigned int)(speed + 0.5);
        } else {
            LOG_DEBUG("power: malformed speed '%s' for %s", buf, syspath);
            dev->speed_mbps = 0;
        }
    } else {
        LOG_DEBUG("power: speed not readable for %s", syspath);
        dev->speed_mbps = 0;
    }

    /* bmAttributes — hex byte, bit 6 = self-powered */
    if (read_sysfs_attr(syspath, "bmAttributes", buf, sizeof(buf)) >= 0) {
        dev->self_powered = power_parse_self_powered(buf);
    } else {
        LOG_DEBUG("power: bmAttributes not readable for %s", syspath);
        dev->self_powered = 0;
    }

    LOG_VERBOSE("power: legacy — %u mA, %u Mbps, %s", dev->max_power_ma,
                dev->speed_mbps,
                dev->self_powered ? "self-powered" : "bus-powered");
    return 0;
}

/* ── USB-C Power Delivery ────────────────────────────────────────────────── */

int power_read_pd(struct hs_device *dev) {
    if (!dev) {
        return -1;
    }

    dev->has_pd = 0;
    dev->pd_voltage_uv = 0;
    dev->pd_current_ua = 0;
    dev->pd_power_role[0] = '\0';

    LOG_DEBUG("power: no reliable per-device USB-C PD association for %s",
              dev->syspath);
    return 0;
}

/* ── Human-readable summary ──────────────────────────────────────────────── */

int power_format_string(const struct hs_device *dev, char *buf, size_t len) {
    if (!dev || !buf || len == 0) {
        return -1;
    }

    int written = snprintf(buf, len,
                           "Legacy USB Power:\n"
                           "  bMaxPower (declared max): %u mA\n"
                           "  Bus-powered: %s\n"
                           "  Speed: %u Mbps",
                           dev->max_power_ma,
                           dev->self_powered ? "no (self-powered)" : "yes",
                           dev->speed_mbps);

    if (written < 0 || (size_t)written >= len) {
        return written;
    }

    if (dev->has_pd) {
        int pd_written =
            snprintf(buf + written, len - (size_t)written,
                     "\nUSB-C PD:\n"
                     "  Power role: %s\n"
                     "  Negotiated voltage: %.1f V\n"
                     "  Negotiated current: %.1f A\n"
                     "  Negotiated power: %.1f W",
                     dev->pd_power_role[0] ? dev->pd_power_role : "unknown",
                     (double)dev->pd_voltage_uv / 1000000.0,
                     (double)dev->pd_current_ua / 1000000.0,
                     ((double)dev->pd_voltage_uv / 1000000.0) *
                         ((double)dev->pd_current_ua / 1000000.0));

        if (pd_written > 0) {
            written += pd_written;
        }
    }

    return written;
}
