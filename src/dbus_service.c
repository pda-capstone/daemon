/*
 * dbus_service.c — D-Bus service for hotswapd.
 *
 * Owns "org.postmarketos.HotSwap" on the system bus.
 * Uses the low-level libdbus API (no GLib dependency).
 *
 * Signals:
 *   ModuleAttached(s:devpath, s:vid, s:pid, s:name, s:category, u:power, u:speed)
 *   ModuleDetached(s:devpath, s:name, b:was_unclean)
 *   ModuleReadyForRemoval(s:devpath, s:name)
 *   ModuleReleaseFailed(s:devpath, s:reason)
 *   PowerChanged(u:total_ma, u:count)
 *
 * Methods:
 *   ListModules()       → a(ssssu)
 *   GetModuleInfo(s)    → a{sv}
 *   GetTotalPowerDraw() → u
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/dbus_service.h"
#include "../include/device_state.h"
#include "../include/log.h"
#include "../include/power_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

/* ── Module state ────────────────────────────────────────────────────────── */

static DBusConnection *g_conn;

/* epoll fd that D-Bus watches are added to (set by dbus_service_setup_epoll) */
static int g_epoll_fd = -1;

/* ── D-Bus watch integration for epoll ───────────────────────────────────── */

/*
 * libdbus uses a "watch" abstraction for its file descriptors.
 * We integrate these into our epoll loop so we don't need a
 * separate dispatch thread.
 */

static dbus_bool_t watch_add(DBusWatch *watch, void *data) {
  (void)data;

  if (!dbus_watch_get_enabled(watch)) {
    return TRUE;
  }

  int fd = dbus_watch_get_unix_fd(watch);
  unsigned int flags = dbus_watch_get_flags(watch);

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.data.ptr = watch;

  if (flags & DBUS_WATCH_READABLE) {
    ev.events |= EPOLLIN;
  }
  if (flags & DBUS_WATCH_WRITABLE) {
    ev.events |= EPOLLOUT;
  }

  if (g_epoll_fd >= 0) {
    /* Try EPOLL_CTL_ADD first; if the fd already exists, use MOD */
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
      epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }
  }

  return TRUE;
}

static void watch_remove(DBusWatch *watch, void *data) {
  (void)data;

  int fd = dbus_watch_get_unix_fd(watch);
  if (g_epoll_fd >= 0) {
    epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
  }
}

static void watch_toggled(DBusWatch *watch, void *data) {
  if (dbus_watch_get_enabled(watch)) {
    watch_add(watch, data);
  } else {
    watch_remove(watch, data);
  }
}

/* ── Method handlers ─────────────────────────────────────────────────────── */

/**
 * ListModules() → a(ssssu)
 * Returns array of (devpath, name, category, state, power_ma)
 */
/*
 * File-scope callback for ListModules iteration.
 * Uses a static pointer (safe: single-threaded daemon).
 */
static DBusMessageIter *s_list_iter;

static int list_modules_cb(const struct hs_device *dev, void *userdata) {
  (void)userdata;
  DBusMessageIter struct_iter;

  dbus_message_iter_open_container(s_list_iter, DBUS_TYPE_STRUCT, NULL,
                                   &struct_iter);

  const char *devpath = dev->devpath;
  const char *name = dev->product_name[0] ? dev->product_name : "Unknown";
  const char *category = category_to_string(dev->category);
  const char *state = state_to_string(dev->state);
  dbus_uint32_t power = dev->max_power_ma;

  dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &devpath);
  dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &name);
  dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &category);
  dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &state);
  dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT32, &power);

  dbus_message_iter_close_container(s_list_iter, &struct_iter);
  return 0;
}

static DBusMessage *handle_list_modules(DBusMessage *msg) {
  DBusMessage *reply = dbus_message_new_method_return(msg);
  if (!reply) {
    return NULL;
  }

  DBusMessageIter iter, array_iter;
  dbus_message_iter_init_append(reply, &iter);
  dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ssssu)",
                                   &array_iter);

  s_list_iter = &array_iter;
  state_iterate(list_modules_cb, NULL);
  s_list_iter = NULL;

  dbus_message_iter_close_container(&iter, &array_iter);
  return reply;
}

/*
 * File-scope callback context for GetModuleInfo.
 */
