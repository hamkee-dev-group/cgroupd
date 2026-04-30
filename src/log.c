#define _GNU_SOURCE
#include "log.h"
#include <string.h>
#include <unistd.h>

int g_log_level = LOG_INFO_;

static const char *level_str(int level) {
    switch (level) {
    case LOG_ERR_:   return "ERR ";
    case LOG_WARN_:  return "WARN";
    case LOG_INFO_:  return "INFO";
    case LOG_DEBUG_: return "DBG ";
    default:         return "??? ";
    }
}

void log_msg(int level, const char *file, int line, const char *fmt, ...) {
    if (level > g_log_level) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char buf[64];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);

    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;

    fprintf(stderr, "%s.%03ld %s %s:%d ",
            buf, ts.tv_nsec / 1000000, level_str(level), base, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}
