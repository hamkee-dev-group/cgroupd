#define _GNU_SOURCE
#include "cgroup.h"
#include "log.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

 
static int my_cgroup_rel(char *out, size_t outlen) {
    char buf[4096];
    if (read_file("/proc/self/cgroup", buf, sizeof(buf)) < 0) return -1;
     
    char *p = strstr(buf, "0::");
    if (!p) { errno = ENOENT; return -1; }
    p += 3;
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    if (snprintf(out, outlen, "%s", p) >= (int)outlen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int cg_resolve_root(const char *root_hint, char *dst, size_t dstlen) {
    if (root_hint && *root_hint) {
        if (snprintf(dst, dstlen, "%s", root_hint) >= (int)dstlen) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    if (geteuid() == 0) {
        if (snprintf(dst, dstlen, "%s/cgroupd.slice", CGV2_ROOT) >= (int)dstlen) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    char rel[PATH_MAX];
    if (my_cgroup_rel(rel, sizeof(rel)) < 0) return -1;

    uid_t uid = getuid();
    char user_service[PATH_MAX];
    if (snprintf(user_service, sizeof(user_service),
                 "%s/user.slice/user-%u.slice/user@%u.service",
                 CGV2_ROOT, (unsigned)uid, (unsigned)uid) < (int)sizeof(user_service) &&
        path_exists(user_service)) {
        if (snprintf(dst, dstlen, "%s/cgroupd.slice", user_service) >= (int)dstlen) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    


    if (strcmp(rel, "/") == 0) {
        if (snprintf(dst, dstlen, "%s/cgroupd.slice", CGV2_ROOT) >= (int)dstlen) {
            errno = ENAMETOOLONG; return -1;
        }
        return 0;
    }

     
    char parent[PATH_MAX];
    if (snprintf(parent, sizeof(parent), "%s", rel) >= (int)sizeof(parent)) {
        errno = ENAMETOOLONG; return -1;
    }
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = '\0';
    else parent[0] = '\0', strcpy(parent, "/");

    if (snprintf(dst, dstlen, "%s%s/cgroupd.slice", CGV2_ROOT, parent) >= (int)dstlen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}



static int enable_controller_in_parent(const char *path, const char *ctrl) {
    char parent[PATH_MAX];
    if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent))
        return -1;
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return -1;
    *slash = '\0';

    char file[PATH_MAX + 32];
    snprintf(file, sizeof(file), "%s/cgroup.subtree_control", parent);

    char buf[64];
    snprintf(buf, sizeof(buf), "+%s", ctrl);
     
    if (write_file(file, buf) < 0) {
        if (errno == EINVAL || errno == EBUSY || errno == EOPNOTSUPP || errno == EPERM) {
            LOGD("enable %s in %s: %s", ctrl, parent, strerror(errno));
            return 0;
        }
        LOGW("enable %s in %s failed: %s", ctrl, parent, strerror(errno));
        return -1;
    }
    LOGD("enabled controller %s in %s", ctrl, parent);
    return 0;
}

int cg_setup_root(const char *root_path) {
    if (mkdir_p(root_path, 0755) < 0) {
        LOGE("mkdir_p %s: %s", root_path, strerror(errno));
        return -1;
    }
     
    static const char *ctrls[] = {"cpu", "memory", "io", "pids", "cpuset", NULL};
    for (int i = 0; ctrls[i]; i++) enable_controller_in_parent(root_path, ctrls[i]);

     
    char file[PATH_MAX + 32];
    snprintf(file, sizeof(file), "%s/cgroup.subtree_control", root_path);
    for (int i = 0; ctrls[i]; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "+%s", ctrls[i]);
        if (write_file(file, buf) < 0)
            LOGD("enable %s in own subtree: %s", ctrls[i], strerror(errno));
    }
    return 0;
}

int cg_create_child(const char *parent, const char *name, char *dst, size_t dstlen) {
    if (snprintf(dst, dstlen, "%s/%s", parent, name) >= (int)dstlen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(dst, 0755) < 0 && errno != EEXIST) {
        LOGE("mkdir %s: %s", dst, strerror(errno));
        return -1;
    }
    return 0;
}

static int write_member(const char *path, const char *member, const char *val) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/%s", path, member);
    if (write_file(file, val) < 0) {
        LOGW("write %s = %s: %s", file, val, strerror(errno));
        return -1;
    }
    return 0;
}

static int write_member_u64(const char *path, const char *member, uint64_t v) {
    char buf[64];
    if (v == UINT64_MAX) snprintf(buf, sizeof(buf), "max");
    else snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return write_member(path, member, buf);
}

int cg_apply_limits(const char *path, const struct cg_limits *lim) {
    if (!lim) return 0;
    int rc = 0;
    if (lim->cpu_max && *lim->cpu_max)
        rc |= write_member(path, "cpu.max", lim->cpu_max);
    if (lim->cpu_weight) {
        char b[16]; snprintf(b, sizeof(b), "%d", lim->cpu_weight);
        rc |= write_member(path, "cpu.weight", b);
    }
    if (lim->cpuset_cpus && *lim->cpuset_cpus)
        rc |= write_member(path, "cpuset.cpus", lim->cpuset_cpus);
    if (lim->cpuset_mems && *lim->cpuset_mems)
        rc |= write_member(path, "cpuset.mems", lim->cpuset_mems);
    if (lim->memory_max)
        rc |= write_member_u64(path, "memory.max", lim->memory_max);
    if (lim->memory_high)
        rc |= write_member_u64(path, "memory.high", lim->memory_high);
    if (lim->memory_low)
        rc |= write_member_u64(path, "memory.low", lim->memory_low);
    if (lim->memory_min)
        rc |= write_member_u64(path, "memory.min", lim->memory_min);
    if (lim->memory_swap_max_set)
        rc |= write_member_u64(path, "memory.swap.max", lim->memory_swap_max);
    if (lim->io_weight) {
        char b[16]; snprintf(b, sizeof(b), "default %d", lim->io_weight);
        rc |= write_member(path, "io.weight", b);
    }
    for (int i = 0; i < lim->io_max_rulec; i++) {
        if (lim->io_max_rules[i] && *lim->io_max_rules[i])
            rc |= write_member(path, "io.max", lim->io_max_rules[i]);
    }
    if (lim->pids_max)
        rc |= write_member_u64(path, "pids.max", lim->pids_max);
    return rc;
}

int cg_attach_pid(const char *path, pid_t pid) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/cgroup.procs", path);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", pid);
    if (write_file(file, buf) < 0) {
        LOGE("attach pid %d to %s: %s", pid, path, strerror(errno));
        return -1;
    }
    return 0;
}

