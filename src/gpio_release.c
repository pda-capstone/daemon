/*
 * gpio_release.c — Linux GPIO character-device safe-release input.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/gpio_release.h"
#ifdef HOTSWAPD_TESTING
#include "../include/gpio_release_internal.h"
#endif
#include "../include/log.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#endif

struct gpio_release {
  int event_fd;
  unsigned int debounce_us;
  uint64_t last_trigger_ns;
  char chip_path[128];
};

#if defined(__linux__) && defined(GPIO_V2_GET_LINE_IOCTL)

static int chip_identity_score(const char *chip_label, const char *line_name,
                               const char *expected_name) {
  int score = 0;
  if (chip_label && strstr(chip_label, "pinctrl-rp1") != NULL) {
    /* The 40-pin header is driven by RP1 on CM5. Keep this preference higher
     * than a perfectly named line on the BCM2712 controller. */
    score = 8;
  } else if (chip_label && strstr(chip_label, "pinctrl-bcm") != NULL) {
    score = 4;
  }
  if (line_name && expected_name && strcmp(line_name, expected_name) == 0) {
    score += 2;
  }
  return score;
}

static int chip_match_score(int fd, unsigned int offset,
                            const char *expected_name) {
  struct gpiochip_info chip_info;
  memset(&chip_info, 0, sizeof(chip_info));
  if (ioctl(fd, (int)GPIO_GET_CHIPINFO_IOCTL, &chip_info) != 0 ||
      offset >= chip_info.lines) {
    return 0;
  }

  struct gpio_v2_line_info line_info;
  memset(&line_info, 0, sizeof(line_info));
  line_info.offset = offset;
  if (ioctl(fd, (int)GPIO_V2_GET_LINEINFO_IOCTL, &line_info) != 0) {
    line_info.name[0] = '\0';
  }

  /* Prefer RP1 over the BCM2712's own GPIO controller on CM5. Individual
   * line names add confidence, while the constrained label fallback supports
   * images that omit them. */
  return chip_identity_score(chip_info.label, line_info.name, expected_name);
}

#ifdef HOTSWAPD_TESTING
int gpio_release_test_chip_score(const char *chip_label, const char *line_name,
                                 unsigned int line_offset) {
  char expected_name[GPIO_MAX_NAME_SIZE];
  snprintf(expected_name, sizeof(expected_name), "GPIO%u", line_offset);
  return chip_identity_score(chip_label, line_name, expected_name);
}
#endif

static int open_auto_chip(unsigned int offset, char *resolved,
                          size_t resolved_size) {
  DIR *directory = opendir("/dev");
  if (!directory) {
    return -1;
  }

  char expected_name[GPIO_MAX_NAME_SIZE];
  snprintf(expected_name, sizeof(expected_name), "GPIO%u", offset);
  int selected_fd = -1;
  int selected_score = 0;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strncmp(entry->d_name, "gpiochip", 8) != 0) {
      continue;
    }

    char path[128];
    int written = snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(path)) {
      continue;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    int score = chip_match_score(fd, offset, expected_name);
    if (score > selected_score) {
      if (selected_fd >= 0) {
        close(selected_fd);
      }
      selected_fd = fd;
      selected_score = score;
      snprintf(resolved, resolved_size, "%s", path);
      continue;
    }
    close(fd);
  }

  closedir(directory);
  if (selected_fd < 0) {
    errno = ENODEV;
  }
  return selected_fd;
}

static int request_line(int chip_fd, unsigned int offset,
                        unsigned int debounce_us) {
  struct gpio_v2_line_request request;
  memset(&request, 0, sizeof(request));
  request.offsets[0] = offset;
  request.num_lines = 1;
  request.event_buffer_size = 16;
  snprintf(request.consumer, sizeof(request.consumer), "hotswapd-release");
  request.config.flags = GPIO_V2_LINE_FLAG_INPUT |
                         GPIO_V2_LINE_FLAG_EDGE_RISING |
                         GPIO_V2_LINE_FLAG_BIAS_PULL_UP;

  if (debounce_us > 0) {
    request.config.num_attrs = 1;
    request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
    request.config.attrs[0].attr.debounce_period_us = debounce_us;
    request.config.attrs[0].mask = 1;
  }

  if (ioctl(chip_fd, (int)GPIO_V2_GET_LINE_IOCTL, &request) == 0) {
    return request.fd;
  }

  /* Some GPIO drivers cannot apply the debounce attribute. Userspace still
   * filters event timestamps, so retry while retaining the required pull-up. */
  if (debounce_us > 0 && (errno == EINVAL || errno == ENOTSUP ||
                          errno == EOPNOTSUPP || errno == ENXIO)) {
    request.config.num_attrs = 0;
    if (ioctl(chip_fd, (int)GPIO_V2_GET_LINE_IOCTL, &request) == 0) {
      LOG_WARN("gpio: kernel debounce unavailable; using userspace debounce");
      return request.fd;
    }
  }
  return -1;
}

