/*
 * module_registry.c — JSON module registry with inotify live reload.
 *
 * Parses /etc/hotswapd/modules.json into an array of module_info structs.
 * Provides lookup by (vendor_id, product_id).  Watches the file for
 * changes and atomically reloads on modification.
 *
 * Internally, the parser produces a flat Vec<module_info>-style array
 * that can later be extended to merge definitions from multiple sources.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/module_registry.h"
#include "../include/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <json-c/json.h>

/* ── Internal types ──────────────────────────────────────────────────────── */

/* Per-category defaults */
struct category_defaults {
    struct module_action  on_attach;
    struct module_action  on_detach;
    struct module_sync_policy sync_policy;
    int has_defaults;
};

struct module_registry {
    /* Path to the JSON file */
    char path[PATH_MAX];

    /* Module definitions */
    struct module_info *modules;
    int                 count;
    int                 capacity;

    /* Category defaults */
    struct category_defaults defaults[DEV_CAT_COUNT];

    /* inotify */
    char watch_dir[PATH_MAX];
    char watch_name[NAME_MAX + 1];
    int inotify_fd;
    int watch_fd;
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/**
 * Safely copy a JSON string value into a fixed-size buffer.
 */
static void json_strcpy(char *dst, size_t dstlen,
                        struct json_object *obj, const char *key)
{
    struct json_object *val;
    if (json_object_object_get_ex(obj, key, &val) &&
        json_object_is_type(val, json_type_string)) {
        const char *s = json_object_get_string(val);
        if (s) {
            size_t slen = strlen(s);
            if (slen >= dstlen) {
                slen = dstlen - 1;
            }
            memcpy(dst, s, slen);
            dst[slen] = '\0';
            return;
        }
    }
    dst[0] = '\0';
}

/**
 * Parse an action object. mount_point is optional and may contain {device},
 * which is expanded to the selected block-device basename at attach time.
 */
static void parse_action(struct json_object *obj, struct module_action *act)
{
    memset(act, 0, sizeof(*act));
    if (!obj || !json_object_is_type(obj, json_type_object)) {
        return;
    }

    json_strcpy(act->action, sizeof(act->action), obj, "action");
    json_strcpy(act->options, sizeof(act->options), obj, "options");
    json_strcpy(act->mount_point, sizeof(act->mount_point), obj,
                "mount_point");
    act->has_action = (act->action[0] != '\0') ? 1 : 0;
}

/**
 * Parse a sync_policy object from JSON.
 */
static void parse_sync_policy(struct json_object *obj,
                              struct module_sync_policy *sp)
{
    memset(sp, 0, sizeof(*sp));
    sp->mode = SYNC_MODE_IDLE;
    sp->idle_sync_delay_s = STORAGE_DEFAULT_IDLE_SYNC_DELAY_S;
    sp->fallback_sync_interval_s = STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S;

    if (!obj || !json_object_is_type(obj, json_type_object)) {
        return;
    }

    struct json_object *val;

    if (json_object_object_get_ex(obj, "mode", &val)) {
        const char *mode_str = json_object_get_string(val);
        if (mode_str) {
            if (strcmp(mode_str, "idle") == 0)
                sp->mode = SYNC_MODE_IDLE;
            else if (strcmp(mode_str, "periodic") == 0)
                sp->mode = SYNC_MODE_PERIODIC;
            else if (strcmp(mode_str, "manual") == 0)
                sp->mode = SYNC_MODE_MANUAL;
            else if (strcmp(mode_str, "disabled") == 0)
                sp->mode = SYNC_MODE_DISABLED;
        }
    }

    if (json_object_object_get_ex(obj, "idle_sync_delay", &val)) {
        sp->idle_sync_delay_s = json_object_get_int(val);
        if (sp->idle_sync_delay_s <= 0) {
            sp->idle_sync_delay_s = STORAGE_DEFAULT_IDLE_SYNC_DELAY_S;
        }
    }

