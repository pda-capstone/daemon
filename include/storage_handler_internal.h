/*
 * storage_handler_internal.h — Test-only storage implementation seams.
 *
 * SPDX-FileCopyrightText: 2026 Alexander Olivier
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HOTSWAPD_STORAGE_HANDLER_INTERNAL_H
#define HOTSWAPD_STORAGE_HANDLER_INTERNAL_H

#include <stddef.h>

#ifdef HOTSWAPD_TESTING
int storage_test_resolve_usb_parent_path(const char *resolved, char *buf,
                                         size_t buflen);
#endif

#endif /* HOTSWAPD_STORAGE_HANDLER_INTERNAL_H */