struct gpio_release *
gpio_release_open(const struct gpio_release_config *config) {
  if (!config) {
    errno = EINVAL;
    return NULL;
  }

  struct gpio_release *release = calloc(1, sizeof(*release));
  if (!release) {
    return NULL;
  }
  release->event_fd = -1;
  release->debounce_us = config->debounce_us;

  int chip_fd;
  if (!config->chip_path || strcmp(config->chip_path, "auto") == 0) {
    chip_fd = open_auto_chip(config->line_offset, release->chip_path,
                             sizeof(release->chip_path));
  } else {
    snprintf(release->chip_path, sizeof(release->chip_path), "%s",
             config->chip_path);
    chip_fd = open(release->chip_path, O_RDONLY | O_CLOEXEC);
  }
  if (chip_fd < 0) {
    free(release);
    return NULL;
  }

  release->event_fd =
      request_line(chip_fd, config->line_offset, config->debounce_us);
  int saved_errno = errno;
  close(chip_fd);
  if (release->event_fd < 0) {
    free(release);
    errno = saved_errno;
    return NULL;
  }
  int flags = fcntl(release->event_fd, F_GETFL);
  if (flags < 0 || fcntl(release->event_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    saved_errno = errno;
    close(release->event_fd);
    free(release);
    errno = saved_errno;
    return NULL;
  }

  LOG_INFO("gpio: monitoring %s line %u (physical header pin 37 on CM5IO)",
           release->chip_path, config->line_offset);
  return release;
}

int gpio_release_process(struct gpio_release *release) {
  if (!release || release->event_fd < 0) {
    errno = EINVAL;
    return -1;
  }

  int triggered = 0;
  for (;;) {
    struct gpio_v2_line_event event;
    ssize_t count = read(release->event_fd, &event, sizeof(event));
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    if (count < 0) {
      return -1;
    }
    if (count != (ssize_t)sizeof(event)) {
      errno = EIO;
      return -1;
    }
    if (event.id != GPIO_V2_LINE_EVENT_RISING_EDGE) {
      continue;
    }

    uint64_t debounce_ns = (uint64_t)release->debounce_us * 1000ULL;
    if (release->last_trigger_ns != 0 &&
        event.timestamp_ns - release->last_trigger_ns < debounce_ns) {
      continue;
    }
    release->last_trigger_ns = event.timestamp_ns;
    triggered = 1;
  }
  return triggered;
}

#ifdef HOTSWAPD_TESTING
struct gpio_release *
gpio_release_test_adopt_event_fd(int event_fd, unsigned int debounce_us) {
  if (event_fd < 0) {
    errno = EINVAL;
    return NULL;
  }

  int flags = fcntl(event_fd, F_GETFL);
  if (flags < 0 || fcntl(event_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return NULL;
  }

  struct gpio_release *release = calloc(1, sizeof(*release));
  if (!release) {
    return NULL;
  }
  release->event_fd = event_fd;
  release->debounce_us = debounce_us;
  snprintf(release->chip_path, sizeof(release->chip_path), "test-event-fd");
  return release;
}
#endif

#else

struct gpio_release *
gpio_release_open(const struct gpio_release_config *config) {
  (void)config;
  errno = ENOTSUP;
  return NULL;
}

int gpio_release_process(struct gpio_release *release) {
  (void)release;
  errno = ENOTSUP;
  return -1;
}

#ifdef HOTSWAPD_TESTING
struct gpio_release *
gpio_release_test_adopt_event_fd(int event_fd, unsigned int debounce_us) {
  (void)event_fd;
  (void)debounce_us;
  errno = ENOTSUP;
  return NULL;
}

int gpio_release_test_chip_score(const char *chip_label,
                                 const char *line_name,
                                 unsigned int line_offset) {
  (void)chip_label;
  (void)line_name;
  (void)line_offset;
  return 0;
}
#endif

#endif

int gpio_release_get_fd(const struct gpio_release *release) {
  return release ? release->event_fd : -1;
}

const char *gpio_release_chip_path(const struct gpio_release *release) {
  return release ? release->chip_path : "";
}

void gpio_release_close(struct gpio_release *release) {
  if (!release) {
    return;
  }
  if (release->event_fd >= 0) {
    close(release->event_fd);
  }
  free(release);
}
