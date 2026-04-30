#define _GNU_SOURCE
#include "util.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

ssize_t read_file(const char *path, char *buf, size_t buflen) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t total = 0;
    while ((size_t)total < buflen - 1) {
        ssize_t n = read(fd, buf + total, buflen - 1 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    close(fd);
    return total;
}

int write_file(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = strlen(s);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, s + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

int write_filef(const char *path, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        errno = EOVERFLOW;
        return -1;
    }
    return write_file(path, buf);
}

void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

uint64_t parse_size(const char *s) {
    if (!s || !*s) return (uint64_t)-1;
    if (strcmp(s, "max") == 0) return UINT64_MAX;

    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s) return (uint64_t)-1;
    while (*end == ' ' || *end == '\t') end++;

    uint64_t mult = 1;
    switch (*end) {
    case 0:                                   break;
    case 'k': case 'K': mult = 1024ULL;       break;
    case 'm': case 'M': mult = 1024ULL*1024;  break;
    case 'g': case 'G': mult = 1024ULL*1024*1024; break;
    case 't': case 'T': mult = 1024ULL*1024*1024*1024; break;
    default: return (uint64_t)-1;
    }
    if (*end && *(end + 1) != '\0' && !((*(end+1) == 'b' || *(end+1) == 'B') && *(end+2) == 0))
        return (uint64_t)-1;

    return (uint64_t)v * mult;
}

void format_size(uint64_t bytes, char *dst, size_t dstlen) {
    if (bytes == UINT64_MAX) { snprintf(dst, dstlen, "max"); return; }
    static const char *suf[] = {"B","K","M","G","T","P"};
    int i = 0;
    double v = (double)bytes;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    if (i == 0) snprintf(dst, dstlen, "%llu", (unsigned long long)bytes);
    else snprintf(dst, dstlen, "%.1f%s", v, suf[i]);
}

int mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) return -1;
    char buf[4096];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, path, len + 1);

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, mode) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

uint64_t now_real_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
