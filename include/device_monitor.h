/*
 * device_monitor.h — libudev device monitor interface.
 *
 * Handles initial enumeration of existing USB devices and ongoing
 * monitoring of attach/detach events via the kernel uevent interface.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef HOTSWAPD_DEVICE_MONITOR_H
#define HOTSWAPD_DEVICE_MONITOR_H

#include "hotswapd.h"
#include "module_registry.h"

/* Forward declaration — full struct is opaque */
struct device_monitor;

/**
 * Callback invoked when a USB device is attached.
 *
 * @param dev  Fully populated device record.  The callback takes ownership;
 *             it must eventually free() the device (typically by adding it
 *             to the device state list, which frees on removal).
 */
typedef void (*monitor_attach_cb)(struct hs_device *dev);

/**
 * Callback invoked when a USB device is detached.
 *
 * @param devpath     The DEVPATH of the removed device.
 * @param was_unclean Non-zero if the device was yanked without a prior
 *                    clean-detach request.
 */
typedef void (*monitor_detach_cb)(const char *devpath, int was_unclean);

/* Lifecycle */

/**
 * Create and initialize the device monitor.
 *
 * @param reg        Module registry for looking up device metadata.
 * @param on_attach  Callback for attach events.
 * @param on_detach  Callback for detach events.
 * @return Monitor handle, or NULL on error.
 */
struct device_monitor *monitor_create(const struct module_registry *reg,
                                      monitor_attach_cb on_attach,
                                      monitor_detach_cb on_detach);

/**
 * Perform initial enumeration of all existing USB devices.
 * Calls on_attach for each device found.
 *
 * @return Number of devices enumerated, or -1 on error.
 */
int monitor_enumerate(struct device_monitor *mon);

/**
 * Get the file descriptor for the udev monitor.
 * Add this to your epoll set; when readable, call monitor_process_event().
 */
int monitor_get_fd(const struct device_monitor *mon);

/**
 * Process a pending udev event.  Reads one event from the monitor fd
 * and dispatches to the appropriate callback.
 *
 * @return 0 on success, -1 on error.
 */
int monitor_process_event(struct device_monitor *mon);

/**
 * Update the registry reference (after a live reload).
 */
void monitor_set_registry(struct device_monitor *mon,
                          const struct module_registry *reg);

/**
 * Destroy the monitor and free resources.
 */
void monitor_destroy(struct device_monitor *mon);

#endif /* HOTSWAPD_DEVICE_MONITOR_H */
