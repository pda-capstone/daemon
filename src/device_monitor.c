/*
 * device_monitor.c — libudev device monitor.
 *
 * Enumerates existing USB devices at startup, then monitors for
 * attach/detach events via the kernel uevent interface.
 *
 * Only processes events for devtype == "usb_device" (not usb_interface)
 * to get one event per physical device.
 *
 * All sysfs attributes are cached at add time because they become
 * unreadable after a remove event.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/device_monitor.h"
#include "../include/device_state.h"
#include "../include/log.h"
#include "../include/power_info.h"

#include <libudev.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Internal structure */

struct device_monitor {
  struct udev *udev;
  struct udev_monitor *mon;
  const struct module_registry *registry;

  monitor_attach_cb on_attach;
  monitor_detach_cb on_detach;
};

/* Helpers */

/**
 * Safely copy a string, defaulting to "" if src is NULL.
 */
static void safe_strncpy(char *dst, const char *src, size_t n) {
  if (src) {
    size_t slen = strlen(src);
    if (slen >= n) {
      slen = n - 1;
    }
    memcpy(dst, src, slen);
    dst[slen] = '\0';
  } else {
    dst[0] = '\0';
  }
}

/**
 * Guess a device category from USB class codes.
 *
 * Uses bDeviceClass from the USB descriptor.  If bDeviceClass == 0x00
 * (per-interface), we would need to inspect interface classes — for now
 * we fall back to DEV_CAT_UNKNOWN and let the registry override.
 */
static enum device_category guess_category_from_class(const char *class_str) {
  if (!class_str) {
    return DEV_CAT_UNKNOWN;
  }

  unsigned int cls = (unsigned int)strtoul(class_str, NULL, 16);

  switch (cls) {
  case 0x01:
    return DEV_CAT_AUDIO; /* Audio */
  case 0x02:
    return DEV_CAT_SERIAL; /* CDC / serial */
  case 0x03:
    return DEV_CAT_HID; /* HID */
  case 0x08:
    return DEV_CAT_STORAGE; /* Mass Storage */
  case 0x09:
    return DEV_CAT_HUB; /* Hub */
  case 0x0e:
    return DEV_CAT_VIDEO; /* Video */
  case 0x0a:
    return DEV_CAT_SERIAL; /* CDC-Data */
  case 0xe0:
    return DEV_CAT_NETWORK; /* Wireless controller */
  default:
    return DEV_CAT_UNKNOWN;
  }
}

static void apply_sync_policy(struct hs_device *dev,
                              const struct module_sync_policy *policy) {
  if (!dev) {
    return;
  }

  dev->sync_policy = SYNC_MODE_IDLE;
  dev->idle_sync_delay_s = STORAGE_DEFAULT_IDLE_SYNC_DELAY_S;
  dev->fallback_sync_interval_s = STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S;

  if (!policy) {
    return;
  }

  dev->sync_policy = policy->mode;
  dev->idle_sync_delay_s = policy->idle_sync_delay_s > 0
                               ? policy->idle_sync_delay_s
                               : STORAGE_DEFAULT_IDLE_SYNC_DELAY_S;
  dev->fallback_sync_interval_s =
      policy->fallback_sync_interval_s > 0
          ? policy->fallback_sync_interval_s
          : STORAGE_DEFAULT_FALLBACK_SYNC_INTERVAL_S;
}

/**
 * Populate an hs_device from a udev_device object.
 */