    if (json_object_object_get_ex(obj, "fallback_sync_interval", &val)) {
        sp->fallback_sync_interval_s = json_object_get_int(val);
        if (sp->fallback_sync_interval_s <= 0) {
            sp->fallback_sync_interval_s = STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S;
        }
    }
}

/* Storage handler default constants (avoid circular include) */
#ifndef STORAGE_DEFAULT_IDLE_SYNC_DELAY_S
#define STORAGE_DEFAULT_IDLE_SYNC_DELAY_S       5
#endif
#ifndef STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S
#define STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S 60
#endif

/**
 * Parse a single module entry from the "modules" array.
 */
static int parse_module(struct json_object *entry, struct module_info *info)
{
    memset(info, 0, sizeof(*info));

    if (!entry || !json_object_is_type(entry, json_type_object)) {
        return -1;
    }

    json_strcpy(info->vendor_id, sizeof(info->vendor_id), entry, "vendor_id");
    json_strcpy(info->product_id, sizeof(info->product_id), entry, "product_id");
    json_strcpy(info->name, sizeof(info->name), entry, "name");
    json_strcpy(info->description, sizeof(info->description), entry, "description");

    /* Category */
    struct json_object *cat_obj;
    if (json_object_object_get_ex(entry, "category", &cat_obj)) {
        const char *cat_str = json_object_get_string(cat_obj);
        info->category = category_from_string(cat_str);
    } else {
        info->category = DEV_CAT_UNKNOWN;
    }

    /* on_attach / on_detach */
    struct json_object *act_obj;
    if (json_object_object_get_ex(entry, "on_attach", &act_obj)) {
        parse_action(act_obj, &info->on_attach);
    }
    if (json_object_object_get_ex(entry, "on_detach", &act_obj)) {
        parse_action(act_obj, &info->on_detach);
    }

    /* sync_policy */
    struct json_object *sync_obj;
    if (json_object_object_get_ex(entry, "sync_policy", &sync_obj)) {
        parse_sync_policy(sync_obj, &info->sync_policy);
        info->has_sync_policy = 1;
    } else {
        info->sync_policy.mode = SYNC_MODE_IDLE;
        info->sync_policy.idle_sync_delay_s = STORAGE_DEFAULT_IDLE_SYNC_DELAY_S;
        info->sync_policy.fallback_sync_interval_s = STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S;
    }

    /* Validate: vendor_id and product_id are required */
    if (info->vendor_id[0] == '\0' || info->product_id[0] == '\0') {
        LOG_WARN("registry: module entry missing vendor_id or product_id");
        return -1;
    }

    return 0;
}

/**
 * Parse the "defaults" section.
 */
static void parse_defaults(struct json_object *defaults_obj,
                           struct category_defaults defs[])
{
    if (!defaults_obj || !json_object_is_type(defaults_obj, json_type_object)) {
        return;
    }

    /* Iterate over category keys: "storage", "hid", "serial", etc. */
    struct json_object_iterator it = json_object_iter_begin(defaults_obj);
    struct json_object_iterator end = json_object_iter_end(defaults_obj);

