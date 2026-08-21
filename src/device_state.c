/*
 * device_state.c — In-memory device state tracking (linked list).
 *
 * SPDX-FileCopyrightText: 2026 Alexander Olivier
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../include/device_state.h"
#include "../include/log.h"

#include <stdlib.h>
#include <string.h>

/* ── Module state ────────────────────────────────────────────────────────── */

static struct hs_device *g_head;
static int g_count;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int state_add(struct hs_device *dev) {
    if (!dev || dev->devpath[0] == '\0') {
        LOG_ERR("state_add: NULL device or empty devpath");
        return -1;
    }

    /* Reject duplicates */
    if (state_find(dev->devpath)) {
        LOG_WARN("state_add: device already tracked: %s", dev->devpath);
        return -1;
    }

    dev->next = g_head;
    g_head = dev;
    g_count++;

    LOG_DEBUG("state_add: now tracking %d device(s)", g_count);
    return 0;
}

struct hs_device *state_remove(const char *devpath) {
    if (!devpath) {
        return NULL;
    }

    struct hs_device **pp = &g_head;
    while (*pp) {
        if (strcmp((*pp)->devpath, devpath) == 0) {
            struct hs_device *removed = *pp;
            *pp = removed->next;
            removed->next = NULL;
            g_count--;
            LOG_DEBUG("state_remove: removed %s, %d device(s) remain", devpath,
                      g_count);
            return removed;
        }
        pp = &(*pp)->next;
    }

    LOG_WARN("state_remove: device not found: %s", devpath);
    return NULL;
}

void state_free_all(void) {
    struct hs_device *dev = g_head;
    while (dev) {
        struct hs_device *next = dev->next;
        free(dev);
        dev = next;
    }
    g_head = NULL;
    g_count = 0;
    LOG_DEBUG("state_free_all: all devices freed");
}

/* ── Queries ─────────────────────────────────────────────────────────────── */

struct hs_device *state_find(const char *devpath) {
    if (!devpath) {
        return NULL;
    }

    struct hs_device *dev = g_head;
    while (dev) {
        if (strcmp(dev->devpath, devpath) == 0) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

int state_count(void) { return g_count; }

unsigned int state_total_power_ma(void) {
    unsigned int total = 0;
    struct hs_device *dev = g_head;
    while (dev) {
        if (!dev->self_powered) {
            total += dev->max_power_ma;
        }
        dev = dev->next;
    }
    return total;
}

/* ── Iteration ───────────────────────────────────────────────────────────── */

int state_iterate(state_iterate_cb cb, void *userdata) {
    if (!cb) {
        return 0;
    }

    struct hs_device *dev = g_head;
    while (dev) {
        int rc = cb(dev, userdata);
        if (rc != 0) {
            return rc;
        }
        dev = dev->next;
    }
    return 0;
}

/* ── Helpers for sync timers ─────────────────────────────────────────────── */

int state_collect_sync_fds(int *fds, int max_fds) {
    int n = 0;
    struct hs_device *dev = g_head;
    while (dev && n < max_fds) {
        if (dev->sync_timer_fd >= 0) {
            fds[n++] = dev->sync_timer_fd;
        }
        dev = dev->next;
    }
    return n;
}

struct hs_device *state_find_by_sync_fd(int fd) {
    struct hs_device *dev = g_head;
    while (dev) {
        if (dev->sync_timer_fd == fd) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

/* ── Utility implementations from hotswapd.h ─────────────────────────────── */

const char *category_to_string(enum device_category cat) {
    switch (cat) {
    case DEV_CAT_STORAGE:
        return "storage";
    case DEV_CAT_HID:
        return "hid";
    case DEV_CAT_SERIAL:
        return "serial";
    case DEV_CAT_NETWORK:
        return "network";
    case DEV_CAT_AUDIO:
        return "audio";
    case DEV_CAT_VIDEO:
        return "video";
    case DEV_CAT_HUB:
        return "hub";
    case DEV_CAT_UNKNOWN:
        return "unknown";
    default:
        return "unknown";
    }
}

enum device_category category_from_string(const char *str) {
    if (!str)
        return DEV_CAT_UNKNOWN;

    if (strcmp(str, "storage") == 0)
        return DEV_CAT_STORAGE;
    if (strcmp(str, "hid") == 0)
        return DEV_CAT_HID;
    if (strcmp(str, "serial") == 0)
        return DEV_CAT_SERIAL;
    if (strcmp(str, "network") == 0)
        return DEV_CAT_NETWORK;
    if (strcmp(str, "audio") == 0)
        return DEV_CAT_AUDIO;
    if (strcmp(str, "video") == 0)
        return DEV_CAT_VIDEO;
    if (strcmp(str, "hub") == 0)
        return DEV_CAT_HUB;

    return DEV_CAT_UNKNOWN;
}

const char *state_to_string(enum device_state st) {
    switch (st) {
    case DEV_STATE_ATTACHED:
        return "attached";
    case DEV_STATE_DETACHING:
        return "detaching";
    case DEV_STATE_DETACHED:
        return "detached";
    default:
        return "unknown";
    }
}
