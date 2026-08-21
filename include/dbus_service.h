/*
 * dbus_service.h — D-Bus service interface for hotswapd.
 *
 * Owns "org.postmarketos.HotSwap" on the system bus.
 * Emits signals on device events and exposes query methods.
 *
 * Uses the low-level libdbus API directly (no GLib dependency).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef HOTSWAPD_DBUS_SERVICE_H
#define HOTSWAPD_DBUS_SERVICE_H

#include "hotswapd.h"
#include <dbus/dbus.h>

struct module_registry;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * Initialize the D-Bus service.  Connects to the system bus and
 * requests the well-known bus name.
 *
 * @return 0 on success, -1 on error.
 */
int dbus_service_init(struct module_registry *registry);

/**
 * Shut down the D-Bus service.  Releases the bus name and closes
 * the connection.
 */
void dbus_service_shutdown(void);

/**
 * Get the D-Bus connection.  Used to integrate D-Bus dispatch into
 * the main epoll loop via dbus_watch.
 *
 * @return The DBusConnection pointer, or NULL if not initialized.
 */
DBusConnection *dbus_service_get_connection(void);

/* ── Signal emission ─────────────────────────────────────────────────────── */

/**
 * Emit a ModuleAttached signal.
 *
 * Signal signature: (sssssuu)
 *   devpath, vendor_id, product_id, name, category, max_power_ma, speed
 */
int dbus_emit_module_attached(const struct hs_device *dev);

/**
 * Emit a ModuleDetached signal.
 *
 * Signal signature: (ssb)
 *   devpath, name, was_unclean
 */
int dbus_emit_module_detached(const char *devpath, const char *name,
                              int was_unclean);

/**
 * Emit a PowerChanged signal.
 *
 * Signal signature: (uu)
 *   total_draw_ma, device_count
 */
int dbus_emit_power_changed(unsigned int total_draw_ma,
                            unsigned int device_count);

/** Emit ModuleReadyForRemoval(ss): devpath, name. */
int dbus_emit_module_ready(const struct hs_device *dev);

/** Emit ModuleReleaseFailed(ss): devpath (or selector), reason. */
int dbus_emit_release_failed(const char *devpath, const char *reason);

/* ── Method dispatch ─────────────────────────────────────────────────────── */

/**
 * Process incoming D-Bus messages.  Call this when D-Bus indicates
 * there are messages to dispatch (from the epoll wakeup).
 *
 * Handles:
 *   ListModules()       → a(ssssu)
 *   GetModuleInfo(s)    → a{sv}
 *   GetTotalPowerDraw() → u
 *   ListRegistry()      → a(sssss)
 *   RegisterModule(sbsss) → sssssb (root callers only)
 *
 * @return DBUS_HANDLER_RESULT_HANDLED or DBUS_HANDLER_RESULT_NOT_YET_HANDLED.
 */
DBusHandlerResult dbus_handle_message(DBusConnection *conn, DBusMessage *msg,
                                      void *userdata);

/* ── epoll integration helpers ───────────────────────────────────────────── */

/**
 * Set up D-Bus watch functions so that D-Bus fds are managed by
 * the caller's epoll instance.
 *
 * @param epoll_fd  The epoll file descriptor to add D-Bus watches to.
 * @return 0 on success, -1 on error.
 */
int dbus_service_setup_epoll(int epoll_fd);

/**
 * Dispatch pending D-Bus work.  Call this after epoll indicates
 * a D-Bus watch fd is ready.
 */
void dbus_service_dispatch(void);

#endif /* HOTSWAPD_DBUS_SERVICE_H */
