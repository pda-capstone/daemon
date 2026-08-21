/*
 * hsctl.c — Command line interface tool for hotswapd.
 *
 * Talks to the daemon via the D-Bus system bus.
 * Commands: list, info <devpath>, power, registry, register, monitor.
 *
 * SPDX-FileCopyrightText: 2026 Alexander Olivier
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../../include/hotswapd.h"

#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── D-Bus Call Helper ───────────────────────────────────────────────────── */

static DBusMessage *send_method_call(DBusConnection *conn, const char *method,
                                     int arg_type, ...) {
  DBusMessage *msg = dbus_message_new_method_call(
      HOTSWAP_DBUS_BUS_NAME, HOTSWAP_DBUS_OBJECT_PATH, HOTSWAP_DBUS_INTERFACE,
      method);
  if (!msg) {
    fprintf(stderr, "Error: out of memory creating D-Bus message\n");
    return NULL;
  }

  if (arg_type != DBUS_TYPE_INVALID) {
    va_list args;
    va_start(args, arg_type);

    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);

    int current_type = arg_type;
    while (current_type != DBUS_TYPE_INVALID) {
      if (current_type == DBUS_TYPE_STRING) {
        const char *val = va_arg(args, const char *);
        dbus_message_iter_append_basic(&iter, current_type, &val);
      } else if (current_type == DBUS_TYPE_UINT32) {
        dbus_uint32_t val = va_arg(args, dbus_uint32_t);
        dbus_message_iter_append_basic(&iter, current_type, &val);
      } else if (current_type == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t val = va_arg(args, dbus_bool_t) ? TRUE : FALSE;
        dbus_message_iter_append_basic(&iter, current_type, &val);
      } else {
        fprintf(stderr, "Error: unsupported argument type %d\n", current_type);
        dbus_message_unref(msg);
        va_end(args);
        return NULL;
      }
      current_type = va_arg(args, int);
    }
    va_end(args);
  }

  DBusError err;
  dbus_error_init(&err);

  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
  dbus_message_unref(msg);

  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "D-Bus Error: %s\n", err.message);
    dbus_error_free(&err);
    return NULL;
  }

  return reply;
}

/* ── Commands ────────────────────────────────────────────────────────────── */

static int do_list(DBusConnection *conn) {
  DBusMessage *reply = send_method_call(conn, "ListModules", DBUS_TYPE_INVALID);
  if (!reply) {
    return EXIT_FAILURE;
  }

  DBusMessageIter iter, array_iter;
  if (!dbus_message_iter_init(reply, &iter) ||
      dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
    fprintf(stderr, "Error: ListModules returned invalid response type\n");
    dbus_message_unref(reply);
    return EXIT_FAILURE;
  }

  dbus_message_iter_recurse(&iter, &array_iter);

  printf("%-24s %-24s %-12s %-12s %-10s\n", "DEVPATH", "NAME", "CATEGORY",
         "STATE", "POWER (mA)");
  printf(
      "------------------------------------------------------------------------"
      "--------\n");

  int count = 0;
  while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_STRUCT) {
    DBusMessageIter struct_iter;
    dbus_message_iter_recurse(&array_iter, &struct_iter);

    const char *devpath = NULL;
    const char *name = NULL;
    const char *category = NULL;
    const char *state = NULL;
    dbus_uint32_t power = 0;

    if (dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_STRING) {
      dbus_message_iter_get_basic(&struct_iter, &devpath);
    }
    dbus_message_iter_next(&struct_iter);

    if (dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_STRING) {
      dbus_message_iter_get_basic(&struct_iter, &name);
    }
    dbus_message_iter_next(&struct_iter);

    if (dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_STRING) {
      dbus_message_iter_get_basic(&struct_iter, &category);
    }
    dbus_message_iter_next(&struct_iter);

    if (dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_STRING) {
      dbus_message_iter_get_basic(&struct_iter, &state);
    }
    dbus_message_iter_next(&struct_iter);

    if (dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_UINT32) {
      dbus_message_iter_get_basic(&struct_iter, &power);
    }

    printf("%-24s %-24s %-12s %-12s %-10u\n", devpath ? devpath : "",
           name ? name : "", category ? category : "", state ? state : "",
           power);

    count++;
    dbus_message_iter_next(&array_iter);
  }

  if (count == 0) {
    printf("(No hot-swap modules attached)\n");
  }

  dbus_message_unref(reply);
  return EXIT_SUCCESS;
}

