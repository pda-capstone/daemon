#
# Makefile for hotswapd and hsctl.
#
# SPDX-FileCopyrightText: 2026 Alexander Olivier
# SPDX-License-Identifier: GPL-3.0-or-later
#

CC      := gcc
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE -Iinclude
LDFLAGS +=

PREFIX ?= /usr
SBINDIR ?= $(PREFIX)/sbin
BINDIR ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
SYSTEMDUNITDIR ?= $(PREFIX)/lib/systemd/system
DBUS_POLICY_DIR ?= $(SYSCONFDIR)/dbus-1/system.d
HOTSWAPD_CONFIG_DIR ?= $(SYSCONFDIR)/hotswapd
MANDIR ?= $(PREFIX)/share/man

# Libraries queried via pkg-config
PKG_LIBS = libudev dbus-1 json-c
CFLAGS  += $(shell pkg-config --cflags $(PKG_LIBS))
LIBS     = $(shell pkg-config --libs $(PKG_LIBS))

DBUS_LIBS = $(shell pkg-config --libs dbus-1)

# Target Binaries
DAEMON = hotswapd
CLI    = hsctl

# Source Files
DAEMON_SRCS = src/main.c \
              src/device_monitor.c \
              src/gpio_release.c \
              src/usb_classification.c \
              src/module_registry.c \
              src/device_state.c \
              src/dbus_service.c \
              src/power_info.c \
              src/storage_handler.c \
              src/log.c

DAEMON_OBJS = $(DAEMON_SRCS:.c=.o)

CLI_SRCS = src/hsctl/hsctl.c
CLI_OBJS = $(CLI_SRCS:.c=.o)

.PHONY: all clean test install uninstall

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

all: $(DAEMON) $(CLI)

$(DAEMON): $(DAEMON_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(CLI): $(CLI_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(DBUS_LIBS)

clean:
	rm -f $(DAEMON) $(CLI) $(DAEMON_OBJS) $(CLI_OBJS)
	rm -f tests/test_registry tests/test_device_state tests/test_power_info tests/test_storage tests/test_gpio_release tests/test_usb_classification tests/*.o src/*.o src/hsctl/*.o

# Unit Tests Target
test: tests/test_registry tests/test_device_state tests/test_power_info tests/test_storage tests/test_gpio_release tests/test_usb_classification
	@echo "Running unit tests..."
	./tests/test_registry
	./tests/test_device_state
	./tests/test_power_info
	./tests/test_storage
	./tests/test_gpio_release
	./tests/test_usb_classification

tests/test_registry: tests/test_registry.o src/module_registry.o src/log.o src/device_state.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

tests/test_device_state: tests/test_device_state.o src/device_state.o src/log.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/test_power_info: tests/test_power_info.o src/power_info.o src/log.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/test_storage: tests/test_storage.o tests/storage_handler_testable.o src/log.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/test_storage.o: CFLAGS += -DHOTSWAPD_TESTING

tests/storage_handler_testable.o: src/storage_handler.c
	$(CC) $(CFLAGS) -DHOTSWAPD_TESTING -c -o $@ $<

tests/test_gpio_release: tests/test_gpio_release.o tests/gpio_release_testable.o src/log.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/gpio_release_testable.o: src/gpio_release.c
	$(CC) $(CFLAGS) -DHOTSWAPD_TESTING -c -o $@ $<

tests/test_usb_classification: tests/test_usb_classification.o src/usb_classification.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

# Installation
install: all
	install -d $(DESTDIR)$(SBINDIR)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(HOTSWAPD_CONFIG_DIR)
	install -d $(DESTDIR)$(DBUS_POLICY_DIR)
	install -d $(DESTDIR)$(SYSTEMDUNITDIR)
	install -d $(DESTDIR)$(MANDIR)/man8
	install -m 0755 $(DAEMON) $(DESTDIR)$(SBINDIR)/$(DAEMON)
	install -m 0755 $(CLI) $(DESTDIR)$(BINDIR)/$(CLI)
	install -m 0644 config/modules.json $(DESTDIR)$(HOTSWAPD_CONFIG_DIR)/modules.json
	install -m 0644 config/hotswapd.conf $(DESTDIR)$(DBUS_POLICY_DIR)/hotswapd.conf
	install -m 0644 config/hotswapd.service $(DESTDIR)$(SYSTEMDUNITDIR)/hotswapd.service
	install -m 0644 man/hotswapd.8 $(DESTDIR)$(MANDIR)/man8/hotswapd.8

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/$(DAEMON)
	rm -f $(DESTDIR)$(BINDIR)/$(CLI)
	rm -f $(DESTDIR)$(HOTSWAPD_CONFIG_DIR)/modules.json
	rm -f $(DESTDIR)$(DBUS_POLICY_DIR)/hotswapd.conf
	rm -f $(DESTDIR)$(SYSTEMDUNITDIR)/hotswapd.service
	rm -f $(DESTDIR)$(MANDIR)/man8/hotswapd.8
