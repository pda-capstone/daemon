/*
 * device_state.h — In-memory device state tracking.
 *
 * Maintains a singly-linked list of hs_device records.  The list is
 * adequate for the expected device count (1–10 concurrent USB peripherals).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef HOTSWAPD_DEVICE_STATE_H
#define HOTSWAPD_DEVICE_STATE_H

#include "hotswapd.h"

/* Lifecycle */

/**
 * Add a device to the tracked list.  The device struct is moved (caller
 * must not free it after this call).
 *
 * @return 0 on success, -1 if a device with the same devpath already exists.
 */
int state_add(struct hs_device *dev);

/**
 * Remove a device by its devpath.
 *
 * @return The removed device (caller must free it), or NULL if not found.
 */
struct hs_device *state_remove(const char *devpath);

/**
 * Free all tracked devices.
 */
void state_free_all(void);

/* Queries */

/**
 * Find a device by devpath.
 *
 * @return Pointer to the device, or NULL.  Pointer remains valid until
 *         the device is removed or state_free_all() is called.
 */
struct hs_device *state_find(const char *devpath);

/**
 * Return the number of currently tracked devices.
 */
int state_count(void);

/**
 * Return the total declared power draw (sum of max_power_ma) across
 * all tracked bus-powered devices.
 */
unsigned int state_total_power_ma(void);

/* Iteration */

/**
 * Callback type for state_iterate().
 *
 * @param dev       Pointer to the device (do not free).
 * @param userdata  Opaque pointer passed through from state_iterate().
 * @return 0 to continue iterating, nonzero to stop early.
 */
typedef int (*state_iterate_cb)(const struct hs_device *dev, void *userdata);

/**
 * Call `cb` for each tracked device.
 *
 * @return 0 if all devices were visited, or the nonzero return value
 *         from the callback that stopped iteration.
 */
int state_iterate(state_iterate_cb cb, void *userdata);

/* Helpers for storage sync timers */

/**
 * Collect all sync timer fds into the provided array.
 *
 * @param fds      Output array.
 * @param max_fds  Maximum entries to write.
 * @return         Number of fds written.
 */
int state_collect_sync_fds(int *fds, int max_fds);

/**
 * Find the device that owns a given sync timer fd.
 *
 * @return Pointer to the device, or NULL.
 */
struct hs_device *state_find_by_sync_fd(int fd);

#endif /* HOTSWAPD_DEVICE_STATE_H */
