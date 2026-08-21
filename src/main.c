/*
 * main.c — Main event loop and entry point for hotswapd.
 *
 * SPDX-FileCopyrightText: 2026 Alexander Olivier
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../include/dbus_service.h"
#include "../include/device_monitor.h"
#include "../include/device_state.h"
#include "../include/gpio_release.h"
#include "../include/hotswapd.h"
#include "../include/log.h"
#include "../include/module_registry.h"
#include "../include/storage_handler.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define MAX_EVENTS 16
#define MAX_SYSTEM_FDS 32

/* ── Event source types for epoll grouping ───────────────────────────────── */

enum event_source {
    SRC_UDEV,
    SRC_SIGNAL,
    SRC_INOTIFY,
    SRC_GPIO_RELEASE,
    SRC_ATTACH_TIMER,
    SRC_SYNC_TIMER
};

struct main_event_ctx {
    enum event_source source;
    int fd;
    void *data;
};

/* ── Global/Static State ─────────────────────────────────────────────────── */

static int g_epoll_fd = -1;
static void *g_active_contexts[MAX_SYSTEM_FDS];
static int g_active_context_count = 0;

static struct main_event_ctx g_ctx_udev = {SRC_UDEV, -1, NULL};
static struct main_event_ctx g_ctx_signal = {SRC_SIGNAL, -1, NULL};
static struct main_event_ctx g_ctx_inotify = {SRC_INOTIFY, -1, NULL};
static struct main_event_ctx g_ctx_gpio = {SRC_GPIO_RELEASE, -1, NULL};
static char g_release_devpath_prefix[HOTSWAP_MAX_DEVPATH];

/* ── Context Registration Helpers ────────────────────────────────────────── */

static void register_context(void *ctx) {
    if (g_active_context_count < MAX_SYSTEM_FDS) {
        g_active_contexts[g_active_context_count++] = ctx;
    }
}

static void unregister_context(void *ctx) {
    for (int i = 0; i < g_active_context_count; i++) {
        if (g_active_contexts[i] == ctx) {
            for (int j = i; j < g_active_context_count - 1; j++) {
                g_active_contexts[j] = g_active_contexts[j + 1];
            }
            g_active_context_count--;
            break;
        }
    }
}

static int register_device_fd(enum event_source source, int fd,
                              struct hs_device *dev) {
    if (fd < 0 || !dev) {
        return -1;
    }
    struct main_event_ctx *context = malloc(sizeof(*context));
    if (!context) {
        return -1;
    }
    context->source = source;
    context->fd = fd;
    context->data = dev;

    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.ptr = context;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
        LOG_ERR("main: failed to add storage fd to epoll: %s", strerror(errno));
        free(context);
        return -1;
    }
    register_context(context);
    return 0;
}

static void remove_device_contexts(struct hs_device *dev) {
    int index = 0;
    while (index < g_active_context_count) {
        struct main_event_ctx *context = g_active_contexts[index];
        if (context && context->data == dev &&
            (context->source == SRC_ATTACH_TIMER ||
             context->source == SRC_SYNC_TIMER)) {
            epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, context->fd, NULL);
            unregister_context(context);
            free(context);
            continue;
        }
        index++;
    }
}

/* ── PID File Management ─────────────────────────────────────────────────── */

static void write_pid_file(void) {
    FILE *f = fopen(HOTSWAP_PID_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    } else {
        LOG_WARN("main: failed to write PID file %s: %s", HOTSWAP_PID_FILE,
                 strerror(errno));
    }
}

static void remove_pid_file(void) { unlink(HOTSWAP_PID_FILE); }

/* ── Device Monitor Callbacks ────────────────────────────────────────────── */

static void on_device_attach(struct hs_device *dev) {
    if (!dev) {
        return;
    }

    LOG_INFO("main: device attached: %s (%s %s)", dev->devpath,
             dev->vendor_name, dev->product_name);

    if (state_add(dev) != 0) {
        LOG_ERR("main: failed to add device to state: %s", dev->devpath);
        free(dev);
        return;
    }

    /* Handle storage specific initialization (mounting and sync timers) */
    if (dev->category == DEV_CAT_STORAGE) {
        storage_on_attach(dev);
        if (dev->attach_timer_fd >= 0) {
            register_device_fd(SRC_ATTACH_TIMER, dev->attach_timer_fd, dev);
        }
        if (dev->sync_timer_fd >= 0) {
            register_device_fd(SRC_SYNC_TIMER, dev->sync_timer_fd, dev);
        }
    }

    dbus_emit_module_attached(dev);
    dbus_emit_power_changed(state_total_power_ma(), state_count());
}

