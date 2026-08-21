/*
 * gpio_release_internal.h — Test seam for GPIO event handling.
 *
 * This is intentionally separate from gpio_release.h: daemon callers should
 * only use the production GPIO API.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef HOTSWAPD_GPIO_RELEASE_INTERNAL_H
#define HOTSWAPD_GPIO_RELEASE_INTERNAL_H

#include "gpio_release.h"

/** Adopt a synthetic GPIO event fd. The returned object owns the fd. */
struct gpio_release *gpio_release_test_adopt_event_fd(int event_fd,
                                                      unsigned int debounce_us);

/** Score a gpiochip identity using the same rules as automatic discovery. */
int gpio_release_test_chip_score(const char *chip_label, const char *line_name,
                                 unsigned int line_offset);

#endif /* HOTSWAPD_GPIO_RELEASE_INTERNAL_H */