static int get_module_info_cb(const struct hs_device *dev,
                              DBusMessageIter *dict_iter) {
/* Helper: append a string entry to the dict */
#define APPEND_STRING(key, val)                                                \
  do {                                                                         \
    DBusMessageIter entry, variant;                                            \
    const char *k = (key);                                                     \
    const char *v = (val);                                                     \
    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL,    \
                                     &entry);                                  \
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);              \
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s",           \
                                     &variant);                                \
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &v);            \
    dbus_message_iter_close_container(&entry, &variant);                       \
    dbus_message_iter_close_container(dict_iter, &entry);                      \
  } while (0)

/* Helper: append a uint32 entry to the dict */
#define APPEND_UINT32(key, val)                                                \
  do {                                                                         \
    DBusMessageIter entry, variant;                                            \
    const char *k = (key);                                                     \
    dbus_uint32_t v = (val);                                                   \
    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL,    \
                                     &entry);                                  \
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);              \
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u",           \
                                     &variant);                                \
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &v);            \
    dbus_message_iter_close_container(&entry, &variant);                       \
    dbus_message_iter_close_container(dict_iter, &entry);                      \
  } while (0)

#define APPEND_BOOL(key, val)                                                  \
  do {                                                                         \
    DBusMessageIter entry, variant;                                            \
    const char *k = (key);                                                     \
    dbus_bool_t v = (val) ? TRUE : FALSE;                                      \
    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, NULL,    \
                                     &entry);                                  \
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);              \
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b",           \
                                     &variant);                                \
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &v);           \
    dbus_message_iter_close_container(&entry, &variant);                       \
    dbus_message_iter_close_container(dict_iter, &entry);                      \
  } while (0)

  APPEND_STRING("devpath", dev->devpath);
  APPEND_STRING("syspath", dev->syspath);
  APPEND_STRING("vendor_id", dev->vendor_id);
  APPEND_STRING("product_id", dev->product_id);
  APPEND_STRING("vendor_name", dev->vendor_name);
  APPEND_STRING("product_name", dev->product_name);
  APPEND_STRING("serial", dev->serial);
  APPEND_STRING("category", category_to_string(dev->category));
  APPEND_STRING("state", state_to_string(dev->state));
  APPEND_UINT32("max_power_ma", dev->max_power_ma);
  APPEND_UINT32("speed_mbps", dev->speed_mbps);
  APPEND_BOOL("self_powered", dev->self_powered);
  APPEND_BOOL("has_pd", dev->has_pd);

  if (dev->has_pd) {
    APPEND_UINT32("pd_voltage_uv", dev->pd_voltage_uv);
    APPEND_UINT32("pd_current_ua", dev->pd_current_ua);
    APPEND_STRING("pd_power_role", dev->pd_power_role);
  }

  APPEND_UINT32("mount_count", (dbus_uint32_t)dev->mount_count);

#undef APPEND_STRING
#undef APPEND_UINT32
#undef APPEND_BOOL

  return 0;
}

static DBusMessage *handle_get_module_info(DBusMessage *msg) {
  const char *devpath = NULL;
  DBusError err;
  dbus_error_init(&err);

  if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &devpath,
                             DBUS_TYPE_INVALID)) {
    LOG_WARN("dbus: GetModuleInfo: bad args: %s", err.message);
    dbus_error_free(&err);
    return dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
                                  "Expected string argument (devpath)");
  }

  const struct hs_device *dev = state_find(devpath);
  if (!dev) {
    return dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "Device not found");
  }

  DBusMessage *reply = dbus_message_new_method_return(msg);
  if (!reply) {
    return NULL;
  }

  DBusMessageIter iter, dict_iter;
  dbus_message_iter_init_append(reply, &iter);
  dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);

  get_module_info_cb(dev, &dict_iter);

  dbus_message_iter_close_container(&iter, &dict_iter);
  return reply;
}

static DBusMessage *handle_get_total_power_draw(DBusMessage *msg) {
  DBusMessage *reply = dbus_message_new_method_return(msg);
  if (!reply) {
    return NULL;
  }

  dbus_uint32_t total = state_total_power_ma();

  DBusMessageIter iter;
  dbus_message_iter_init_append(reply, &iter);
  dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &total);

  return reply;
}

/* ── Message filter ──────────────────────────────────────────────────────── */