    while (!json_object_iter_equal(&it, &end)) {
        const char *key = json_object_iter_peek_name(&it);
        struct json_object *val = json_object_iter_peek_value(&it);

        enum device_category cat = category_from_string(key);
        if (cat != DEV_CAT_UNKNOWN && cat < DEV_CAT_COUNT) {
            struct json_object *act_obj;
            if (json_object_object_get_ex(val, "on_attach", &act_obj)) {
                parse_action(act_obj, &defs[cat].on_attach);
            }
            if (json_object_object_get_ex(val, "on_detach", &act_obj)) {
                parse_action(act_obj, &defs[cat].on_detach);
            }
            struct json_object *sync_obj;
            if (json_object_object_get_ex(val, "sync_policy", &sync_obj)) {
                parse_sync_policy(sync_obj, &defs[cat].sync_policy);
            }
            defs[cat].has_defaults = 1;
        }

        json_object_iter_next(&it);
    }
}

/**
 * Internal: parse the full JSON file into the registry struct.
 */
static int registry_parse(struct module_registry *reg)
{
    struct json_object *root = json_object_from_file(reg->path);
    if (!root) {
        LOG_ERR("registry: failed to parse %s: %s",
                reg->path, json_util_get_last_err());
        return -1;
    }

    /* Parse "modules" array */
    struct json_object *modules_arr;
    if (!json_object_object_get_ex(root, "modules", &modules_arr) ||
        !json_object_is_type(modules_arr, json_type_array)) {
        LOG_ERR("registry: missing or invalid 'modules' array in %s",
                reg->path);
        json_object_put(root);
        return -1;
    }

    int n = (int)json_object_array_length(modules_arr);
    struct module_info *new_modules = calloc((size_t)n, sizeof(*new_modules));
    if (!new_modules && n > 0) {
        LOG_ERR("registry: out of memory allocating %d modules", n);
        json_object_put(root);
        return -1;
    }

    int valid_count = 0;
    for (int i = 0; i < n; i++) {
        struct json_object *entry = json_object_array_get_idx(modules_arr, (size_t)i);
        if (parse_module(entry, &new_modules[valid_count]) == 0) {
            valid_count++;
        }
    }

    /* Parse "defaults" section (optional) */
    struct category_defaults new_defaults[DEV_CAT_COUNT];
    memset(new_defaults, 0, sizeof(new_defaults));

    struct json_object *defaults_obj;
    if (json_object_object_get_ex(root, "defaults", &defaults_obj)) {
        parse_defaults(defaults_obj, new_defaults);
    }

    json_object_put(root);

    /* Atomic swap: free old data, install new */
    free(reg->modules);
    reg->modules = new_modules;
    reg->count = valid_count;
    reg->capacity = n;
    memcpy(reg->defaults, new_defaults, sizeof(reg->defaults));

    LOG_INFO("registry: loaded %d module definition(s) from %s",
             valid_count, reg->path);
    return 0;
}

static int registry_setup_watch(struct module_registry *reg)
{
    const char *slash;
    size_t dirlen;
    size_t namelen;

    if (!reg || reg->inotify_fd < 0) {
        return -1;
    }

    slash = strrchr(reg->path, '/');
    if (!slash) {
        memcpy(reg->watch_dir, ".", 2);
        dirlen = 1;
        reg->watch_name[0] = '\0';
        namelen = strlen(reg->path);
        if (namelen >= sizeof(reg->watch_name)) {
            namelen = sizeof(reg->watch_name) - 1;
        }
        memcpy(reg->watch_name, reg->path, namelen);
        reg->watch_name[namelen] = '\0';
    } else {
        dirlen = (size_t)(slash - reg->path);
        if (dirlen == 0) {
            memcpy(reg->watch_dir, "/", 2);
        } else {
            if (dirlen >= sizeof(reg->watch_dir)) {
                dirlen = sizeof(reg->watch_dir) - 1;
            }
            memcpy(reg->watch_dir, reg->path, dirlen);
            reg->watch_dir[dirlen] = '\0';
        }

        namelen = strlen(slash + 1);
        if (namelen >= sizeof(reg->watch_name)) {
            namelen = sizeof(reg->watch_name) - 1;
        }
        memcpy(reg->watch_name, slash + 1, namelen);
        reg->watch_name[namelen] = '\0';
    }

    reg->watch_fd = inotify_add_watch(reg->inotify_fd, reg->watch_dir,
                                      IN_CLOSE_WRITE | IN_MOVED_TO |
                                          IN_CREATE | IN_DELETE |
                                          IN_DELETE_SELF | IN_MOVE_SELF);
    if (reg->watch_fd < 0) {
        LOG_WARN("registry: inotify_add_watch failed for %s: %s",
                 reg->watch_dir, strerror(errno));
        return -1;
    }

    LOG_INFO("registry: watching %s for %s", reg->watch_dir, reg->watch_name);
    return 0;
}

/* ── Public API — Lifecycle ──────────────────────────────────────────────── */

struct module_registry *registry_load(const char *path)
{
    if (!path) {
        LOG_ERR("registry: NULL path");
        return NULL;
    }

    struct module_registry *reg = calloc(1, sizeof(*reg));
    if (!reg) {
        LOG_ERR("registry: out of memory");
        return NULL;
    }

    size_t pathlen = strlen(path);
    if (pathlen >= sizeof(reg->path)) {
        pathlen = sizeof(reg->path) - 1;
    }
    memcpy(reg->path, path, pathlen);
    reg->path[pathlen] = '\0';

    reg->inotify_fd = -1;
    reg->watch_fd = -1;

