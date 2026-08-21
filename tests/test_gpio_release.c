/* SPDX-FileCopyrightText: 2026 Alexander Olivier */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../include/gpio_release.h"
#include "../include/gpio_release_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(condition, message)                                              \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL: %s\n", message);                                  \
      failures++;                                                              \
    }                                                                          \
  } while (0)

struct release_pipe {
  struct gpio_release *release;
  int write_fd;
  int read_fd;
};

static struct release_pipe make_release(unsigned int debounce_us) {
  struct release_pipe pipe_state = {0};
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) {
    perror("pipe2");
    exit(EXIT_FAILURE);
  }
  pipe_state.read_fd = fds[0];
  pipe_state.write_fd = fds[1];
  pipe_state.release = gpio_release_test_adopt_event_fd(fds[0], debounce_us);
  if (!pipe_state.release) {
    perror("gpio_release_test_adopt_event_fd");
    exit(EXIT_FAILURE);
  }
  return pipe_state;
}

static void close_release(struct release_pipe *pipe_state) {
  gpio_release_close(pipe_state->release);
  close(pipe_state->write_fd);
  pipe_state->release = NULL;
  pipe_state->write_fd = -1;
}

static void emit_event(int fd, uint32_t event_id, uint64_t timestamp_ns) {
  struct gpio_v2_line_event event;
  memset(&event, 0, sizeof(event));
  event.id = event_id;
  event.timestamp_ns = timestamp_ns;
  ssize_t written = write(fd, &event, sizeof(event));
  if (written != (ssize_t)sizeof(event)) {
    perror("write gpio event");
    exit(EXIT_FAILURE);
  }
}

static void test_empty_queue(void) {
  struct release_pipe pipe_state =
      make_release(GPIO_RELEASE_DEFAULT_DEBOUNCE_US);
  CHECK(gpio_release_process(pipe_state.release) == 0,
        "an empty nonblocking event queue reports no trigger");
  close_release(&pipe_state);
}

static void test_edge_filtering(void) {
  struct release_pipe pipe_state =
      make_release(GPIO_RELEASE_DEFAULT_DEBOUNCE_US);

  emit_event(pipe_state.write_fd, GPIO_V2_LINE_EVENT_RISING_EDGE,
             1000000000ULL);
  CHECK(gpio_release_process(pipe_state.release) == 1,
        "a rising edge reports a release trigger");

  emit_event(pipe_state.write_fd, GPIO_V2_LINE_EVENT_FALLING_EDGE,
             2000000000ULL);
  CHECK(gpio_release_process(pipe_state.release) == 0,
        "a falling edge is ignored");
  CHECK(gpio_release_process(pipe_state.release) == 0,
        "processing drains the complete event queue");

  close_release(&pipe_state);
}

static void test_userspace_debounce(void) {
  struct release_pipe pipe_state =
      make_release(GPIO_RELEASE_DEFAULT_DEBOUNCE_US);

  emit_event(pipe_state.write_fd, GPIO_V2_LINE_EVENT_RISING_EDGE,
             1000000000ULL);
  CHECK(gpio_release_process(pipe_state.release) == 1,
        "the first rising edge is accepted");

  emit_event(pipe_state.write_fd, GPIO_V2_LINE_EVENT_RISING_EDGE,
             1010000000ULL);
  CHECK(gpio_release_process(pipe_state.release) == 0,
        "a rising edge inside the debounce window is rejected");

  emit_event(pipe_state.write_fd, GPIO_V2_LINE_EVENT_RISING_EDGE,
             1050000000ULL);
  CHECK(gpio_release_process(pipe_state.release) == 1,
        "an edge at the debounce boundary is accepted");

  close_release(&pipe_state);
}

static void test_zero_debounce(void) {
  struct release_pipe pipe_state = make_release(0);
  emit_event(pipe_state.write_fd, GPIO_V2_LINE_EVENT_RISING_EDGE, 1000ULL);
  CHECK(gpio_release_process(pipe_state.release) == 1,
        "zero debounce accepts the first edge");
  emit_event(pipe_state.write_fd, GPIO_V2_LINE_EVENT_RISING_EDGE, 1001ULL);
  CHECK(gpio_release_process(pipe_state.release) == 1,
        "zero debounce accepts an immediately following edge");
  close_release(&pipe_state);
}

