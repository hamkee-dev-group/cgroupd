#define _GNU_SOURCE
#include "psi.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_psi_line(const char *line, struct psi_avg *out) {
     
    const char *a10 = strstr(line, "avg10=");
    const char *a60 = strstr(line, "avg60=");
    const char *a300 = strstr(line, "avg300=");
    const char *tot = strstr(line, "total=");
    if (!a10 || !a60 || !a300 || !tot) return -1;
    out->avg10  = strtod(a10 + 6, NULL);
    out->avg60  = strtod(a60 + 6, NULL);
    out->avg300 = strtod(a300 + 7, NULL);
    out->total  = strtoull(tot + 6, NULL, 10);
    return 0;
}

int psi_read(const char *cgroup_path, const char *resource, struct psi *out) {
    char path[PATH_MAX];
    if (cgroup_path && *cgroup_path)
        snprintf(path, sizeof(path), "%s/%s.pressure", cgroup_path, resource);
    else
        snprintf(path, sizeof(path), "/proc/pressure/%s", resource);

    char buf[1024];
    if (read_file(path, buf, sizeof(buf)) < 0) return -1;

    memset(out, 0, sizeof(*out));
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "some ", 5) == 0)
            parse_psi_line(line + 5, &out->some);
        else if (strncmp(line, "full ", 5) == 0) {
            parse_psi_line(line + 5, &out->full);
            out->has_full = 1;
        }
        line = nl ? nl + 1 : NULL;
    }
    return 0;
}

int psi_watch(const char *cgroup_path, const char *resource,
              const char *level, uint64_t stall_us, uint64_t window_us) {
    char path[PATH_MAX];
    if (cgroup_path && *cgroup_path)
        snprintf(path, sizeof(path), "%s/%s.pressure", cgroup_path, resource);
    else
        snprintf(path, sizeof(path), "/proc/pressure/%s", resource);

    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
         
        LOGD("psi watch open %s: %s", path, strerror(errno));
        return -1;
    }
    char trig[64];
    int n = snprintf(trig, sizeof(trig), "%s %llu %llu",
                     level,
                     (unsigned long long)stall_us,
                     (unsigned long long)window_us);
    if (write(fd, trig, (size_t)n) < 0) {
        

        LOGD("psi watch trigger %s on %s: %s — falling back to polling",
             trig, path, strerror(errno));
        close(fd);
        return -1;
    }
    LOGI("psi watch armed: %s [%s]", path, trig);
    return fd;
}
