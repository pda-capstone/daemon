/*
 * module_registry.h — Module registry interface.
 *
 * Loads module definitions from a JSON file and provides lookup by
 * (vendor_id, product_id).  Watches the file via inotify for live
 * reload without daemon restart.
 *
 * The internal representation is a flat array of module_info structs.
 * This is designed so that additional sources (split files, directories,
 * network) can be added later by feeding more module_info entries into
 * the same array.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef HOTSWAPD_MODULE_REGISTRY_H
#define HOTSWAPD_MODULE_REGISTRY_H

#include "hotswapd.h"

#define STORAGE_DEFAULT_IDLE_SYNC_DELAY_S 5
#define STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S 60

/* ── Per-module sync policy (overrides global default) ───────────────────── */

struct module_sync_policy {
  enum sync_mode mode;
  int idle_sync_delay_s;        /* seconds of idle before sync (mode=idle) */
  int fallback_sync_interval_s; /* periodic fallback interval (mode=idle) */
};

/* ── Module definition ───────────────────────────────────────────────────── */

struct module_info {
  char vendor_id[HOTSWAP_MAX_ID];
  char product_id[HOTSWAP_MAX_ID];
  char name[HOTSWAP_MAX_NAME];
  char description[256];
  enum device_category category;
  int has_sync_policy;

  struct module_action on_attach;
  struct module_action on_detach;

  struct module_sync_policy sync_policy;
};

/* ── Registry handle (opaque to callers) ─────────────────────────────────── */

struct module_registry;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * Load the module registry from a JSON file.
 *
 * @param path  Path to the JSON registry file.
 * @return      Opaque registry handle, or NULL on error.
 */
struct module_registry *registry_load(const char *path);

/**
 * Reload the registry from disk.  Atomically swaps the internal data.
 * Existing pointers from registry_lookup() become invalid after this call.
 *
 * @return 0 on success, -1 on error (old registry is retained).
 */
int registry_reload(struct module_registry *reg);

/**
 * Free all registry resources.
 */
void registry_free(struct module_registry *reg);

/* ── Lookup ──────────────────────────────────────────────────────────────── */

/**
 * Look up a module by vendor/product ID.
 *
 * @return Pointer to the module_info, or NULL if not found.
 *         The pointer is valid until the next registry_reload() or
 *         registry_free().
 */
const struct module_info *registry_lookup(const struct module_registry *reg,
                                          const char *vendor_id,
                                          const char *product_id);

/**
 * Return a registry entry by zero-based index.
 *
 * The pointer remains valid until the next reload, registration, or free.
 */
const struct module_info *registry_get(const struct module_registry *reg,
                                       int index);

/**
 * Add the identity of a currently connected device to the JSON registry.
 * Existing per-device actions, descriptions, and sync policy are preserved
 * when replace is non-zero.
 *
 * The update is locked, written to a same-directory temporary file, synced,
 * atomically renamed, and reloaded into the active registry.
 *
 * @param reg           Active registry.
 * @param dev           Currently tracked device to register.
 * @param name          Optional display-name override; NULL/empty uses device.
 * @param description   Optional description; NULL/empty preserves an existing
 *                      description or supplies a default for a new entry.
 * @param replace       Permit updating an existing VID/PID entry.
 * @param was_replaced  Optional output set to 1 for replacement, 0 for add.
 * @return 0 on success, -1 on failure with errno set. EEXIST means that an
 *         entry already exists and replace was not requested.
 */
int registry_register_device(struct module_registry *reg,
                             const struct hs_device *dev, const char *name,
                             const char *description, int replace,
                             int *was_replaced);

/**
 * Get the default action for a category (from the "defaults" section).
 * Returns NULL if no default is defined for this category.
 */
const struct module_action *
registry_default_attach(const struct module_registry *reg,
                        enum device_category cat);

const struct module_action *
registry_default_detach(const struct module_registry *reg,
                        enum device_category cat);

/**
 * Get the default sync policy for a category.
 * Returns NULL if no default is defined.
 */
const struct module_sync_policy *
registry_default_sync(const struct module_registry *reg,
                      enum device_category cat);

/* ── inotify integration ─────────────────────────────────────────────────── */

/**
 * Get the inotify file descriptor for the registry watch.
 * Add this fd to your epoll set.  When readable, call
 * registry_handle_inotify_event().
 *
 * @return fd >= 0, or -1 if inotify is not set up.
 */
int registry_get_inotify_fd(const struct module_registry *reg);

/**
 * Process a pending inotify event.  If the registry file changed,
 * performs an atomic reload.
 *
 * @return 0 on success (or no-op), -1 on reload error.
 */
int registry_handle_inotify_event(struct module_registry *reg);

/* ── Introspection (for hsctl / debugging) ───────────────────────────────── */

/**
 * Get the number of module definitions loaded.
 */
int registry_count(const struct module_registry *reg);

/**
 * Get the file path the registry was loaded from.
 */
const char *registry_path(const struct module_registry *reg);

#endif /* HOTSWAPD_MODULE_REGISTRY_H */