static void on_device_detach(const char *devpath, int was_unclean) {
    if (!devpath) {
        return;
    }

    struct hs_device *dev = state_find(devpath);
    if (!dev) {
        LOG_WARN("main: detach event for untracked device: %s", devpath);
        return;
    }

    LOG_INFO("main: device detached: %s (unclean=%d)", devpath, was_unclean);

    if (dev->category == DEV_CAT_STORAGE) {
        remove_device_contexts(dev);
        int dummy_was_unclean = was_unclean;
        storage_on_detach(dev, &dummy_was_unclean);
    }

    char product_name[HOTSWAP_MAX_NAME];
    strncpy(product_name, dev->product_name, sizeof(product_name) - 1);
    product_name[sizeof(product_name) - 1] = '\0';

    /* Remove from state list */
    state_remove(devpath);

    dbus_emit_module_detached(devpath, product_name, was_unclean);
    dbus_emit_power_changed(state_total_power_ma(), state_count());

    free(dev);
}

struct release_selection {
    const char *prefix;
    struct hs_device *only_device;
    int matched;
    int failed;
};

static int release_matches(const struct hs_device *dev, const char *prefix) {
    if (!dev || dev->state == DEV_STATE_DETACHED) {
        return 0;
    }
    return !prefix || prefix[0] == '\0' ||
           strncmp(dev->devpath, prefix, strlen(prefix)) == 0;
}

static int count_release_candidates_cb(const struct hs_device *const_dev,
                                       void *userdata) {
    struct release_selection *selection = userdata;
    if (!release_matches(const_dev, selection->prefix)) {
        return 0;
    }
    selection->matched++;
    selection->only_device = (struct hs_device *)const_dev;
    return 0;
}

static int prepare_release_cb(const struct hs_device *const_dev,
                              void *userdata) {
    struct release_selection *selection = userdata;
    struct hs_device *dev = (struct hs_device *)const_dev;
    if (!release_matches(dev, selection->prefix)) {
        return 0;
    }
    if (dev->state == DEV_STATE_DETACHING) {
        dbus_emit_module_ready(dev);
        return 0;
    }

    if (dev->category == DEV_CAT_STORAGE) {
        remove_device_contexts(dev);
        if (storage_prepare_release(dev) != 0) {
            selection->failed++;
            dbus_emit_release_failed(dev->devpath,
                                     "filesystem flush or unmount failed");
            /* Keep automatic sync policy alive when the module remains mounted.
             */
            if (storage_finish_attach(dev) == 0 && dev->sync_timer_fd >= 0) {
                register_device_fd(SRC_SYNC_TIMER, dev->sync_timer_fd, dev);
            }
            return 0;
        }
    }

    dev->state = DEV_STATE_DETACHING;
    LOG_INFO("main: module ready for physical removal: %s", dev->devpath);
    dbus_emit_module_ready(dev);
    return 0;
}

static void handle_gpio_release_trigger(void) {
    struct release_selection selection;
    memset(&selection, 0, sizeof(selection));
    selection.prefix = g_release_devpath_prefix;
    state_iterate(count_release_candidates_cb, &selection);

    if (selection.matched == 0) {
        LOG_WARN("gpio: release triggered but no matching module is attached");
        dbus_emit_release_failed(g_release_devpath_prefix,
                                 "no matching module is attached");
        return;
    }
    if (g_release_devpath_prefix[0] == '\0' && selection.matched != 1) {
        LOG_WARN(
            "gpio: release is ambiguous with %d attached modules; configure "
            "--release-devpath-prefix",
            selection.matched);
        dbus_emit_release_failed("",
                                 "multiple modules attached; configure a USB "
                                 "DEVPATH prefix for this release contact");
        return;
    }

    LOG_INFO("gpio: safe-release contact opened (%d module%s selected)",
             selection.matched, selection.matched == 1 ? "" : "s");
    if (g_release_devpath_prefix[0] == '\0') {
        selection.prefix = selection.only_device->devpath;
    }
    state_iterate(prepare_release_cb, &selection);
    if (selection.failed > 0) {
        LOG_WARN("gpio: %d selected module(s) are not safe to remove",
                 selection.failed);
    }
}

