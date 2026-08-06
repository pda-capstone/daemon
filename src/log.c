/*
 * log.c — Logging implementation for hotswapd.
 *
 * Routes log messages to either syslog or stderr depending on how the
 * daemon was started.  Verbosity is controlled at init time via the
 * -v / -vv flags.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>
#include <time.h>

/* ── Module state ────────────────────────────────────────────────────────── */

static int  g_use_syslog;
static int  g_max_level = LOG_LVL_INFO;  /* default: show INFO and above */

/* ── Map our levels to syslog priorities ─────────────────────────────────── */

static int level_to_syslog_priority(enum log_level level)
{
    switch (level) {
    case LOG_LVL_ERROR:   return LOG_ERR;
    case LOG_LVL_WARN:    return LOG_WARNING;
    case LOG_LVL_INFO:    return LOG_INFO;
    case LOG_LVL_VERBOSE: return LOG_INFO;      /* syslog has no "verbose" */
    case LOG_LVL_DEBUG:   return LOG_DEBUG;
    }
    return LOG_INFO;
}

static const char *level_to_prefix(enum log_level level)
{
    switch (level) {
    case LOG_LVL_ERROR:   return "ERROR";
    case LOG_LVL_WARN:    return "WARN ";
    case LOG_LVL_INFO:    return "INFO ";
    case LOG_LVL_VERBOSE: return "VERB ";
    case LOG_LVL_DEBUG:   return "DEBUG";
    }
    return "?????";
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void log_init(int use_syslog, int verbosity)
{
    g_use_syslog = use_syslog;

    /* Map verbosity flag count to maximum visible level */
    switch (verbosity) {
    case 0:  g_max_level = LOG_LVL_INFO;    break;
    case 1:  g_max_level = LOG_LVL_VERBOSE; break;
    default: g_max_level = LOG_LVL_DEBUG;   break;
    }

    if (g_use_syslog) {
        openlog("hotswapd", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }
}

void log_shutdown(void)
{
    if (g_use_syslog) {
        closelog();
    }
}

void log_msg(enum log_level level, const char *fmt, ...)
{
    if ((int)level > g_max_level) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);

    if (g_use_syslog) {
        vsyslog(level_to_syslog_priority(level), fmt, ap);
    } else {
        /* stderr mode: prefix with timestamp and level */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);

        fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                ts.tv_nsec / 1000000L,
                level_to_prefix(level));
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        fflush(stderr);
    }

    va_end(ap);
}