DBusHandlerResult dbus_handle_message(DBusConnection *conn, DBusMessage *msg,
                                      void *userdata) {
  (void)userdata;

  if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  const char *iface = dbus_message_get_interface(msg);
  const char *member = dbus_message_get_member(msg);

  if (!iface || strcmp(iface, HOTSWAP_DBUS_INTERFACE) != 0) {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  LOG_DEBUG("dbus: method call: %s", member);

  DBusMessage *reply = NULL;

  if (strcmp(member, "ListModules") == 0) {
    reply = handle_list_modules(msg);
  } else if (strcmp(member, "GetModuleInfo") == 0) {
    reply = handle_get_module_info(msg);
  } else if (strcmp(member, "GetTotalPowerDraw") == 0) {
    reply = handle_get_total_power_draw(msg);
  } else {
    reply = dbus_message_new_error_printf(msg, DBUS_ERROR_UNKNOWN_METHOD,
                                          "Unknown method: %s", member);
  }

  if (reply) {
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
  }

  return DBUS_HANDLER_RESULT_HANDLED;
}

/* ── Public API — Lifecycle ──────────────────────────────────────────────── */

int dbus_service_init(void) {
  DBusError err;
  dbus_error_init(&err);

  g_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
  if (!g_conn) {
    LOG_ERR("dbus: failed to connect to system bus: %s", err.message);
    dbus_error_free(&err);
    return -1;
  }

  /* Request our well-known bus name */
  int ret = dbus_bus_request_name(g_conn, HOTSWAP_DBUS_BUS_NAME,
                                  DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
  if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    LOG_ERR("dbus: failed to acquire bus name '%s': %s", HOTSWAP_DBUS_BUS_NAME,
            dbus_error_is_set(&err) ? err.message : "already owned");
    dbus_error_free(&err);
    dbus_connection_unref(g_conn);
    g_conn = NULL;
    return -1;
  }

  /* Register message filter for method calls */
  if (!dbus_connection_add_filter(g_conn, dbus_handle_message, NULL, NULL)) {
    LOG_ERR("dbus: failed to add message filter");
    dbus_connection_unref(g_conn);
    g_conn = NULL;
    return -1;
  }

  LOG_INFO("dbus: acquired bus name %s", HOTSWAP_DBUS_BUS_NAME);
  return 0;
}

void dbus_service_shutdown(void) {
  if (!g_conn) {
    return;
  }

  dbus_connection_remove_filter(g_conn, dbus_handle_message, NULL);

  DBusError err;
  dbus_error_init(&err);
  dbus_bus_release_name(g_conn, HOTSWAP_DBUS_BUS_NAME, &err);
  if (dbus_error_is_set(&err)) {
    LOG_WARN("dbus: error releasing bus name: %s", err.message);
    dbus_error_free(&err);
  }

  dbus_connection_unref(g_conn);
  g_conn = NULL;

  LOG_INFO("dbus: shutdown");
}

DBusConnection *dbus_service_get_connection(void) { return g_conn; }

/* ── Signal emission ─────────────────────────────────────────────────────── */

int dbus_emit_module_attached(const struct hs_device *dev) {
  if (!g_conn || !dev) {
    return -1;
  }

  DBusMessage *sig = dbus_message_new_signal(
      HOTSWAP_DBUS_OBJECT_PATH, HOTSWAP_DBUS_INTERFACE, "ModuleAttached");
  if (!sig) {
    return -1;
  }

  const char *devpath = dev->devpath;
  const char *vid = dev->vendor_id;
  const char *pid = dev->product_id;
  const char *name = dev->product_name[0] ? dev->product_name : "Unknown";
  const char *category = category_to_string(dev->category);
  dbus_uint32_t power = dev->max_power_ma;
  dbus_uint32_t speed = dev->speed_mbps;

  dbus_message_append_args(sig, DBUS_TYPE_STRING, &devpath, DBUS_TYPE_STRING,
                           &vid, DBUS_TYPE_STRING, &pid, DBUS_TYPE_STRING,
                           &name, DBUS_TYPE_STRING, &category, DBUS_TYPE_UINT32,
                           &power, DBUS_TYPE_UINT32, &speed,
                           DBUS_TYPE_INVALID);

  dbus_connection_send(g_conn, sig, NULL);
  dbus_connection_flush(g_conn);
  dbus_message_unref(sig);

  LOG_VERBOSE("dbus: emitted ModuleAttached for %s", devpath);
  return 0;
}

int dbus_emit_module_detached(const char *devpath, const char *name,
                              int was_unclean) {
  if (!g_conn) {
    return -1;
  }

  DBusMessage *sig = dbus_message_new_signal(
      HOTSWAP_DBUS_OBJECT_PATH, HOTSWAP_DBUS_INTERFACE, "ModuleDetached");
  if (!sig) {
    return -1;
  }

  const char *dp = devpath ? devpath : "";
  const char *n = name ? name : "Unknown";
  dbus_bool_t unclean = was_unclean ? TRUE : FALSE;

  dbus_message_append_args(sig, DBUS_TYPE_STRING, &dp, DBUS_TYPE_STRING, &n,
                           DBUS_TYPE_BOOLEAN, &unclean, DBUS_TYPE_INVALID);

  dbus_connection_send(g_conn, sig, NULL);
  dbus_connection_flush(g_conn);
  dbus_message_unref(sig);

  LOG_VERBOSE("dbus: emitted ModuleDetached for %s (unclean=%d)", dp,
              was_unclean);
  return 0;
}

int dbus_emit_power_changed(unsigned int total_draw_ma,
                            unsigned int device_count) {
  if (!g_conn) {
    return -1;
  }

  DBusMessage *sig = dbus_message_new_signal(
      HOTSWAP_DBUS_OBJECT_PATH, HOTSWAP_DBUS_INTERFACE, "PowerChanged");
  if (!sig) {
    return -1;
  }

  dbus_uint32_t total = total_draw_ma;
  dbus_uint32_t count = device_count;

  dbus_message_append_args(sig, DBUS_TYPE_UINT32, &total, DBUS_TYPE_UINT32,
                           &count, DBUS_TYPE_INVALID);

  dbus_connection_send(g_conn, sig, NULL);
  dbus_connection_flush(g_conn);
  dbus_message_unref(sig);

  LOG_DEBUG("dbus: emitted PowerChanged: %u mA, %u devices", total_draw_ma,
            device_count);
  return 0;
}

int dbus_emit_module_ready(const struct hs_device *dev) {
  if (!g_conn || !dev) {
    return -1;
  }

  DBusMessage *sig = dbus_message_new_signal(
      HOTSWAP_DBUS_OBJECT_PATH, HOTSWAP_DBUS_INTERFACE,
      "ModuleReadyForRemoval");
  if (!sig) {
    return -1;
  }
  const char *devpath = dev->devpath;
  const char *name = dev->product_name[0] ? dev->product_name : "Unknown";
  dbus_message_append_args(sig, DBUS_TYPE_STRING, &devpath, DBUS_TYPE_STRING,
                           &name, DBUS_TYPE_INVALID);
  dbus_connection_send(g_conn, sig, NULL);
  dbus_connection_flush(g_conn);
  dbus_message_unref(sig);
  LOG_INFO("dbus: emitted ModuleReadyForRemoval for %s", devpath);
  return 0;
}

int dbus_emit_release_failed(const char *devpath, const char *reason) {
  if (!g_conn) {
    return -1;
  }

  DBusMessage *sig = dbus_message_new_signal(
      HOTSWAP_DBUS_OBJECT_PATH, HOTSWAP_DBUS_INTERFACE,
      "ModuleReleaseFailed");
  if (!sig) {
    return -1;
  }
  const char *path = devpath ? devpath : "";
  const char *message = reason ? reason : "release preparation failed";
  dbus_message_append_args(sig, DBUS_TYPE_STRING, &path, DBUS_TYPE_STRING,
                           &message, DBUS_TYPE_INVALID);
  dbus_connection_send(g_conn, sig, NULL);
  dbus_connection_flush(g_conn);
  dbus_message_unref(sig);
  LOG_WARN("dbus: emitted ModuleReleaseFailed for %s: %s", path, message);
  return 0;
}

/* ── epoll integration ───────────────────────────────────────────────────── */

int dbus_service_setup_epoll(int epoll_fd) {
  if (!g_conn) {
    return -1;
  }

  g_epoll_fd = epoll_fd;

  if (!dbus_connection_set_watch_functions(g_conn, watch_add, watch_remove,
                                           watch_toggled, NULL, NULL)) {
    LOG_ERR("dbus: failed to set watch functions");
    return -1;
  }

  LOG_VERBOSE("dbus: watch functions registered with epoll fd %d", epoll_fd);
  return 0;
}

void dbus_service_dispatch(void) {
  if (!g_conn) {
    return;
  }

  /* Process all pending dispatches */
  while (dbus_connection_dispatch(g_conn) == DBUS_DISPATCH_DATA_REMAINS) {
    /* keep dispatching */
  }
}