static int do_info(DBusConnection *conn, const char *devpath) {
  DBusMessage *reply = send_method_call(conn, "GetModuleInfo", DBUS_TYPE_STRING,
                                        devpath, DBUS_TYPE_INVALID);
  if (!reply) {
    return EXIT_FAILURE;
  }

  DBusMessageIter iter, array_iter;
  if (!dbus_message_iter_init(reply, &iter) ||
      dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
    fprintf(stderr, "Error: GetModuleInfo returned invalid response type\n");
    dbus_message_unref(reply);
    return EXIT_FAILURE;
  }

  dbus_message_iter_recurse(&iter, &array_iter);

  printf("Properties for module %s:\n", devpath);
  printf("--------------------------------------------------\n");

  while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry_iter, val_iter;
    dbus_message_iter_recurse(&array_iter, &entry_iter);

    const char *key = NULL;
    if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_STRING) {
      dbus_message_iter_get_basic(&entry_iter, &key);
    }
    dbus_message_iter_next(&entry_iter);

    if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_VARIANT) {
      dbus_message_iter_recurse(&entry_iter, &val_iter);
      int val_type = dbus_message_iter_get_arg_type(&val_iter);

      printf("  %-20s: ", key ? key : "");

      if (val_type == DBUS_TYPE_STRING) {
        const char *val_str;
        dbus_message_iter_get_basic(&val_iter, &val_str);
        printf("%s\n", val_str);
      } else if (val_type == DBUS_TYPE_UINT32) {
        dbus_uint32_t val_u32;
        dbus_message_iter_get_basic(&val_iter, &val_u32);
        printf("%u\n", val_u32);
      } else if (val_type == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t val_bool;
        dbus_message_iter_get_basic(&val_iter, &val_bool);
        printf("%s\n", val_bool ? "true" : "false");
      } else {
        printf("(unsupported type %c)\n", (char)val_type);
      }
    }

    dbus_message_iter_next(&array_iter);
  }

  dbus_message_unref(reply);
  return EXIT_SUCCESS;
}

static int do_power(DBusConnection *conn) {
  DBusMessage *reply =
      send_method_call(conn, "GetTotalPowerDraw", DBUS_TYPE_INVALID);
  if (!reply) {
    return EXIT_FAILURE;
  }

  DBusMessageIter iter;
  if (!dbus_message_iter_init(reply, &iter) ||
      dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32) {
    fprintf(stderr,
            "Error: GetTotalPowerDraw returned invalid response type\n");
    dbus_message_unref(reply);
    return EXIT_FAILURE;
  }

  dbus_uint32_t total = 0;
  dbus_message_iter_get_basic(&iter, &total);

  printf("Total USB bus power draw by hot-swap modules: %u mA\n", total);

  dbus_message_unref(reply);
  return EXIT_SUCCESS;
}