static int shutdown_storage_timers_cb(const struct hs_device *dev,
                                      void *userdata) {
    (void)userdata;

    if (!dev || dev->category != DEV_CAT_STORAGE) {
        return 0;
    }

    storage_stop_sync_timer((struct hs_device *)dev);
    storage_cancel_attach((struct hs_device *)dev);
    return 0;
}

/* ── CLI Usage ───────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -f          Run in the foreground (do not daemonize)\n");
    printf("  -c <path>   Path to modules.json registry config file\n");
    printf("  -G <chip>   GPIO chip path, or 'auto' (default: auto)\n");
    printf("  -L <line>   GPIO line offset (default: 26 / header pin 37)\n");
    printf(
        "  -P <prefix> USB DEVPATH prefix controlled by the release contact\n");
    printf("  --no-gpio-release  Disable the GPIO safe-release input\n");
    printf("  -v          Verbose logging output\n");
    printf("  -vv         Debug logging output (very verbose)\n");
    printf("  -h          Print this help message\n");
}

/* ── Main Entry Point ────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int foreground = 0;
    int verbosity = 0;
    const char *config_path = HOTSWAP_DEFAULT_REGISTRY_PATH;
    const char *gpio_chip_path = "auto";
    unsigned int gpio_line = GPIO_RELEASE_DEFAULT_LINE;
    int gpio_enabled = 1;

    static const struct option long_options[] = {
        {"gpio-chip", required_argument, NULL, 'G'},
        {"gpio-line", required_argument, NULL, 'L'},
        {"release-devpath-prefix", required_argument, NULL, 'P'},
        {"no-gpio-release", no_argument, NULL, 1000},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "fc:G:L:P:vvh", long_options,
                              NULL)) != -1) {
        switch (opt) {
        case 'f':
            foreground = 1;
            break;
        case 'c':
            config_path = optarg;
            break;
        case 'G':
            gpio_chip_path = optarg;
            break;
        case 'L': {
            char *end = NULL;
            unsigned long value = strtoul(optarg, &end, 10);
            if (!end || end == optarg || *end != '\0' || value > 65535UL) {
                fprintf(stderr, "Invalid GPIO line offset: %s\n", optarg);
                return EXIT_FAILURE;
            }
            gpio_line = (unsigned int)value;
            break;
        }
        case 'P':
            if (strlen(optarg) >= sizeof(g_release_devpath_prefix)) {
                fprintf(stderr, "Release DEVPATH prefix is too long\n");
                return EXIT_FAILURE;
            }
            snprintf(g_release_devpath_prefix, sizeof(g_release_devpath_prefix),
                     "%s", optarg);
            break;
        case 1000:
            gpio_enabled = 0;
            break;
        case 'v':
            verbosity = (verbosity < 1) ? 1 : verbosity;
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            if (optopt == 'c') {
                fprintf(stderr, "Option -c requires an argument.\n");
            }
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* Handle debug verbosity (double 'v') */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-vv") == 0) {
            verbosity = 2;
        }
    }

    /* 1. Daemonize if requested (MUST happen before descriptors / threads) */
    if (!foreground) {
        if (daemon(0, 0) < 0) {
            fprintf(stderr, "hotswapd: failed to daemonize: %s\n",
                    strerror(errno));
            return EXIT_FAILURE;
        }
    }

    /* 2. Initialize logging */
    log_init(!foreground, verbosity);
    LOG_INFO("main: starting hotswapd v%s (foreground=%d, verbosity=%d)",
             HOTSWAPD_VERSION_STRING, foreground, verbosity);

    /* 3. Write PID file */
    write_pid_file();

    /* 4. Set up signalfd for signal handling in epoll loop */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOG_ERR("main: sigprocmask failed: %s", strerror(errno));
        remove_pid_file();
        log_shutdown();
        return EXIT_FAILURE;
    }

    int sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd < 0) {
        LOG_ERR("main: signalfd creation failed: %s", strerror(errno));
        remove_pid_file();
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* 5. Load Module Registry */
    struct module_registry *reg = registry_load(config_path);
    if (!reg) {
        LOG_ERR("main: failed to load module registry from %s", config_path);
        close(sigfd);
        remove_pid_file();
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* 6. Initialize D-Bus Service */
    if (dbus_service_init(reg) != 0) {
        LOG_ERR("main: failed to initialize D-Bus service");
        registry_free(reg);
        close(sigfd);
        remove_pid_file();
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* 7. Create Epoll Instance */
    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epoll_fd < 0) {
        LOG_ERR("main: epoll_create1 failed: %s", strerror(errno));
        dbus_service_shutdown();
        registry_free(reg);
        close(sigfd);
        remove_pid_file();
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* 8. Setup D-Bus epoll watches */
    if (dbus_service_setup_epoll(g_epoll_fd) != 0) {
        LOG_ERR("main: failed to register D-Bus watches with epoll");
        close(g_epoll_fd);
        dbus_service_shutdown();
        registry_free(reg);
        close(sigfd);
        remove_pid_file();
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* 9. Register system-level fds in epoll */
    struct epoll_event ev;

    /* Register signalfd */
    g_ctx_signal.fd = sigfd;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &g_ctx_signal;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, sigfd, &ev) < 0) {
        LOG_ERR("main: failed to add signalfd to epoll: %s", strerror(errno));
        close(g_epoll_fd);
        dbus_service_shutdown();
        registry_free(reg);
        close(sigfd);
        remove_pid_file();
        log_shutdown();
        return EXIT_FAILURE;
    }
    register_context(&g_ctx_signal);

    /* GPIO safe release is optional at runtime so development machines and
     * carrier boards without the release contact can still run the daemon. */
    struct gpio_release *gpio_release = NULL;
    if (gpio_enabled) {
        struct gpio_release_config gpio_config = {
            .chip_path = gpio_chip_path,
            .line_offset = gpio_line,
            .debounce_us = GPIO_RELEASE_DEFAULT_DEBOUNCE_US,
        };
        gpio_release = gpio_release_open(&gpio_config);
        if (!gpio_release) {
            LOG_WARN("main: GPIO safe release unavailable: %s",
                     strerror(errno));
        } else {
            g_ctx_gpio.fd = gpio_release_get_fd(gpio_release);
            g_ctx_gpio.data = gpio_release;
            memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLIN;
            ev.data.ptr = &g_ctx_gpio;
            if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_ctx_gpio.fd, &ev) != 0) {
                LOG_WARN("main: failed to add GPIO release input to epoll: %s",
                         strerror(errno));
                gpio_release_close(gpio_release);
                gpio_release = NULL;
            } else {
                register_context(&g_ctx_gpio);
            }
        }
    }

    /* Register inotify watch fd */
    int inotify_fd = registry_get_inotify_fd(reg);
    if (inotify_fd >= 0) {
        g_ctx_inotify.fd = inotify_fd;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = &g_ctx_inotify;
        if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, inotify_fd, &ev) < 0) {
            LOG_ERR("main: failed to add inotify fd to epoll: %s",
                    strerror(errno));
            /* Non-fatal: registry changes just won't reload automatically */
        } else {
            register_context(&g_ctx_inotify);
        }
    }

    /* 10. Initialize Device Monitor */
    struct device_monitor *mon =
        monitor_create(reg, on_device_attach, on_device_detach);
    if (!mon) {
        LOG_ERR("main: failed to initialize device monitor");
        gpio_release_close(gpio_release);
        close(g_epoll_fd);
        dbus_service_shutdown();
        registry_free(reg);
        close(sigfd);
        remove_pid_file();
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* Register udev fd */
    int udev_fd = monitor_get_fd(mon);
    if (udev_fd >= 0) {
        g_ctx_udev.fd = udev_fd;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = &g_ctx_udev;
        if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, udev_fd, &ev) < 0) {
            LOG_ERR("main: failed to add udev fd to epoll: %s",
                    strerror(errno));
            monitor_destroy(mon);
            gpio_release_close(gpio_release);
            close(g_epoll_fd);
            dbus_service_shutdown();
            registry_free(reg);
            close(sigfd);
            remove_pid_file();
            log_shutdown();
            return EXIT_FAILURE;
        }
        register_context(&g_ctx_udev);
    }

    /* 11. Initial device enumeration */
    LOG_INFO("main: enumerating existing devices");
    monitor_enumerate(mon);

    /* 12. Main Event Loop */
    int keep_running = 1;
    struct epoll_event events[MAX_EVENTS];

    LOG_INFO("main: entering event loop");
    while (keep_running) {
        int nfds = epoll_wait(g_epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERR("main: epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            void *ptr = events[i].data.ptr;
            int is_our_ctx = 0;

            for (int j = 0; j < g_active_context_count; j++) {
                if (ptr == g_active_contexts[j]) {
                    is_our_ctx = 1;
                    break;
                }
            }

            if (is_our_ctx) {
                struct main_event_ctx *ctx = (struct main_event_ctx *)ptr;
                if (ctx->source == SRC_UDEV) {
                    monitor_process_event(mon);
                } else if (ctx->source == SRC_SIGNAL) {
                    struct signalfd_siginfo fdsi;
                    ssize_t s =
                        read(ctx->fd, &fdsi, sizeof(struct signalfd_siginfo));
                    if (s == sizeof(struct signalfd_siginfo)) {
                        if (fdsi.ssi_signo == SIGINT ||
                            fdsi.ssi_signo == SIGTERM) {
                            LOG_INFO("main: received termination signal (%d), "
                                     "shutting down",
                                     fdsi.ssi_signo);
                            keep_running = 0;
                        } else if (fdsi.ssi_signo == SIGHUP) {
                            LOG_INFO(
                                "main: received SIGHUP, reloading registry");
                            if (registry_reload(reg) == 0) {
                                monitor_set_registry(mon, reg);
                            }
                        }
                    }
                } else if (ctx->source == SRC_INOTIFY) {
                    LOG_INFO("main: registry file changed on disk, reloading");
                    if (registry_handle_inotify_event(reg) == 0) {
                        monitor_set_registry(mon, reg);
                    }
                } else if (ctx->source == SRC_GPIO_RELEASE) {
                    int triggered =
                        gpio_release_process((struct gpio_release *)ctx->data);
                    if (triggered > 0) {
                        handle_gpio_release_trigger();
                    } else if (triggered < 0) {
                        LOG_WARN("main: failed to read GPIO release event: %s",
                                 strerror(errno));
                    }
                } else if (ctx->source == SRC_ATTACH_TIMER) {
                    struct hs_device *dev = (struct hs_device *)ctx->data;
                    int result = storage_handle_attach_timer(dev);
                    if (result != STORAGE_ATTACH_PENDING) {
                        epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, ctx->fd, NULL);
                        unregister_context(ctx);
                        free(ctx);
                        storage_finish_attach(dev);
                        if (dev->sync_timer_fd >= 0) {
                            register_device_fd(SRC_SYNC_TIMER,
                                               dev->sync_timer_fd, dev);
                        }
                    }
                } else if (ctx->source == SRC_SYNC_TIMER) {
                    struct hs_device *dev = (struct hs_device *)ctx->data;
                    storage_handle_sync_timer(dev);
                }
            } else {
                /* D-Bus watch fd event */
                DBusWatch *watch = (DBusWatch *)ptr;
                unsigned int dbus_flags = 0;
                if (events[i].events & EPOLLIN) {
                    dbus_flags |= DBUS_WATCH_READABLE;
                }
                if (events[i].events & EPOLLOUT) {
                    dbus_flags |= DBUS_WATCH_WRITABLE;
                }
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    dbus_flags |= DBUS_WATCH_ERROR;
                }

                dbus_watch_handle(watch, dbus_flags);
                dbus_service_dispatch();
            }
        }
    }

    /* 13. Shutdown & Cleanup */
    LOG_INFO("main: shutting down daemon");

    if (mon) {
        monitor_destroy(mon);
    }

    dbus_service_shutdown();

    gpio_release_close(gpio_release);

    close(sigfd);

    if (reg) {
        registry_free(reg);
    }

    state_iterate(shutdown_storage_timers_cb, NULL);

    /* Free remaining dynamic storage timer contexts. */
    for (int i = 0; i < g_active_context_count; i++) {
        struct main_event_ctx *ctx = g_active_contexts[i];
        if (ctx && (ctx->source == SRC_ATTACH_TIMER ||
                    ctx->source == SRC_SYNC_TIMER)) {
            free(ctx);
        }
    }

    state_free_all();

    if (g_epoll_fd >= 0) {
        close(g_epoll_fd);
    }

    remove_pid_file();
    LOG_INFO("main: terminated successfully");
    log_shutdown();

    return EXIT_SUCCESS;
}
