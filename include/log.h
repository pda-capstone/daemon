/*
 * log.h — Logging interface for hotswapd.
 *
 * Supports syslog (daemon mode) and stderr (foreground mode).
 * Verbosity levels: normal, verbose (-v), debug (-vv).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef HOTSWAPD_LOG_H
#define HOTSWAPD_LOG_H

/* Log levels */

enum log_level {
    LOG_LVL_ERROR = 0,
    LOG_LVL_WARN,
    LOG_LVL_INFO,
    LOG_LVL_VERBOSE,
    LOG_LVL_DEBUG
};

/* Initialization */

/**
 * Initialize the logging subsystem.
 *
 * @param use_syslog  If nonzero, log to syslog. Otherwise log to stderr.
 * @param verbosity   0 = normal (INFO+), 1 = verbose, 2 = debug.
 */
void log_init(int use_syslog, int verbosity);

/**
 * Shut down the logging subsystem (closes syslog if open).
 */
void log_shutdown(void);

/* Core logging function */

/**
 * Log a message at the given level.
 * Messages above the configured verbosity threshold are silently dropped.
 */
void log_msg(enum log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Convenience macros */

#define LOG_ERR(fmt, ...)     log_msg(LOG_LVL_ERROR,   fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)    log_msg(LOG_LVL_WARN,    fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    log_msg(LOG_LVL_INFO,    fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(fmt, ...) log_msg(LOG_LVL_VERBOSE, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)   log_msg(LOG_LVL_DEBUG,   fmt, ##__VA_ARGS__)

#endif /* HOTSWAPD_LOG_H */