static int do_registry(DBusConnection *conn) {
  DBusMessage *reply =
      send_method_call(conn, "ListRegistry", DBUS_TYPE_INVALID);
  if (!reply) {
    return EXIT_FAILURE;
  }

  DBusMessageIter iter, array_iter;
  if (!dbus_message_iter_init(reply, &iter) ||
      dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
    fprintf(stderr, "Error: ListRegistry returned invalid response type\n");
    dbus_message_unref(reply);
    return EXIT_FAILURE;
  }

  dbus_message_iter_recurse(&iter, &array_iter);
  printf("%-4s %-4s %-28s %-10s %s\n", "VID", "PID", "NAME", "CATEGORY",
         "DESCRIPTION");
  printf("---------------------------------------------------------------------"
         "----------\n");

  int count = 0;
  while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_STRUCT) {
    DBusMessageIter struct_iter;
    dbus_message_iter_recurse(&array_iter, &struct_iter);

    const char *values[5] = {NULL, NULL, NULL, NULL, NULL};
    int valid = 1;
    for (size_t i = 0; i < 5; i++) {
      if (dbus_message_iter_get_arg_type(&struct_iter) != DBUS_TYPE_STRING) {
        valid = 0;
        break;
      }
      dbus_message_iter_get_basic(&struct_iter, &values[i]);
      if (i < 4) {
        dbus_message_iter_next(&struct_iter);
      }
    }
    if (!valid) {
      fprintf(stderr, "Error: ListRegistry returned an invalid entry\n");
      dbus_message_unref(reply);
      return EXIT_FAILURE;
    }

    printf("%-4s %-4s %-28s %-10s %s\n", values[0], values[1], values[2],
           values[3], values[4]);
    count++;
    dbus_message_iter_next(&array_iter);
  }

  if (count == 0) {
    printf("(Registry is empty)\n");
  }

  dbus_message_unref(reply);
  return EXIT_SUCCESS;
}

static int do_register(DBusConnection *conn, const char *devpath, int replace,
                       const char *name, const char *category,
                       const char *description) {
  dbus_bool_t replace_value = replace ? TRUE : FALSE;
  const char *name_value = name ? name : "";
  const char *category_value = category ? category : "";
  const char *description_value = description ? description : "";

  DBusMessage *reply = send_method_call(
      conn, "RegisterModule", DBUS_TYPE_STRING, devpath, DBUS_TYPE_BOOLEAN,
      replace_value, DBUS_TYPE_STRING, name_value, DBUS_TYPE_STRING,
      category_value, DBUS_TYPE_STRING, description_value, DBUS_TYPE_INVALID);
  if (!reply) {
    return EXIT_FAILURE;
  }

  const char *vendor_id = NULL;
  const char *product_id = NULL;
  const char *registered_name = NULL;
  const char *registered_category = NULL;
  const char *registered_description = NULL;
  dbus_bool_t was_replaced = FALSE;
  DBusError error;
  dbus_error_init(&error);
  if (!dbus_message_get_args(
          reply, &error, DBUS_TYPE_STRING, &vendor_id, DBUS_TYPE_STRING,
          &product_id, DBUS_TYPE_STRING, &registered_name, DBUS_TYPE_STRING,
          &registered_category, DBUS_TYPE_STRING, &registered_description,
          DBUS_TYPE_BOOLEAN, &was_replaced, DBUS_TYPE_INVALID)) {
    fprintf(stderr, "Error: RegisterModule returned invalid response: %s\n",
            error.message);
    dbus_error_free(&error);
    dbus_message_unref(reply);
    return EXIT_FAILURE;
  }

  printf("%s module %s:%s\n", was_replaced ? "Updated" : "Registered",
         vendor_id, product_id);
  printf("  Name:        %s\n", registered_name);
  printf("  Category:    %s\n", registered_category);
  printf("  Description: %s\n", registered_description);
  printf("The entry will apply the next time the device is attached.\n");

  dbus_message_unref(reply);
  return EXIT_SUCCESS;
}