    /* Parse the JSON file */
    if (registry_parse(reg) != 0) {
        free(reg);
        return NULL;
    }

    /* Set up inotify watch */
    reg->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (reg->inotify_fd < 0) {
        LOG_WARN("registry: inotify_init1 failed: %s (live reload disabled)",
                 strerror(errno));
    } else {
        if (registry_setup_watch(reg) != 0) {
            close(reg->inotify_fd);
            reg->inotify_fd = -1;
        } else {
            LOG_INFO("registry: live reload enabled for %s", reg->path);
        }
    }

    return reg;
}

int registry_reload(struct module_registry *reg)
{
    if (!reg) {
        return -1;
    }

    LOG_INFO("registry: reloading %s", reg->path);
    return registry_parse(reg);
}

void registry_free(struct module_registry *reg)
{
    if (!reg) {
        return;
    }

    if (reg->watch_fd >= 0) {
        inotify_rm_watch(reg->inotify_fd, reg->watch_fd);
    }
    if (reg->inotify_fd >= 0) {
        close(reg->inotify_fd);
    }

    free(reg->modules);
    free(reg);
}

/* ── Public API — Lookup ─────────────────────────────────────────────────── */

const struct module_info *registry_lookup(const struct module_registry *reg,
                                          const char *vendor_id,
                                          const char *product_id)
{
    if (!reg || !vendor_id || !product_id) {
        return NULL;
    }

    for (int i = 0; i < reg->count; i++) {
        if (strcmp(reg->modules[i].vendor_id, vendor_id) == 0 &&
            strcmp(reg->modules[i].product_id, product_id) == 0) {
            return &reg->modules[i];
        }
    }

    return NULL;
}

const struct module_action *registry_default_attach(
    const struct module_registry *reg, enum device_category cat)
{
    if (!reg || cat >= DEV_CAT_COUNT || !reg->defaults[cat].has_defaults) {
        return NULL;
    }
    return reg->defaults[cat].on_attach.has_action
           ? &reg->defaults[cat].on_attach : NULL;
}

const struct module_action *registry_default_detach(
    const struct module_registry *reg, enum device_category cat)
{
    if (!reg || cat >= DEV_CAT_COUNT || !reg->defaults[cat].has_defaults) {
        return NULL;
    }
    return reg->defaults[cat].on_detach.has_action
           ? &reg->defaults[cat].on_detach : NULL;
}

const struct module_sync_policy *registry_default_sync(
    const struct module_registry *reg, enum device_category cat)
{
    if (!reg || cat >= DEV_CAT_COUNT || !reg->defaults[cat].has_defaults) {
        return NULL;
    }
    return &reg->defaults[cat].sync_policy;
}

/* ── Public API — inotify ────────────────────────────────────────────────── */

int registry_get_inotify_fd(const struct module_registry *reg)
{
    return reg ? reg->inotify_fd : -1;
}

int registry_handle_inotify_event(struct module_registry *reg)
{
    if (!reg || reg->inotify_fd < 0) {
        return -1;
    }

    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    int should_reload = 0;

    for (;;) {
        ssize_t n = read(reg->inotify_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN) {
                break;
            }
            LOG_WARN("registry: failed to read inotify events: %s",
                     strerror(errno));
            return -1;
        }
        if (n == 0) {
            break;  /* EAGAIN or error — done */
        }

        for (char *ptr = buf; ptr < buf + n; ) {
            struct inotify_event *ev = (struct inotify_event *)ptr;

            if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                should_reload = 1;
            } else if (ev->len > 0 && strcmp(ev->name, reg->watch_name) == 0) {
                if ((ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                                 IN_DELETE)) != 0) {
                    should_reload = 1;
                }
            }

            ptr += sizeof(struct inotify_event) + ev->len;
        }
    }

    if (!should_reload) {
        return 0;
    }

    LOG_INFO("registry: detected update for %s", reg->path);
    return registry_reload(reg);
}

/* ── Public API — Introspection ──────────────────────────────────────────── */

int registry_count(const struct module_registry *reg)
{
    return reg ? reg->count : 0;
}

const char *registry_path(const struct module_registry *reg)
{
    return reg ? reg->path : NULL;
}