static struct hs_device *
populate_device(struct udev_device *udev_dev,
                const struct module_registry *registry) {
  struct hs_device *dev = calloc(1, sizeof(*dev));
  if (!dev) {
    LOG_ERR("monitor: out of memory allocating hs_device");
    return NULL;
  }

  dev->sync_timer_fd = -1;
  dev->state = DEV_STATE_ATTACHED;
  clock_gettime(CLOCK_MONOTONIC, &dev->attached_at);
  apply_sync_policy(dev, NULL);

  /* Core identity from udev */
  safe_strncpy(dev->devpath, udev_device_get_devpath(udev_dev),
               sizeof(dev->devpath));
  safe_strncpy(dev->syspath, udev_device_get_syspath(udev_dev),
               sizeof(dev->syspath));

  /* USB IDs — try udev properties first, fall back to sysfs */
  const char *vid = udev_device_get_property_value(udev_dev, "ID_VENDOR_ID");
  if (!vid) {
    vid = udev_device_get_sysattr_value(udev_dev, "idVendor");
  }
  safe_strncpy(dev->vendor_id, vid, sizeof(dev->vendor_id));

  const char *pid = udev_device_get_property_value(udev_dev, "ID_MODEL_ID");
  if (!pid) {
    pid = udev_device_get_sysattr_value(udev_dev, "idProduct");
  }
  safe_strncpy(dev->product_id, pid, sizeof(dev->product_id));

  /* Names */
  const char *vendor = udev_device_get_property_value(udev_dev, "ID_VENDOR");
  if (!vendor) {
    vendor = udev_device_get_sysattr_value(udev_dev, "manufacturer");
  }
  safe_strncpy(dev->vendor_name, vendor, sizeof(dev->vendor_name));

  const char *model = udev_device_get_property_value(udev_dev, "ID_MODEL");
  if (!model) {
    model = udev_device_get_sysattr_value(udev_dev, "product");
  }
  safe_strncpy(dev->product_name, model, sizeof(dev->product_name));

  /* Serial */
  const char *serial =
      udev_device_get_property_value(udev_dev, "ID_SERIAL_SHORT");
  if (!serial) {
    serial = udev_device_get_sysattr_value(udev_dev, "serial");
  }
  safe_strncpy(dev->serial, serial, sizeof(dev->serial));

  /* Category — from USB device class */
  const char *devclass =
      udev_device_get_sysattr_value(udev_dev, "bDeviceClass");
  dev->category = guess_category_from_class(devclass);

  /* Override category from registry if we have a match */
  if (registry) {
    const struct module_info *info =
        registry_lookup(registry, dev->vendor_id, dev->product_id);
    if (info) {
      if (dev->product_name[0] == '\0') {
        safe_strncpy(dev->product_name, info->name, sizeof(dev->product_name));
      }
      dev->category = info->category;
      if (info->has_sync_policy) {
        apply_sync_policy(dev, &info->sync_policy);
      } else {
        apply_sync_policy(dev, registry_default_sync(registry, dev->category));
      }

      LOG_INFO("monitor: matched registry entry: %s (%s)", info->name,
               category_to_string(info->category));
    } else {
      apply_sync_policy(dev, registry_default_sync(registry, dev->category));
    }
  }

  /* Read power information */
  power_read_legacy(dev->syspath, dev);
  power_read_pd(dev);

  LOG_INFO("monitor: populated device %s — %s %s [%s:%s] %s, %u mA",
           dev->devpath, dev->vendor_name, dev->product_name, dev->vendor_id,
           dev->product_id, category_to_string(dev->category),
           dev->max_power_ma);

  return dev;
}

/* Public API */

struct device_monitor *monitor_create(const struct module_registry *reg,
                                      monitor_attach_cb on_attach,
                                      monitor_detach_cb on_detach) {
  struct device_monitor *m = calloc(1, sizeof(*m));
  if (!m) {
    LOG_ERR("monitor: out of memory");
    return NULL;
  }

  m->registry = reg;
  m->on_attach = on_attach;
  m->on_detach = on_detach;

  /* Create udev context */
  m->udev = udev_new();
  if (!m->udev) {
    LOG_ERR("monitor: udev_new() failed");
    free(m);
    return NULL;
  }

  /* Create udev monitor — listen for kernel uevents processed by udev */
  m->mon = udev_monitor_new_from_netlink(m->udev, "udev");
  if (!m->mon) {
    LOG_ERR("monitor: udev_monitor_new_from_netlink() failed");
    udev_unref(m->udev);
    free(m);
    return NULL;
  }

  /* Filter to USB device events only */
  if (udev_monitor_filter_add_match_subsystem_devtype(m->mon, "usb",
                                                      "usb_device") < 0) {
    LOG_ERR("monitor: failed to add subsystem filter");
    udev_monitor_unref(m->mon);
    udev_unref(m->udev);
    free(m);
    return NULL;
  }

  if (udev_monitor_enable_receiving(m->mon) < 0) {
    LOG_ERR("monitor: failed to enable receiving");
    udev_monitor_unref(m->mon);
    udev_unref(m->udev);
    free(m);
    return NULL;
  }

  LOG_INFO("monitor: initialized, listening for USB device events");
  return m;
}

