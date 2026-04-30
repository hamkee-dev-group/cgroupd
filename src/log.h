#ifndef CGROUPD_LOG_H
#define CGROUPD_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

enum log_level {
    LOG_ERR_ = 0,
    LOG_WARN_ = 1,
    LOG_INFO_ = 2,
    LOG_DEBUG_ = 3,
};

extern int g_log_level;

void log_msg(int level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOGE(...) log_msg(LOG_ERR_,  __FILE__, __LINE__, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN_, __FILE__, __LINE__, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO_, __FILE__, __LINE__, __VA_ARGS__)
#define LOGD(...) log_msg(LOG_DEBUG_,__FILE__, __LINE__, __VA_ARGS__)

#endif