static void test_multiple_events_are_drained(void) {
  struct release_pipe pipe_state =
      make_release(GPIO_RELEASE_DEFAULT_DEBOUNCE_US);
  emit_event(pipe_state.write_fd, GPIO_V2_LINE_EVENT_FALLING_EDGE,
             1000000000ULL);
  emit_event(pipe_state.write_fd, GPIO_V2_LINE_EVENT_RISING_EDGE,
             2000000000ULL);
  emit_event(pipe_state.write_fd, GPIO_V2_LINE_EVENT_RISING_EDGE,
             2010000000ULL);
  CHECK(gpio_release_process(pipe_state.release) == 1,
        "a batch reports an accepted trigger when other edges are ignored");
  CHECK(gpio_release_process(pipe_state.release) == 0,
        "a batch read drains every queued event");
  close_release(&pipe_state);
}

static void test_malformed_event(void) {
  struct release_pipe pipe_state =
      make_release(GPIO_RELEASE_DEFAULT_DEBOUNCE_US);
  unsigned char byte = 0;
  CHECK(write(pipe_state.write_fd, &byte, sizeof(byte)) == 1,
        "the malformed test byte is written");
  errno = 0;
  CHECK(gpio_release_process(pipe_state.release) == -1,
        "a short GPIO event is rejected");
  CHECK(errno == EIO, "a short GPIO event reports EIO");
  close_release(&pipe_state);
}

static void test_invalid_inputs_and_ownership(void) {
  errno = 0;
  CHECK(gpio_release_open(NULL) == NULL,
        "opening GPIO with a NULL configuration fails");
  CHECK(errno == EINVAL, "a NULL GPIO configuration reports EINVAL");

  errno = 0;
  CHECK(gpio_release_process(NULL) == -1,
        "processing a NULL release object fails");
  CHECK(errno == EINVAL, "a NULL release object reports EINVAL");
  CHECK(gpio_release_get_fd(NULL) == -1,
        "a NULL release object has no event fd");
  CHECK(strcmp(gpio_release_chip_path(NULL), "") == 0,
        "a NULL release object has an empty diagnostic path");
  gpio_release_close(NULL);

  errno = 0;
  CHECK(gpio_release_test_adopt_event_fd(-1, 50) == NULL,
        "adopting an invalid event fd fails");
  CHECK(errno == EINVAL, "an invalid event fd reports EINVAL");

  struct release_pipe pipe_state = make_release(50);
  CHECK(gpio_release_get_fd(pipe_state.release) == pipe_state.read_fd,
        "the adopted fd is exposed for epoll registration");
  CHECK(strcmp(gpio_release_chip_path(pipe_state.release), "test-event-fd") ==
            0,
        "a synthetic monitor has a diagnostic chip name");
  int owned_fd = pipe_state.read_fd;
  close_release(&pipe_state);
  errno = 0;
  CHECK(fcntl(owned_fd, F_GETFD) == -1 && errno == EBADF,
        "closing the monitor closes its adopted event fd");
}

static void test_gpiochip_scoring(void) {
  int rp1_named = gpio_release_test_chip_score("pinctrl-rp1", "GPIO26", 26);
  int rp1_unnamed = gpio_release_test_chip_score("pinctrl-rp1", "", 26);
  int bcm_named = gpio_release_test_chip_score("pinctrl-bcm2712", "GPIO26", 26);
  int unknown_named = gpio_release_test_chip_score("other", "GPIO26", 26);
  int unrelated = gpio_release_test_chip_score("other", "GPIO25", 26);

  CHECK(rp1_named > rp1_unnamed,
        "an exact GPIO line name improves the RP1 match");
  CHECK(rp1_unnamed > bcm_named,
        "CM5 auto-discovery prefers RP1 over a named BCM2712 line");
  CHECK(bcm_named > unknown_named,
        "a Raspberry Pi controller label outranks an unknown controller");
  CHECK(unknown_named > unrelated,
        "an exact line name can identify an otherwise unknown controller");
  CHECK(unrelated == 0, "an unrelated controller receives no match score");
}

int main(void) {
  test_empty_queue();
  test_edge_filtering();
  test_userspace_debounce();
  test_zero_debounce();
  test_multiple_events_are_drained();
  test_malformed_event();
  test_invalid_inputs_and_ownership();
  test_gpiochip_scoring();

  if (failures != 0) {
    return EXIT_FAILURE;
  }
  printf("test_gpio_release: ok\n");
  return EXIT_SUCCESS;
}