static int do_monitor(DBusConnection *conn) {
  DBusError err;
  dbus_error_init(&err);

  /* Add match rule to catch signals from hotswapd interface */
  dbus_bus_add_match(conn, "type='signal',interface='org.postmarketos.HotSwap'",
                     &err);
  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "Error adding match rule: %s\n", err.message);
    dbus_error_free(&err);
    return EXIT_FAILURE;
  }

  printf("Monitoring hotswapd events. Press Ctrl+C to exit...\n");
  printf("--------------------------------------------------\n");

  while (dbus_connection_read_write_dispatch(conn, -1)) {
    DBusMessage *msg;
    while ((msg = dbus_connection_pop_message(conn)) != NULL) {
      if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL) {
        const char *member = dbus_message_get_member(msg);

        if (strcmp(member, "ModuleAttached") == 0) {
          const char *devpath = NULL;
          const char *vid = NULL;
          const char *pid = NULL;
          const char *name = NULL;
          const char *category = NULL;
          dbus_uint32_t power = 0;
          dbus_uint32_t speed = 0;

          DBusError sig_err;
          dbus_error_init(&sig_err);

          if (dbus_message_get_args(
                  msg, &sig_err, DBUS_TYPE_STRING, &devpath, DBUS_TYPE_STRING,
                  &vid, DBUS_TYPE_STRING, &pid, DBUS_TYPE_STRING, &name,
                  DBUS_TYPE_STRING, &category, DBUS_TYPE_UINT32, &power,
                  DBUS_TYPE_UINT32, &speed, DBUS_TYPE_INVALID)) {
            printf("[ATTACH] Path: %s | %s [%s:%s] | Category: %s | MaxPower: "
                   "%u mA | Speed: %u Mbps\n",
                   devpath, name, vid, pid, category, power, speed);
          } else {
            fprintf(stderr, "Error parsing ModuleAttached: %s\n",
                    sig_err.message);
            dbus_error_free(&sig_err);
          }

        } else if (strcmp(member, "ModuleDetached") == 0) {
          const char *devpath = NULL;
          const char *name = NULL;
          dbus_bool_t was_unclean = FALSE;

          DBusError sig_err;
          dbus_error_init(&sig_err);

          if (dbus_message_get_args(msg, &sig_err, DBUS_TYPE_STRING, &devpath,
                                    DBUS_TYPE_STRING, &name, DBUS_TYPE_BOOLEAN,
                                    &was_unclean, DBUS_TYPE_INVALID)) {
            printf("[DETACH] Path: %s | %s | Clean detach: %s\n", devpath, name,
                   was_unclean ? "NO (UNCLEAN - potential data loss)" : "YES");
          } else {
            fprintf(stderr, "Error parsing ModuleDetached: %s\n",
                    sig_err.message);
            dbus_error_free(&sig_err);
          }

        } else if (strcmp(member, "PowerChanged") == 0) {
          dbus_uint32_t total = 0;
          dbus_uint32_t count = 0;

          DBusError sig_err;
          dbus_error_init(&sig_err);

          if (dbus_message_get_args(msg, &sig_err, DBUS_TYPE_UINT32, &total,
                                    DBUS_TYPE_UINT32, &count,
                                    DBUS_TYPE_INVALID)) {
            printf("[POWER ] Total Power Draw: %u mA | Connected Devices: %u\n",
                   total, count);
          } else {
            fprintf(stderr, "Error parsing PowerChanged: %s\n",
                    sig_err.message);
            dbus_error_free(&sig_err);
          }
        } else if (strcmp(member, "ModuleReadyForRemoval") == 0) {
          const char *devpath = NULL;
          const char *name = NULL;
          DBusError sig_err;
          dbus_error_init(&sig_err);
          if (dbus_message_get_args(msg, &sig_err, DBUS_TYPE_STRING, &devpath,
                                    DBUS_TYPE_STRING, &name,
                                    DBUS_TYPE_INVALID)) {
            printf("[READY ] Path: %s | %s | Safe to remove now\n", devpath,
                   name);
          } else {
            fprintf(stderr, "Error parsing ModuleReadyForRemoval: %s\n",
                    sig_err.message);
            dbus_error_free(&sig_err);
          }
        } else if (strcmp(member, "ModuleReleaseFailed") == 0) {
          const char *devpath = NULL;
          const char *reason = NULL;
          DBusError sig_err;
          dbus_error_init(&sig_err);
          if (dbus_message_get_args(msg, &sig_err, DBUS_TYPE_STRING, &devpath,
                                    DBUS_TYPE_STRING, &reason,
                                    DBUS_TYPE_INVALID)) {
            printf("[FAILED] Path: %s | Not safe to remove: %s\n", devpath,
                   reason);
          } else {
            fprintf(stderr, "Error parsing ModuleReleaseFailed: %s\n",
                    sig_err.message);
            dbus_error_free(&sig_err);
          }
        }
      }
      dbus_message_unref(msg);
    }
  }

  return EXIT_SUCCESS;
}