int monitor_enumerate(struct device_monitor *mon) {
  if (!mon) {
    return -1;
  }

  struct udev_enumerate *en = udev_enumerate_new(mon->udev);
  if (!en) {
    LOG_ERR("monitor: udev_enumerate_new() failed");
    return -1;
  }

  udev_enumerate_add_match_subsystem(en, "usb");
  udev_enumerate_add_match_property(en, "DEVTYPE", "usb_device");
  udev_enumerate_scan_devices(en);

  int count = 0;
  struct udev_list_entry *entry;
  udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
    const char *path = udev_list_entry_get_name(entry);
    struct udev_device *udev_dev =
        udev_device_new_from_syspath(mon->udev, path);
    if (!udev_dev) {
      continue;
    }

    /*
     * Skip root hubs (they're always present and not "peripherals").
     * Root hubs have bDeviceClass == 09 and their parent is the
     * USB host controller, not another USB device.
     */
    const char *devclass =
        udev_device_get_sysattr_value(udev_dev, "bDeviceClass");
    if (devclass && strtoul(devclass, NULL, 16) == 0x09) {
      /* Check if it's a root hub (no USB parent device) */
      struct udev_device *parent =
          udev_device_get_parent_with_subsystem_devtype(udev_dev, "usb",
                                                        "usb_device");
      if (!parent) {
        udev_device_unref(udev_dev);
        continue; /* Root hub — skip */
      }
    }

    struct hs_device *dev = populate_device(udev_dev, mon->registry);
    udev_device_unref(udev_dev);

    if (dev && mon->on_attach) {
      mon->on_attach(dev);
      count++;
    } else {
      free(dev);
    }
  }

  udev_enumerate_unref(en);

  LOG_INFO("monitor: enumerated %d existing USB device(s)", count);
  return count;
}

int monitor_get_fd(const struct device_monitor *mon) {
  if (!mon || !mon->mon) {
    return -1;
  }
  return udev_monitor_get_fd(mon->mon);
}

int monitor_process_event(struct device_monitor *mon) {
  if (!mon || !mon->mon) {
    return -1;
  }

  struct udev_device *udev_dev = udev_monitor_receive_device(mon->mon);
  if (!udev_dev) {
    LOG_DEBUG("monitor: udev_monitor_receive_device returned NULL");
    return 0; /* Spurious wakeup, not an error */
  }

  const char *action = udev_device_get_action(udev_dev);
  const char *devpath = udev_device_get_devpath(udev_dev);

  if (!action || !devpath) {
    udev_device_unref(udev_dev);
    return 0;
  }

  LOG_DEBUG("monitor: event action=%s devpath=%s", action, devpath);

  if (strcmp(action, "add") == 0) {
    /*
     * Skip root hubs on hotplug too (shouldn't happen, but be safe).
     */
    const char *devclass =
        udev_device_get_sysattr_value(udev_dev, "bDeviceClass");
    if (devclass && strtoul(devclass, NULL, 16) == 0x09) {
      struct udev_device *parent =
          udev_device_get_parent_with_subsystem_devtype(udev_dev, "usb",
                                                        "usb_device");
      if (!parent) {
        udev_device_unref(udev_dev);
        return 0;
      }
    }

    struct hs_device *dev = populate_device(udev_dev, mon->registry);
    if (dev && mon->on_attach) {
      mon->on_attach(dev);
    } else {
      free(dev);
    }

  } else if (strcmp(action, "remove") == 0) {
    /*
     * On remove, sysfs attributes are already gone.  We only have
     * the devpath.  Check if the device was in DETACHING state to
     * determine if this was a clean or unclean detach.
     */
    struct hs_device *tracked = state_find(devpath);
    int was_unclean = 0;

    if (tracked) {
      was_unclean = (tracked->state != DEV_STATE_DETACHING) ? 1 : 0;
      if (was_unclean) {
        LOG_WARN("monitor: unclean detach detected for %s (%s %s)", devpath,
                 tracked->vendor_name, tracked->product_name);
      }
    }

    if (mon->on_detach) {
      mon->on_detach(devpath, was_unclean);
    }

  } else if (strcmp(action, "bind") == 0) {
    LOG_VERBOSE("monitor: driver bound — %s", devpath);

  } else if (strcmp(action, "unbind") == 0) {
    LOG_VERBOSE("monitor: driver unbound — %s", devpath);

  } else if (strcmp(action, "change") == 0) {
    LOG_VERBOSE("monitor: change event — %s", devpath);
  }

  udev_device_unref(udev_dev);
  return 0;
}

void monitor_set_registry(struct device_monitor *mon,
                          const struct module_registry *reg) {
  if (mon) {
    mon->registry = reg;
  }
}

void monitor_destroy(struct device_monitor *mon) {
  if (!mon) {
    return;
  }

  if (mon->mon) {
    udev_monitor_unref(mon->mon);
  }
  if (mon->udev) {
    udev_unref(mon->udev);
  }
  free(mon);

  LOG_INFO("monitor: destroyed");
}
