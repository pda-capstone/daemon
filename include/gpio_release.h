/*
 * gpio_release.h — Rising-edge GPIO safe-release contact.
 *
 * SPDX-FileCopyrightText: 2026 Alexander Olivier
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HOTSWAPD_GPIO_RELEASE_H
#define HOTSWAPD_GPIO_RELEASE_H

#include <stddef.h>

#define GPIO_RELEASE_DEFAULT_LINE 26U
#define GPIO_RELEASE_DEFAULT_DEBOUNCE_US 50000U

struct gpio_release;

struct gpio_release_config {
  const char *chip_path; /* /dev/gpiochipN, or "auto" */
  unsigned int line_offset;
  unsigned int debounce_us;
};

/**
 * Request an input line with a pull-up and rising-edge events. The release
 * contact normally holds the line low; opening it lets the pull-up drive the
 * line high and triggers safe release.
 *
 * The implementation uses the Linux GPIO character-device v2 API directly,
 * so the daemon does not depend on the deprecated sysfs GPIO interface.
 */
struct gpio_release *
gpio_release_open(const struct gpio_release_config *config);

/** Return the pollable GPIO line-event fd, or -1. */
int gpio_release_get_fd(const struct gpio_release *release);

/**
 * Drain pending edge events.
 *
 * @return 1 when a debounced rising edge was observed, 0 when all events were
 *         ignored as bounce, or -1 on error.
 */
int gpio_release_process(struct gpio_release *release);

/** Return the resolved gpiochip path for diagnostics. */
const char *gpio_release_chip_path(const struct gpio_release *release);

/** Release the GPIO line and free the monitor. */
void gpio_release_close(struct gpio_release *release);

#endif /* HOTSWAPD_GPIO_RELEASE_H */