static int kill_all_iter(const char *path) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/cgroup.procs", path);
    char buf[16384];
    ssize_t n = read_file(file, buf, sizeof(buf));
    if (n <= 0) return (n == 0) ? 0 : -1;
    char *p = buf;
    while (p < buf + n) {
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        if (*p) {
            pid_t pid = (pid_t)atoi(p);
            if (pid > 0) kill(pid, SIGKILL);
        }
        p = nl + 1;
    }
    return 0;
}

int cg_kill_all(const char *path) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/cgroup.kill", path);
    if (write_file(file, "1") == 0) return 0;
    if (errno != ENOENT) LOGD("cgroup.kill failed: %s", strerror(errno));
    return kill_all_iter(path);
}

int cg_freeze(const char *path, int freeze) {
    return write_member(path, "cgroup.freeze", freeze ? "1" : "0");
}

uint64_t cg_memory_current(const char *path) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/memory.current", path);
    char buf[64];
    if (read_file(file, buf, sizeof(buf)) < 0) return UINT64_MAX;
    return strtoull(buf, NULL, 10);
}

uint64_t cg_memory_swap_current(const char *path) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/memory.swap.current", path);
    char buf[64];
    if (read_file(file, buf, sizeof(buf)) < 0) return UINT64_MAX;
    return strtoull(buf, NULL, 10);
}

uint64_t cg_pids_current(const char *path) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/pids.current", path);
    char buf[64];
    if (read_file(file, buf, sizeof(buf)) < 0) return UINT64_MAX;
    return strtoull(buf, NULL, 10);
}

int cg_populated(const char *path) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/cgroup.events", path);
    char buf[256];
    if (read_file(file, buf, sizeof(buf)) < 0) return -1;
    char *p = strstr(buf, "populated ");
    if (!p) return -1;
    return atoi(p + strlen("populated "));
}

int cg_remove(const char *path) {
    if (rmdir(path) < 0) {
        LOGD("rmdir %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int cg_open_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) LOGE("open cgroup dir %s: %s", path, strerror(errno));
    return fd;
}

int cg_open_memory_events(const char *path) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/memory.events", path);
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    return fd;
}

int cg_read_memory_events(const char *path, uint64_t *out_oom_kill,
                          uint64_t *out_oom) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/memory.events", path);
    char buf[1024];
    if (read_file(file, buf, sizeof(buf)) < 0) return -1;
    if (out_oom_kill) {
        const char *p = strstr(buf, "oom_kill ");
        *out_oom_kill = p ? strtoull(p + 9, NULL, 10) : 0;
    }
    if (out_oom) {
        const char *p = strstr(buf, "oom ");
         
        if (p && (p == buf || p[-1] == '\n'))
            *out_oom = strtoull(p + 4, NULL, 10);
        else
            *out_oom = 0;
    }
    return 0;
}

int cg_open_cgroup_events(const char *path) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/cgroup.events", path);
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    return fd;
}

int cg_read_io_aggregate(const char *path, uint64_t *out_rbytes,
                         uint64_t *out_wbytes) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/io.stat", path);
    char buf[16384];
    ssize_t n = read_file(file, buf, sizeof(buf));
    if (n < 0) return -1;
    uint64_t r = 0, w = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        const char *p;
        if ((p = strstr(line, "rbytes=")))
            r += strtoull(p + 7, NULL, 10);
        if ((p = strstr(line, "wbytes=")))
            w += strtoull(p + 7, NULL, 10);
        line = nl ? nl + 1 : NULL;
    }
    if (out_rbytes) *out_rbytes = r;
    if (out_wbytes) *out_wbytes = w;
    return 0;
}

uint64_t cg_cpu_usage_usec(const char *path) {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/cpu.stat", path);
    char buf[1024];
    if (read_file(file, buf, sizeof(buf)) < 0) return UINT64_MAX;
    const char *p = strstr(buf, "usage_usec ");
    if (!p) return UINT64_MAX;
    return strtoull(p + 11, NULL, 10);
}