/* ── Main Entry ──────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
  fprintf(stderr, "Usage: %s <command> [arguments]\n", prog);
  fprintf(stderr, "Commands:\n");
  fprintf(stderr, "  list              List all currently attached modules\n");
  fprintf(stderr, "  info <devpath>    Show detailed info for a module\n");
  fprintf(stderr, "  power             Show total USB power draw\n");
  fprintf(stderr, "  registry          List registered module definitions\n");
  fprintf(stderr,
          "  register <devpath> [--replace] [--name <name>]\n"
          "             [--category <category>] [--description <text>]\n"
          "                    Register a currently attached module (root)\n");
  fprintf(stderr, "  monitor           Monitor events and signals live\n");
}

static int parse_register_options(int argc, char *argv[], int *replace,
                                  const char **name, const char **category,
                                  const char **description) {
  *replace = 0;
  *name = NULL;
  *category = NULL;
  *description = NULL;

  for (int i = 3; i < argc; i++) {
    if (strcmp(argv[i], "--replace") == 0) {
      *replace = 1;
      continue;
    }

    const char **destination = NULL;
    if (strcmp(argv[i], "--name") == 0) {
      destination = name;
    } else if (strcmp(argv[i], "--category") == 0) {
      destination = category;
    } else if (strcmp(argv[i], "--description") == 0) {
      destination = description;
    } else {
      fprintf(stderr, "Error: unknown register option '%s'\n", argv[i]);
      return -1;
    }

    if (i + 1 >= argc) {
      fprintf(stderr, "Error: %s requires a value\n", argv[i]);
      return -1;
    }
    *destination = argv[++i];
  }
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  const char *cmd = argv[1];

  DBusError err;
  dbus_error_init(&err);

  DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
  if (!conn) {
    fprintf(stderr, "Error connecting to D-Bus System Bus: %s\n", err.message);
    dbus_error_free(&err);
    return EXIT_FAILURE;
  }

  int rc = EXIT_FAILURE;

  if (strcmp(cmd, "list") == 0) {
    rc = do_list(conn);
  } else if (strcmp(cmd, "info") == 0) {
    if (argc < 3) {
      fprintf(stderr, "Error: info command requires a <devpath> argument\n");
      print_usage(argv[0]);
    } else {
      rc = do_info(conn, argv[2]);
    }
  } else if (strcmp(cmd, "power") == 0) {
    rc = do_power(conn);
  } else if (strcmp(cmd, "registry") == 0) {
    rc = do_registry(conn);
  } else if (strcmp(cmd, "register") == 0) {
    if (argc < 3) {
      fprintf(stderr,
              "Error: register command requires a <devpath> argument\n");
      print_usage(argv[0]);
    } else {
      int replace = 0;
      const char *name = NULL;
      const char *category = NULL;
      const char *description = NULL;
      if (parse_register_options(argc, argv, &replace, &name, &category,
                                 &description) == 0) {
        rc = do_register(conn, argv[2], replace, name, category, description);
      }
    }
  } else if (strcmp(cmd, "monitor") == 0) {
    rc = do_monitor(conn);
  } else {
    fprintf(stderr, "Error: unknown command '%s'\n", cmd);
    print_usage(argv[0]);
  }

  dbus_connection_unref(conn);
  return rc;
}
