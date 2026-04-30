#define _GNU_SOURCE
#include "proto.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

 
int proto_read_msg(int fd, char **out_buf, size_t *out_len) {
    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) return -1;
    int saw_nl = 0;  
    while (1) {
        if (len + 1 >= cap) {
            if (cap >= 1u << 22) { free(buf); errno = E2BIG; return -1; }
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return -1;
        }
        if (n == 0) {
            if (len == 0) { free(buf); return 1; }   
            break;
        }
        len += (size_t)n;
        buf[len] = '\0';
         
        for (size_t i = 1; i < len; i++) {
            if (buf[i-1] == '\n' && buf[i] == '\n') { saw_nl = 1; break; }
        }
        if (saw_nl) break;
    }
    *out_buf = buf;
    *out_len = len;
    return 0;
}

int proto_write(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int proto_write_status(int fd, const char *status, const char *fmt, ...) {
    char body[4096];
    int n = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        n = vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);
        if (n < 0) n = 0;
        if ((size_t)n >= sizeof(body)) n = sizeof(body) - 1;
    }
    char hdr[64];
    int hl = snprintf(hdr, sizeof(hdr), "STATUS: %s\n", status);

    if (proto_write(fd, hdr, (size_t)hl) < 0) return -1;
    if (n > 0 && proto_write(fd, body, (size_t)n) < 0) return -1;
    

    if (n > 0 && body[n-1] != '\n') {
        if (proto_write(fd, "\n", 1) < 0) return -1;
    }
    if (proto_write(fd, "\n", 1) < 0) return -1;
    return 0;
}

const char *proto_verb(const char *buf, char *verb_out, size_t verb_out_len) {
    const char *nl = strchr(buf, '\n');
    if (!nl) return NULL;
    size_t l = (size_t)(nl - buf);
    if (l >= verb_out_len) l = verb_out_len - 1;
    memcpy(verb_out, buf, l);
    verb_out[l] = '\0';
     
    char *p = verb_out;
    while (*p == ' ') p++;
    if (p != verb_out) memmove(verb_out, p, strlen(p) + 1);
    rstrip(verb_out);
    return nl;
}

int proto_next_header(const char **cursor, char *scratch, size_t scratch_len,
                      char **key, char **val) {
    if (!cursor || !*cursor) return -1;
    const char *p = *cursor;
    if (*p == '\n') p++;             
    if (*p == '\n' || *p == '\0') {  
        *cursor = p;
        return 0;
    }
    const char *nl = strchr(p, '\n');
    if (!nl) return -1;
    size_t l = (size_t)(nl - p);
    if (l + 1 > scratch_len) return -1;
    memcpy(scratch, p, l);
    scratch[l] = '\0';

    char *colon = strchr(scratch, ':');
    if (!colon) return -1;
    *colon = '\0';
    char *k = scratch;
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t') v++;
    rstrip(k);
    rstrip(v);

    *key = k;
    *val = v;
    *cursor = nl;   
    return 1;
}
