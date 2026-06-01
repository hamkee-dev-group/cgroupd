#define _GNU_SOURCE
#include "cgroup.h"
#include "log.h"
#include "proto.h"
#include "psi.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef CLONE_INTO_CGROUP
#define CLONE_INTO_CGROUP 0x200000000ULL
#endif

#ifndef SYS_clone3
#define SYS_clone3 435
#endif

struct clone_args_compat {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

static long sys_clone3(struct clone_args_compat *a, size_t s) {
    return syscall(SYS_clone3, a, s);
}

#define MAX_JOBS  4096
#define MAX_ARGV  64
#define MAX_ENVP  64
#define MAX_SERVICES 8
#define MAX_REQUIRE_PATHS 8

enum job_state {
    JOB_PENDING = 0,
    JOB_RUNNING,
    JOB_FROZEN,
    JOB_EXITED,
    JOB_KILLED,
    JOB_FAILED,
};

 
#define MAX_WAITERS 4

struct job {
    int  used;
    char id[64];
    char cgroup_path[PATH_MAX];
    pid_t pid;
    int   state;
    int   exit_code;           
    int   exited_by_signal;
    int   priority;            
    int   demotion_steps;      
    int   warned;
    int   oom_killed;
    uint64_t start_ms;
    uint64_t end_ms;
    uint64_t start_real_ms;
    uint64_t end_real_ms;
    uint64_t prev_oom_kill;

    struct cg_limits limits;
     
    char cpu_max_buf[64];
    char cpuset_cpus_buf[64];
    char cpuset_mems_buf[32];

    char  cwd[PATH_MAX];
    char *argv[MAX_ARGV];
    int   argc;
    char *envp[MAX_ENVP];
    int   envc;
    char *services[MAX_SERVICES];
    int   servicec;
    char *require_paths[MAX_REQUIRE_PATHS];
    int   requirec;

     
    int   log_fd;              
    char  log_path[PATH_MAX];

     
    int   mem_events_fd;

     
    int   waiters[MAX_WAITERS];
};

struct daemon {
    char root[PATH_MAX];
    char sock_path[PATH_MAX];
    char log_dir[PATH_MAX];    
    int  sock_fd;
    int  sigfd;
    int  timerfd;
    int  epfd;
    int  psi_mem_fd;
    int  psi_cpu_fd;
    int  psi_io_fd;
    int  shutting_down;

     
    uint64_t mem_stall_us;     
    uint64_t mem_window_us;
    uint64_t cpu_stall_us;
    uint64_t cpu_window_us;
    uint64_t io_stall_us;
    uint64_t io_window_us;
    double   mem_avg10_kill;   
    double   mem_admit_some_avg10;
    double   mem_admit_full_avg10;
    double   cpu_admit_some_avg10;
    double   io_admit_full_avg10;

    struct job jobs[MAX_JOBS];
    int next_slot;

    uint64_t last_action_ms;
};

static volatile sig_atomic_t g_stop = 0;

 

static struct job *job_find(struct daemon *d, const char *id) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (d->jobs[i].used && strcmp(d->jobs[i].id, id) == 0)
            return &d->jobs[i];
    }
    return NULL;
}

static struct job *job_find_pid(struct daemon *d, pid_t pid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (d->jobs[i].used && d->jobs[i].pid == pid)
            return &d->jobs[i];
    }
    return NULL;
}

static struct job *job_alloc(struct daemon *d) {
    for (int i = 0; i < MAX_JOBS; i++) {
        int slot = (d->next_slot + i) % MAX_JOBS;
        if (!d->jobs[slot].used) {
            d->next_slot = (slot + 1) % MAX_JOBS;
            memset(&d->jobs[slot], 0, sizeof(d->jobs[slot]));
            d->jobs[slot].used = 1;
            d->jobs[slot].priority = 50;
            d->jobs[slot].log_fd = -1;
            d->jobs[slot].mem_events_fd = -1;
            for (int w = 0; w < MAX_WAITERS; w++)
                d->jobs[slot].waiters[w] = -1;
            return &d->jobs[slot];
        }
    }
    return NULL;
}

static void job_free_strings(struct job *j) {
    for (int i = 0; i < j->argc; i++) { free(j->argv[i]); j->argv[i] = NULL; }
    j->argc = 0;
    for (int i = 0; i < j->envc; i++) { free(j->envp[i]); j->envp[i] = NULL; }
    j->envc = 0;
    for (int i = 0; i < j->servicec; i++) { free(j->services[i]); j->services[i] = NULL; }
    j->servicec = 0;
    for (int i = 0; i < j->requirec; i++) {
        free(j->require_paths[i]);
        j->require_paths[i] = NULL;
    }
    j->requirec = 0;
    for (int i = 0; i < j->limits.io_max_rulec; i++) {
        free((char *)j->limits.io_max_rules[i]);
        j->limits.io_max_rules[i] = NULL;
    }
    j->limits.io_max_rulec = 0;
}

static void event_logf(const char *fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(body)) n = sizeof(body) - 1;
    dprintf(STDERR_FILENO, "EVENT schema=cgroupd.v1 ts_unix_ms=%llu %s\n",
            (unsigned long long)now_real_ms(), body);
}

static void format_job_pids(const struct job *j, char *dst, size_t dstlen,
                            int *out_count) {
    char file[PATH_MAX];
    char raw[8192];
    size_t off = 0;
    int count = 0;

    if (dstlen == 0) return;
    dst[0] = '\0';

    if (!j->cgroup_path[0]) {
        if (out_count) *out_count = 0;
        return;
    }

    snprintf(file, sizeof(file), "%s/cgroup.procs", j->cgroup_path);
    ssize_t n = read_file(file, raw, sizeof(raw));
    if (n <= 0) {
        if (out_count) *out_count = 0;
        return;
    }

    char *p = raw;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (*p) {
            if (count > 0 && off + 1 < dstlen)
                dst[off++] = ',';
            size_t len = strlen(p);
            if (off + len >= dstlen)
                len = dstlen - 1 - off;
            memcpy(dst + off, p, len);
            off += len;
            count++;
        }
        if (!nl) break;
        p = nl + 1;
    }
    dst[off] = '\0';
    if (out_count) *out_count = count;
}

static int appendf(char **buf, size_t *cap, size_t *off, const char *fmt, ...) {
    while (1) {
        if (*off + 128 >= *cap) {
            size_t next = (*cap < 1024) ? 1024 : (*cap * 2);
            char *nb = realloc(*buf, next);
            if (!nb) return -1;
            *buf = nb;
            *cap = next;
        }
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(*buf + *off, *cap - *off, fmt, ap);
        va_end(ap);
        if (n < 0) return -1;
        if ((size_t)n < *cap - *off) {
            *off += (size_t)n;
            return 0;
        }
        size_t next = *cap * 2 + (size_t)n + 1;
        char *nb = realloc(*buf, next);
        if (!nb) return -1;
        *buf = nb;
        *cap = next;
    }
}

static int validate_job_preflight(const struct job *j, char *reason, size_t reason_len) {
    for (int i = 0; i < j->requirec; i++) {
        if (access(j->require_paths[i], F_OK) < 0) {
            snprintf(reason, reason_len, "missing path: %s", j->require_paths[i]);
            return -1;
        }
    }
    for (int i = 0; i < j->servicec; i++) {
        if (strchr(j->services[i], '\n')) {
            snprintf(reason, reason_len, "service contains newline");
            return -1;
        }
    }
    return 0;
}

static int read_pressure_with_fallback(const char *cgroup_path, const char *resource,
                                       struct psi *out) {
    if (psi_read(cgroup_path, resource, out) == 0)
        return 0;
    return psi_read(NULL, resource, out);
}

static int admission_check(struct daemon *d, const struct job *j,
                           char *reason, size_t reason_len) {
    struct psi pmem = {0}, pcpu = {0}, pio = {0};
    int have_mem = (read_pressure_with_fallback(d->root, "memory", &pmem) == 0);
    int have_cpu = (read_pressure_with_fallback(d->root, "cpu", &pcpu) == 0);
    int have_io  = (read_pressure_with_fallback(d->root, "io", &pio) == 0);

    if (have_mem && d->mem_admit_full_avg10 >= 0.0 &&
        pmem.full.avg10 >= d->mem_admit_full_avg10) {
        event_logf("type=pressure resource=memory some_avg10=%.2f full_avg10=%.2f "
                   "action=admission_reject target=%s",
                   pmem.some.avg10, pmem.full.avg10, j->id);
        snprintf(reason, reason_len,
                 "admission blocked: memory.full.avg10 %.2f >= %.2f",
                 pmem.full.avg10, d->mem_admit_full_avg10);
        return -1;
    }
    if (have_mem && d->mem_admit_some_avg10 >= 0.0 &&
        pmem.some.avg10 >= d->mem_admit_some_avg10) {
        event_logf("type=pressure resource=memory some_avg10=%.2f full_avg10=%.2f "
                   "action=admission_reject target=%s",
                   pmem.some.avg10, pmem.full.avg10, j->id);
        snprintf(reason, reason_len,
                 "admission blocked: memory.some.avg10 %.2f >= %.2f",
                 pmem.some.avg10, d->mem_admit_some_avg10);
        return -1;
    }
    if (have_cpu && d->cpu_admit_some_avg10 >= 0.0 &&
        pcpu.some.avg10 >= d->cpu_admit_some_avg10) {
        event_logf("type=pressure resource=cpu some_avg10=%.2f full_avg10=%.2f "
                   "action=admission_reject target=%s",
                   pcpu.some.avg10, pcpu.full.avg10, j->id);
        snprintf(reason, reason_len,
                 "admission blocked: cpu.some.avg10 %.2f >= %.2f",
                 pcpu.some.avg10, d->cpu_admit_some_avg10);
        return -1;
    }
    if (have_io && d->io_admit_full_avg10 >= 0.0 &&
        pio.full.avg10 >= d->io_admit_full_avg10) {
        event_logf("type=pressure resource=io some_avg10=%.2f full_avg10=%.2f "
                   "action=admission_reject target=%s",
                   pio.some.avg10, pio.full.avg10, j->id);
        snprintf(reason, reason_len,
                 "admission blocked: io.full.avg10 %.2f >= %.2f",
                 pio.full.avg10, d->io_admit_full_avg10);
        return -1;
    }
    return 0;
}

static int build_service_wrapper(const struct job *j, char **out_script) {
    char *script = NULL;
    size_t cap = 0, off = 0;

    if (appendf(&script, &cap, &off,
                "svc_pids=\"\"\n"
                "cleanup() {\n"
                "  rc=$?\n"
                "  trap - EXIT INT TERM\n"
                "  if [ -n \"$main_pid\" ]; then kill \"$main_pid\" 2>/dev/null || true; fi\n"
                "  if [ -n \"$svc_pids\" ]; then\n"
                "    kill $svc_pids 2>/dev/null || true\n"
                "    wait $svc_pids 2>/dev/null || true\n"
                "  fi\n"
                "  exit \"$rc\"\n"
                "}\n"
                "trap cleanup EXIT INT TERM\n"
                "main_pid=\"\"\n") < 0)
        goto fail;

    for (int i = 0; i < j->servicec; i++) {
        if (appendf(&script, &cap, &off,
                    "%s &\n"
                    "svc_pids=\"$svc_pids $!\"\n",
                    j->services[i]) < 0)
            goto fail;
    }

    if (appendf(&script, &cap, &off,
                "\"$@\" &\n"
                "main_pid=$!\n"
                "wait \"$main_pid\"\n"
                "rc=$?\n"
                "main_pid=\"\"\n"
                "trap - EXIT INT TERM\n"
                "if [ -n \"$svc_pids\" ]; then\n"
                "  kill $svc_pids 2>/dev/null || true\n"
                "  wait $svc_pids 2>/dev/null || true\n"
                "fi\n"
                "exit \"$rc\"\n") < 0)
        goto fail;

    *out_script = script;
    return 0;
fail:
    free(script);
    return -1;
}

static void job_reset(struct daemon *d, struct job *j) {
    if (!j->used) return;
    if (j->mem_events_fd >= 0) {
        epoll_ctl(d->epfd, EPOLL_CTL_DEL, j->mem_events_fd, NULL);
        close(j->mem_events_fd);
        j->mem_events_fd = -1;
    }
    if (j->log_fd >= 0) {
        close(j->log_fd);
        j->log_fd = -1;
    }
     
    for (int w = 0; w < MAX_WAITERS; w++) {
        if (j->waiters[w] >= 0) {
            proto_write_status(j->waiters[w], "err",
                               "reason: job removed before exit\n");
            close(j->waiters[w]);
            j->waiters[w] = -1;
        }
    }
    j->cgroup_path[0] = '\0';
    job_free_strings(j);
    j->used = 0;
}

static void job_release(struct daemon *d, struct job *j) {
    if (!j->used) return;
    if (j->cgroup_path[0]) {
         
        cg_remove(j->cgroup_path);
    }
    job_reset(d, j);
}

 
static void job_notify_waiters(struct job *j) {
    for (int w = 0; w < MAX_WAITERS; w++) {
        if (j->waiters[w] < 0) continue;
        proto_write_status(j->waiters[w], "ok",
            "id: %s\nstate: %s\nexit: %d\nsignal: %d\noom_killed: %d\n",
            j->id,
            (j->state == JOB_EXITED) ? "exited" :
            (j->state == JOB_KILLED) ? "killed" :
            (j->state == JOB_FAILED) ? "failed" : "?",
            j->exited_by_signal ? 0 : j->exit_code,
            j->exited_by_signal ? j->exit_code : 0,
            j->oom_killed);
        close(j->waiters[w]);
        j->waiters[w] = -1;
    }
}

 

 
static void spawn_rollback(struct daemon *d, struct job *j) {
    if (j->mem_events_fd >= 0) {
        epoll_ctl(d->epfd, EPOLL_CTL_DEL, j->mem_events_fd, NULL);
        close(j->mem_events_fd);
        j->mem_events_fd = -1;
    }
    if (j->log_fd >= 0) {
        close(j->log_fd);
        j->log_fd = -1;
    }
    if (j->cgroup_path[0]) {
        cg_remove(j->cgroup_path);
        j->cgroup_path[0] = '\0';
    }
}

static int spawn_job(struct daemon *d, struct job *j) {
    if (cg_create_child(d->root, j->id, j->cgroup_path,
                        sizeof(j->cgroup_path)) < 0)
        return -1;

    if (cg_apply_limits(j->cgroup_path, &j->limits) < 0)
        LOGW("[%s] some limits failed to apply", j->id);

    

    if (d->log_dir[0]) {
        char dirbuf[PATH_MAX];
        char idbuf[sizeof(j->id)];
        snprintf(dirbuf, sizeof(dirbuf), "%s", d->log_dir);
        snprintf(idbuf,  sizeof(idbuf),  "%s", j->id);
        int n = snprintf(j->log_path, sizeof(j->log_path),
                         "%s/%s.log", dirbuf, idbuf);
        if (n < 0 || (size_t)n >= sizeof(j->log_path)) {
            LOGW("[%s] log dir + id too long; logging disabled", j->id);
            j->log_path[0] = '\0';
        } else {
            j->log_fd = open(j->log_path,
                             O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            if (j->log_fd < 0)
                LOGW("[%s] cannot open log %s: %s",
                     j->id, j->log_path, strerror(errno));
        }
    }

     
    cg_read_memory_events(j->cgroup_path, &j->prev_oom_kill, NULL);
    j->mem_events_fd = cg_open_memory_events(j->cgroup_path);
    if (j->mem_events_fd >= 0) {
        struct epoll_event ev = {0};
        ev.events = EPOLLPRI;
        ev.data.ptr = j;   
        if (epoll_ctl(d->epfd, EPOLL_CTL_ADD, j->mem_events_fd, &ev) < 0) {
            close(j->mem_events_fd);
            j->mem_events_fd = -1;
        }
    }

    int cgfd = cg_open_dir(j->cgroup_path);

    pid_t pid = -1;
    if (cgfd >= 0) {
        struct clone_args_compat ca = {
            .flags = CLONE_INTO_CGROUP,
            .exit_signal = SIGCHLD,
            .cgroup = (uint64_t)cgfd,
        };
        long r = sys_clone3(&ca, sizeof(ca));
        if (r < 0) {
            LOGW("[%s] clone3(CLONE_INTO_CGROUP) failed: %s — falling back",
                 j->id, strerror(errno));
            close(cgfd);
            cgfd = -1;
        } else {
            pid = (pid_t)r;
        }
    }

    if (pid < 0 && cgfd < 0) {
        

        int pfd[2];
        if (pipe2(pfd, O_CLOEXEC) < 0) {
            LOGE("[%s] pipe: %s", j->id, strerror(errno));
            spawn_rollback(d, j);
            return -1;
        }
        pid = fork();
        if (pid < 0) {
            close(pfd[0]); close(pfd[1]);
            LOGE("[%s] fork: %s", j->id, strerror(errno));
            spawn_rollback(d, j);
            return -1;
        }
        if (pid == 0) {
            close(pfd[1]);
            char dummy;
            ssize_t rr;
            while ((rr = read(pfd[0], &dummy, 1)) < 0 && errno == EINTR) {}
            close(pfd[0]);
             
        } else {
            close(pfd[0]);
            if (cg_attach_pid(j->cgroup_path, pid) < 0) {
                LOGE("[%s] attach pid %d failed", j->id, pid);
                close(pfd[1]);
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0);
                spawn_rollback(d, j);
                return -1;
            }
            close(pfd[1]);  
        }
    }

    if (pid < 0) {
         
        LOGE("[%s] no path to spawn child", j->id);
        spawn_rollback(d, j);
        return -1;
    }

    if (pid == 0) {
         
        

        if (j->log_fd >= 0) {
            dup2(j->log_fd, STDOUT_FILENO);
            dup2(j->log_fd, STDERR_FILENO);
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        }
        if (j->cwd[0]) {
            if (chdir(j->cwd) < 0) {
                dprintf(STDERR_FILENO, "cgroupd: chdir %s: %s\n",
                        j->cwd, strerror(errno));
                _exit(127);
            }
        }
         
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        setpgid(0, 0);

        if (j->servicec > 0) {
            char *script = NULL;
            if (build_service_wrapper(j, &script) < 0) {
                dprintf(STDERR_FILENO, "cgroupd: build service wrapper failed\n");
                _exit(127);
            }
            char *shell_argv[MAX_ARGV + 8];
            int ai = 0;
            shell_argv[ai++] = "/bin/sh";
            shell_argv[ai++] = "-ceu";
            shell_argv[ai++] = script;
            shell_argv[ai++] = "cgroupd-services";
            for (int i = 0; i < j->argc && ai < MAX_ARGV + 7; i++)
                shell_argv[ai++] = j->argv[i];
            shell_argv[ai] = NULL;

            if (j->envc > 0) {
                char **env = j->envp;
                execvpe("/bin/sh", shell_argv, env);
            } else {
                execvp("/bin/sh", shell_argv);
            }
        } else if (j->envc > 0) {
            char **env = j->envp;
            execvpe(j->argv[0], j->argv, env);
        } else {
            execvp(j->argv[0], j->argv);
        }
        dprintf(STDERR_FILENO, "cgroupd: exec %s: %s\n",
                j->argv[0], strerror(errno));
        _exit(127);
    }

    if (cgfd >= 0) close(cgfd);

    j->pid = pid;
    j->state = JOB_RUNNING;
    j->start_ms = now_ms();
    j->start_real_ms = now_real_ms();
    LOGI("[%s] spawned pid=%d cgroup=%s", j->id, pid, j->cgroup_path);
    event_logf("type=job.start id=%s pid=%d priority=%d cgroup=%s",
               j->id, j->pid, j->priority, j->cgroup_path);
    return 0;
}

 

static int parse_weight_strict(const char *v, const char *key, int *out,
                               char *reason, size_t reason_len) {
    if (!v || !*v) {
        snprintf(reason, reason_len, "%s: empty", key);
        return -1;
    }
    const char *p = v;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) {
        snprintf(reason, reason_len, "%s: empty", key);
        return -1;
    }
    if (*p == '+' || *p == '-') {
        snprintf(reason, reason_len, "%s: out of range", key);
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long val = strtoul(p, &end, 10);
    if (errno || end == p) {
        snprintf(reason, reason_len, "%s: invalid value", key);
        return -1;
    }
    while (*end == ' ' || *end == '\t') end++;
    if (*end) {
        snprintf(reason, reason_len, "%s: invalid value", key);
        return -1;
    }
    if (val < 1 || val > 10000) {
        snprintf(reason, reason_len, "%s: out of range", key);
        return -1;
    }
    *out = (int)val;
    return 0;
}

static int parse_cpu_max_to_buf(const char *v, char *dst, size_t dstlen,
                                char *reason, size_t reason_len) {
    dst[0] = '\0';
    if (!v) {
        snprintf(reason, reason_len, "cpu_max: empty");
        return -1;
    }
    while (*v == ' ' || *v == '\t') v++;
    if (!*v) {
        snprintf(reason, reason_len, "cpu_max: empty");
        return -1;
    }
    char tmp[128];
    if (snprintf(tmp, sizeof(tmp), "%s", v) >= (int)sizeof(tmp)) {
        snprintf(reason, reason_len, "cpu_max: too long");
        return -1;
    }
    size_t tlen = strlen(tmp);
    while (tlen > 0 && (tmp[tlen-1] == ' ' || tmp[tlen-1] == '\t')) {
        tmp[--tlen] = '\0';
    }
    if (tlen == 0) {
        snprintf(reason, reason_len, "cpu_max: empty");
        return -1;
    }
    if (strcmp(tmp, "max") == 0) {
        snprintf(dst, dstlen, "max");
        return 0;
    }
    char *slash = strchr(tmp, '/');
    char *space = strpbrk(tmp, " \t");
    if (slash && space) {
        snprintf(reason, reason_len, "cpu_max: mixed separators");
        return -1;
    }
    char *sep = slash ? slash : space;
    if (!sep) {
        snprintf(reason, reason_len, "cpu_max: invalid value");
        return -1;
    }
    if (slash && strchr(slash + 1, '/')) {
        snprintf(reason, reason_len, "cpu_max: too many slashes");
        return -1;
    }
    *sep = '\0';
    char *q_str = tmp;
    char *p_str = sep + 1;
    while (*p_str == ' ' || *p_str == '\t') p_str++;
    if (!*q_str) {
        snprintf(reason, reason_len, "cpu_max: missing quota");
        return -1;
    }
    if (!*p_str) {
        snprintf(reason, reason_len, "cpu_max: missing period");
        return -1;
    }
    if (strcmp(q_str, "max") != 0) {
        for (const char *p = q_str; *p; p++) {
            if (!isdigit((unsigned char)*p)) {
                snprintf(reason, reason_len, "cpu_max: invalid quota");
                return -1;
            }
        }
        errno = 0;
        char *end;
        unsigned long long q = strtoull(q_str, &end, 10);
        if (errno || *end || q == 0 || q > (unsigned long long)LLONG_MAX) {
            snprintf(reason, reason_len, "cpu_max: quota out of range");
            return -1;
        }
    }
    for (const char *p = p_str; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            snprintf(reason, reason_len, "cpu_max: invalid period");
            return -1;
        }
    }
    errno = 0;
    char *end;
    unsigned long long pval = strtoull(p_str, &end, 10);
    if (errno || *end || pval == 0 || pval > (unsigned long long)LLONG_MAX) {
        snprintf(reason, reason_len, "cpu_max: invalid period");
        return -1;
    }
    int n = snprintf(dst, dstlen, "%s %s", q_str, p_str);
    if (n < 0 || (size_t)n >= dstlen) {
        snprintf(reason, reason_len, "cpu_max: too long");
        return -1;
    }
    return 0;
}

static int handle_run(struct daemon *d, const char *body, int cli_fd) {
    struct job *j = job_alloc(d);
    if (!j) {
        proto_write_status(cli_fd, "err", "reason: too many jobs\n");
        return -1;
    }
    char scratch[2048];
    const char *cur = body;
    char *k, *v;

    int rc;
    while ((rc = proto_next_header(&cur, scratch, sizeof(scratch), &k, &v)) == 1) {
        if (strcmp(k, "id") == 0) {
            snprintf(j->id, sizeof(j->id), "%s", v);
        } else if (strcmp(k, "cpu_max") == 0) {
            char cm_reason[128];
            if (parse_cpu_max_to_buf(v, j->cpu_max_buf, sizeof(j->cpu_max_buf),
                                     cm_reason, sizeof(cm_reason)) < 0) {
                proto_write_status(cli_fd, "err", "reason: %s\n", cm_reason);
                job_release(d, j);
                return -1;
            }
            j->limits.cpu_max = j->cpu_max_buf;
        } else if (strcmp(k, "cpu_weight") == 0) {
            char wreason[128];
            if (parse_weight_strict(v, "cpu_weight", &j->limits.cpu_weight,
                                    wreason, sizeof(wreason)) < 0) {
                proto_write_status(cli_fd, "err", "reason: %s\n", wreason);
                job_release(d, j);
                return -1;
            }
        } else if (strcmp(k, "cpuset_cpus") == 0) {
            snprintf(j->cpuset_cpus_buf, sizeof(j->cpuset_cpus_buf), "%s", v);
            j->limits.cpuset_cpus = j->cpuset_cpus_buf;
        } else if (strcmp(k, "cpuset_mems") == 0) {
            snprintf(j->cpuset_mems_buf, sizeof(j->cpuset_mems_buf), "%s", v);
            j->limits.cpuset_mems = j->cpuset_mems_buf;
        } else if (strcmp(k, "memory_max") == 0) {
            j->limits.memory_max = parse_size(v);
        } else if (strcmp(k, "memory_high") == 0) {
            j->limits.memory_high = parse_size(v);
        } else if (strcmp(k, "memory_low") == 0) {
            j->limits.memory_low = parse_size(v);
        } else if (strcmp(k, "memory_min") == 0) {
            j->limits.memory_min = parse_size(v);
        } else if (strcmp(k, "memory_swap_max") == 0) {
            j->limits.memory_swap_max = parse_size(v);
            j->limits.memory_swap_max_set = 1;
        } else if (strcmp(k, "io_weight") == 0) {
            char wreason[128];
            if (parse_weight_strict(v, "io_weight", &j->limits.io_weight,
                                    wreason, sizeof(wreason)) < 0) {
                proto_write_status(cli_fd, "err", "reason: %s\n", wreason);
                job_release(d, j);
                return -1;
            }
        } else if (strcmp(k, "io_max") == 0) {
            if (j->limits.io_max_rulec < CG_IO_MAX_RULES)
                j->limits.io_max_rules[j->limits.io_max_rulec++] = strdup(v);
        } else if (strcmp(k, "pids_max") == 0) {
            j->limits.pids_max = parse_size(v);
        } else if (strcmp(k, "priority") == 0) {
            j->priority = atoi(v);
            if (j->priority < 0) j->priority = 0;
            if (j->priority > 100) j->priority = 100;
        } else if (strcmp(k, "cwd") == 0) {
            snprintf(j->cwd, sizeof(j->cwd), "%s", v);
        } else if (strcmp(k, "require_path") == 0) {
            if (j->requirec < MAX_REQUIRE_PATHS)
                j->require_paths[j->requirec++] = strdup(v);
        } else if (strcmp(k, "service") == 0) {
            if (j->servicec < MAX_SERVICES)
                j->services[j->servicec++] = strdup(v);
        } else if (strcmp(k, "arg") == 0) {
            if (j->argc < MAX_ARGV - 1) {
                j->argv[j->argc++] = strdup(v);
                j->argv[j->argc] = NULL;
            }
        } else if (strcmp(k, "env") == 0) {
            if (j->envc < MAX_ENVP - 1) {
                j->envp[j->envc++] = strdup(v);
                j->envp[j->envc] = NULL;
            }
        }
    }
    if (rc < 0) {
        proto_write_status(cli_fd, "err", "reason: malformed headers\n");
        job_release(d, j);
        return -1;
    }

    if (!j->id[0]) {
        snprintf(j->id, sizeof(j->id), "job-%llu",
                 (unsigned long long)now_ms());
    }
    if (job_find(d, j->id) && job_find(d, j->id) != j) {
        proto_write_status(cli_fd, "err", "reason: id exists\nid: %s\n", j->id);
        job_release(d, j);
        return -1;
    }
    if (j->argc == 0) {
        proto_write_status(cli_fd, "err", "reason: no command\n");
        job_release(d, j);
        return -1;
    }
    char reason[512];
    if (validate_job_preflight(j, reason, sizeof(reason)) < 0) {
        proto_write_status(cli_fd, "err", "reason: %s\nid: %s\n", reason, j->id);
        job_release(d, j);
        return -1;
    }
    if (admission_check(d, j, reason, sizeof(reason)) < 0) {
        proto_write_status(cli_fd, "err", "reason: %s\nid: %s\n", reason, j->id);
        job_release(d, j);
        return -1;
    }

    if (spawn_job(d, j) < 0) {
        proto_write_status(cli_fd, "err", "reason: spawn failed\nid: %s\n", j->id);
        job_release(d, j);
        return -1;
    }
    proto_write_status(cli_fd, "ok", "id: %s\npid: %d\ncgroup: %s\n",
                       j->id, j->pid, j->cgroup_path);
    return 0;
}

static const char *state_str(int s) {
    switch (s) {
    case JOB_PENDING: return "pending";
    case JOB_RUNNING: return "running";
    case JOB_FROZEN:  return "frozen";
    case JOB_EXITED:  return "exited";
    case JOB_KILLED:  return "killed";
    case JOB_FAILED:  return "failed";
    default:          return "?";
    }
}

static int handle_list(struct daemon *d, int cli_fd) {
    char body[8192];
    size_t off = 0;
    int n = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        struct job *j = &d->jobs[i];
        if (!j->used) continue;
        uint64_t mc = (j->state == JOB_RUNNING || j->state == JOB_FROZEN)
                      ? cg_memory_current(j->cgroup_path) : 0;
        uint64_t pc = (j->state == JOB_RUNNING || j->state == JOB_FROZEN)
                      ? cg_pids_current(j->cgroup_path) : 0;
        uint64_t age = j->start_ms ? (now_ms() - j->start_ms) : 0;
        if (j->end_ms) age = j->end_ms - j->start_ms;
        int w = snprintf(body + off, sizeof(body) - off,
                "id: %s\nstate: %s\npid: %d\npriority: %d\n"
                "exit: %d\nsignal: %d\nmemory_current: %llu\n"
                "pids_current: %llu\nage_ms: %llu\n"
                "start_unix_ms: %llu\nexit_unix_ms: %llu\ncgroup: %s\n\n",
                j->id, state_str(j->state), j->pid, j->priority,
                j->exited_by_signal ? 0 : j->exit_code,
                j->exited_by_signal ? j->exit_code : 0,
                (unsigned long long)mc, (unsigned long long)pc,
                (unsigned long long)age,
                (unsigned long long)j->start_real_ms,
                (unsigned long long)j->end_real_ms,
                j->cgroup_path);
        if (w < 0 || (size_t)w >= sizeof(body) - off) break;
        off += (size_t)w;
        n++;
    }
    if (proto_write(cli_fd, body, off) < 0) return -1;
    return proto_write_status(cli_fd, "ok", "count: %d\n", n);
}

static int handle_kill(struct daemon *d, const char *body, int cli_fd) {
    char scratch[1024], *k, *v;
    const char *cur = body;
    char id[64] = {0};
    int sig = SIGKILL;
    while (proto_next_header(&cur, scratch, sizeof(scratch), &k, &v) == 1) {
        if (strcmp(k, "id") == 0) snprintf(id, sizeof(id), "%s", v);
        else if (strcmp(k, "signal") == 0) sig = atoi(v);
    }
    struct job *j = job_find(d, id);
    if (!j) return proto_write_status(cli_fd, "err", "reason: no such job\nid: %s\n", id);
    if (sig == SIGKILL) {
        cg_kill_all(j->cgroup_path);
    } else {
        if (j->pid > 0 && kill(-j->pid, sig) < 0 && errno == ESRCH)
            kill(j->pid, sig);
    }
    event_logf("type=job.kill id=%s signal=%d reason=client cgroup=%s",
               j->id, sig, j->cgroup_path);
    return proto_write_status(cli_fd, "ok", "id: %s\nsignal: %d\n", id, sig);
}

static int handle_freeze(struct daemon *d, const char *body, int cli_fd, int freeze) {
    char scratch[256], *k, *v;
    const char *cur = body;
    char id[64] = {0};
    while (proto_next_header(&cur, scratch, sizeof(scratch), &k, &v) == 1) {
        if (strcmp(k, "id") == 0) snprintf(id, sizeof(id), "%s", v);
    }
    struct job *j = job_find(d, id);
    if (!j) return proto_write_status(cli_fd, "err", "reason: no such job\n");
    if (cg_freeze(j->cgroup_path, freeze) < 0)
        return proto_write_status(cli_fd, "err", "reason: freeze failed\n");
    j->state = freeze ? JOB_FROZEN : JOB_RUNNING;
    event_logf("type=job.%s id=%s reason=client cgroup=%s",
               freeze ? "freeze" : "thaw", j->id, j->cgroup_path);
    return proto_write_status(cli_fd, "ok", "id: %s\nstate: %s\n", id, state_str(j->state));
}

static int handle_inspect(struct daemon *d, const char *body, int cli_fd) {
    char scratch[256], *k, *v;
    const char *cur = body;
    char id[64] = {0};
    while (proto_next_header(&cur, scratch, sizeof(scratch), &k, &v) == 1) {
        if (strcmp(k, "id") == 0) snprintf(id, sizeof(id), "%s", v);
    }
    struct job *j = job_find(d, id);
    if (!j) return proto_write_status(cli_fd, "err", "reason: no such job\n");

    struct psi pmem = {0}, pcpu = {0}, pio = {0};
    psi_read(j->cgroup_path, "memory", &pmem);
    psi_read(j->cgroup_path, "cpu",    &pcpu);
    psi_read(j->cgroup_path, "io",     &pio);

    uint64_t mc = cg_memory_current(j->cgroup_path);
    uint64_t sc = cg_memory_swap_current(j->cgroup_path);
    uint64_t pc = cg_pids_current(j->cgroup_path);
    uint64_t cu = cg_cpu_usage_usec(j->cgroup_path);
    uint64_t rb = 0, wb = 0;
    cg_read_io_aggregate(j->cgroup_path, &rb, &wb);
    uint64_t oom_kill = 0;
    cg_read_memory_events(j->cgroup_path, &oom_kill, NULL);
    uint64_t age = j->start_ms ? (now_ms() - j->start_ms) : 0;
    if (j->end_ms) age = j->end_ms - j->start_ms;
    char pid_list[8192];
    int pid_count = 0;
    format_job_pids(j, pid_list, sizeof(pid_list), &pid_count);

    return proto_write_status(cli_fd, "ok",
        "id: %s\nstate: %s\npid: %d\npriority: %d\n"
        "exit: %d\nsignal: %d\noom_killed: %d\n"
        "service_count: %d\nrequire_path_count: %d\nio_max_rule_count: %d\n"
        "memory_current: %llu\nmemory_swap_current: %llu\npids_current: %llu\n"
        "cpu_usage_usec: %llu\nio_rbytes: %llu\nio_wbytes: %llu\n"
        "oom_kill_count: %llu\n"
        "age_ms: %llu\n"
        "start_mono_ms: %llu\nexit_mono_ms: %llu\n"
        "start_unix_ms: %llu\nexit_unix_ms: %llu\n"
        "pid_count: %d\npids: %s\n"
        "cgroup: %s\nlog: %s\n"
        "psi_mem_some_avg10: %.2f\npsi_mem_full_avg10: %.2f\n"
        "psi_cpu_some_avg10: %.2f\npsi_io_some_avg10: %.2f\n"
        "psi_io_full_avg10: %.2f\n",
        j->id, state_str(j->state), j->pid, j->priority,
        j->exited_by_signal ? 0 : j->exit_code,
        j->exited_by_signal ? j->exit_code : 0,
        j->oom_killed,
        j->servicec, j->requirec, j->limits.io_max_rulec,
        (unsigned long long)mc, (unsigned long long)sc, (unsigned long long)pc,
        (unsigned long long)cu, (unsigned long long)rb, (unsigned long long)wb,
        (unsigned long long)oom_kill,
        (unsigned long long)age,
        (unsigned long long)j->start_ms,
        (unsigned long long)j->end_ms,
        (unsigned long long)j->start_real_ms,
        (unsigned long long)j->end_real_ms,
        pid_count, pid_list[0] ? pid_list : "",
        j->cgroup_path,
        j->log_path[0] ? j->log_path : "",
        pmem.some.avg10, pmem.full.avg10,
        pcpu.some.avg10, pio.some.avg10, pio.full.avg10);
}

static int handle_stats(struct daemon *d, int cli_fd) {
    struct psi pmem = {0}, pcpu = {0}, pio = {0};
    psi_read(d->root, "memory", &pmem);
    psi_read(d->root, "cpu",    &pcpu);
    psi_read(d->root, "io",     &pio);

    int njobs = 0, nrun = 0, nfrozen = 0;
    uint64_t total_mem = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        struct job *j = &d->jobs[i];
        if (!j->used) continue;
        njobs++;
        if (j->state == JOB_RUNNING) nrun++;
        if (j->state == JOB_FROZEN)  nfrozen++;
        if (j->state == JOB_RUNNING || j->state == JOB_FROZEN) {
            uint64_t mc = cg_memory_current(j->cgroup_path);
            if (mc != UINT64_MAX) total_mem += mc;
        }
    }

    return proto_write_status(cli_fd, "ok",
        "root: %s\njobs: %d\nrunning: %d\nfrozen: %d\n"
        "memory_total: %llu\n"
        "psi_mem_some_avg10: %.2f\npsi_mem_full_avg10: %.2f\n"
        "psi_cpu_some_avg10: %.2f\npsi_io_some_avg10: %.2f\n"
        "psi_io_full_avg10: %.2f\n",
        d->root, njobs, nrun, nfrozen,
        (unsigned long long)total_mem,
        pmem.some.avg10, pmem.full.avg10,
        pcpu.some.avg10, pio.some.avg10, pio.full.avg10);
}

static int handle_wait(struct daemon *d, const char *body, int cli_fd,
                       int *out_keep_open) {
    char scratch[256], *k, *v;
    const char *cur = body;
    char id[64] = {0};
    while (proto_next_header(&cur, scratch, sizeof(scratch), &k, &v) == 1) {
        if (strcmp(k, "id") == 0) snprintf(id, sizeof(id), "%s", v);
    }
    struct job *j = job_find(d, id);
    if (!j) return proto_write_status(cli_fd, "err",
                                      "reason: no such job\nid: %s\n", id);
     
    if (j->state == JOB_EXITED || j->state == JOB_KILLED ||
        j->state == JOB_FAILED) {
        return proto_write_status(cli_fd, "ok",
            "id: %s\nstate: %s\nexit: %d\nsignal: %d\noom_killed: %d\n",
            j->id, state_str(j->state),
            j->exited_by_signal ? 0 : j->exit_code,
            j->exited_by_signal ? j->exit_code : 0,
            j->oom_killed);
    }
     
    for (int w = 0; w < MAX_WAITERS; w++) {
        if (j->waiters[w] < 0) {
            j->waiters[w] = cli_fd;
            *out_keep_open = 1;
            return 0;
        }
    }
    return proto_write_status(cli_fd, "err", "reason: too many waiters\n");
}

static int handle_remove(struct daemon *d, const char *body, int cli_fd) {
    char scratch[256], *k, *v;
    const char *cur = body;
    char id[64] = {0};
    while (proto_next_header(&cur, scratch, sizeof(scratch), &k, &v) == 1) {
        if (strcmp(k, "id") == 0) snprintf(id, sizeof(id), "%s", v);
    }
    struct job *j = job_find(d, id);
    if (!j) return proto_write_status(cli_fd, "err", "reason: no such job\n");
    if (j->state == JOB_RUNNING || j->state == JOB_FROZEN)
        return proto_write_status(cli_fd, "err", "reason: still running\n");
    if (j->cgroup_path[0]) {
        int populated = cg_populated(j->cgroup_path);
        if (populated > 0)
            return proto_write_status(cli_fd, "err", "reason: cgroup still populated\n");
        if (cg_remove(j->cgroup_path) < 0 && errno != ENOENT)
            return proto_write_status(cli_fd, "err", "reason: remove failed\n");
    }
    event_logf("type=job.cleanup id=%s result=removed cgroup=%s",
               j->id, j->cgroup_path);
    job_reset(d, j);
    return proto_write_status(cli_fd, "ok", "id: %s\n", id);
}

 
static int handle_client(struct daemon *d, int cli_fd) {
    char *buf = NULL;
    size_t len = 0;
    int r = proto_read_msg(cli_fd, &buf, &len);
    if (r != 0) {
        if (buf) free(buf);
        return 0;
    }
    char verb[32];
    const char *body_start = proto_verb(buf, verb, sizeof(verb));
    if (!body_start) {
        proto_write_status(cli_fd, "err", "reason: missing verb\n");
        free(buf);
        return 0;
    }

    int keep_open = 0;
    if (strcmp(verb, "RUN") == 0)         handle_run(d, body_start, cli_fd);
    else if (strcmp(verb, "LIST") == 0)   handle_list(d, cli_fd);
    else if (strcmp(verb, "KILL") == 0)   handle_kill(d, body_start, cli_fd);
    else if (strcmp(verb, "FREEZE") == 0) handle_freeze(d, body_start, cli_fd, 1);
    else if (strcmp(verb, "THAW") == 0)   handle_freeze(d, body_start, cli_fd, 0);
    else if (strcmp(verb, "INSPECT") == 0)handle_inspect(d, body_start, cli_fd);
    else if (strcmp(verb, "STATS") == 0)  handle_stats(d, cli_fd);
    else if (strcmp(verb, "REMOVE") == 0) handle_remove(d, body_start, cli_fd);
    else if (strcmp(verb, "WAIT") == 0)   handle_wait(d, body_start, cli_fd, &keep_open);
    else if (strcmp(verb, "PING") == 0)   proto_write_status(cli_fd, "ok", "pong: 1\n");
    else if (strcmp(verb, "QUIT") == 0) {
        proto_write_status(cli_fd, "ok", "shutting_down: 1\n");
        d->shutting_down = 1;
        g_stop = 1;
    }
    else proto_write_status(cli_fd, "err", "reason: unknown verb\nverb: %s\n", verb);

    free(buf);
    return keep_open;
}

 

static void reap_children(struct daemon *d) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct job *j = job_find_pid(d, pid);
        if (!j) {
            LOGD("reaped unknown pid %d", pid);
            continue;
        }
        if (WIFEXITED(status)) {
            j->state = (WEXITSTATUS(status) == 0) ? JOB_EXITED : JOB_FAILED;
            j->exit_code = WEXITSTATUS(status);
            j->exited_by_signal = 0;
        } else if (WIFSIGNALED(status)) {
            j->state = (WTERMSIG(status) == SIGKILL) ? JOB_KILLED : JOB_FAILED;
            j->exit_code = WTERMSIG(status);
            j->exited_by_signal = 1;
        }
        j->end_ms = now_ms();
        j->end_real_ms = now_real_ms();

        

        int had_oom = j->oom_killed;
        uint64_t oom_now = 0;
        if (cg_read_memory_events(j->cgroup_path, &oom_now, NULL) == 0 &&
            oom_now > j->prev_oom_kill) {
            j->oom_killed = 1;
            if (j->state != JOB_KILLED) j->state = JOB_KILLED;
        }
        if (j->oom_killed && !had_oom) {
            event_logf("type=job.oom id=%s cgroup=%s oom_kill_count=%llu",
                       j->id, j->cgroup_path,
                       (unsigned long long)oom_now);
        }

         
        if (j->mem_events_fd >= 0) {
            epoll_ctl(d->epfd, EPOLL_CTL_DEL, j->mem_events_fd, NULL);
            close(j->mem_events_fd);
            j->mem_events_fd = -1;
        }
         
        if (j->log_fd >= 0) {
            close(j->log_fd);
            j->log_fd = -1;
        }

        LOGI("[%s] reaped pid=%d state=%s code=%d sig=%d oom=%d",
             j->id, pid, state_str(j->state),
             j->exited_by_signal ? 0 : j->exit_code,
             j->exited_by_signal ? j->exit_code : 0,
             j->oom_killed);
        event_logf("type=job.exit id=%s state=%s exit=%d signal=%d oom_killed=%d "
                   "cgroup=%s start_unix_ms=%llu exit_unix_ms=%llu",
                   j->id, state_str(j->state),
                   j->exited_by_signal ? 0 : j->exit_code,
                   j->exited_by_signal ? j->exit_code : 0,
                   j->oom_killed, j->cgroup_path,
                   (unsigned long long)j->start_real_ms,
                   (unsigned long long)j->end_real_ms);

        job_notify_waiters(j);
        

    }
}

 

static struct job *pick_victim_high_mem(struct daemon *d, int max_priority) {
    struct job *best = NULL;
    uint64_t best_mem = 0;
    int best_prio = INT_MAX;
    for (int i = 0; i < MAX_JOBS; i++) {
        struct job *j = &d->jobs[i];
        if (!j->used) continue;
        if (j->state != JOB_RUNNING && j->state != JOB_FROZEN) continue;
        if (j->priority > max_priority) continue;
        uint64_t mc = cg_memory_current(j->cgroup_path);
        if (mc == UINT64_MAX) mc = 0;
         
        if (j->priority < best_prio ||
            (j->priority == best_prio && mc > best_mem)) {
            best = j;
            best_prio = j->priority;
            best_mem = mc;
        }
    }
    return best;
}

static void demote_cpu_weight_low_prio(struct daemon *d, int max_priority) {
     
    for (int i = 0; i < MAX_JOBS; i++) {
        struct job *j = &d->jobs[i];
        if (!j->used || j->state != JOB_RUNNING) continue;
        if (j->priority > max_priority) continue;
        int cur = j->limits.cpu_weight ? j->limits.cpu_weight : 100;
        int next = cur / 2;
        if (next < 1) next = 1;
        if (next == cur) continue;
        char b[16]; snprintf(b, sizeof(b), "%d", next);
        char file[PATH_MAX];
        snprintf(file, sizeof(file), "%s/cpu.weight", j->cgroup_path);
        if (write_file(file, b) == 0) {
            j->limits.cpu_weight = next;
            j->demotion_steps++;
            LOGW("[%s] demoted cpu.weight -> %d under cpu pressure",
                 j->id, next);
            event_logf("type=job.throttle id=%s resource=cpu cpu_weight=%d "
                       "reason=pressure cgroup=%s",
                       j->id, next, j->cgroup_path);
        }
    }
}

static void backpressure_react(struct daemon *d, const char *resource) {
     
    struct psi p;
    if (psi_read(d->root, resource, &p) < 0) return;

    uint64_t now = now_ms();
    if (now - d->last_action_ms < 1500) return;   

    if (strcmp(resource, "memory") == 0) {
        if (p.full.avg10 >= d->mem_avg10_kill) {
            struct job *v = pick_victim_high_mem(d, 100);
            if (v) {
                LOGW("memory.full.avg10=%.2f -> KILL victim [%s] prio=%d",
                     p.full.avg10, v->id, v->priority);
                event_logf("type=pressure resource=memory some_avg10=%.2f "
                           "full_avg10=%.2f action=kill target=%s",
                           p.some.avg10, p.full.avg10, v->id);
                cg_kill_all(v->cgroup_path);
                v->state = JOB_KILLED;
                event_logf("type=job.kill id=%s signal=%d reason=pressure_memory "
                           "cgroup=%s",
                           v->id, SIGKILL, v->cgroup_path);
                d->last_action_ms = now;
                return;
            }
        }
        if (p.some.avg10 >= 20.0) {
             
            struct job *v = pick_victim_high_mem(d, 30);
            if (v && v->state == JOB_RUNNING) {
                LOGW("memory.some.avg10=%.2f -> FREEZE [%s] prio=%d",
                     p.some.avg10, v->id, v->priority);
                event_logf("type=pressure resource=memory some_avg10=%.2f "
                           "full_avg10=%.2f action=freeze target=%s",
                           p.some.avg10, p.full.avg10, v->id);
                if (cg_freeze(v->cgroup_path, 1) == 0) {
                    v->state = JOB_FROZEN;
                    event_logf("type=job.freeze id=%s reason=pressure_memory "
                               "cgroup=%s",
                               v->id, v->cgroup_path);
                    d->last_action_ms = now;
                }
            }
        }
    } else if (strcmp(resource, "cpu") == 0) {
        if (p.some.avg10 >= 50.0) {
            LOGW("cpu.some.avg10=%.2f -> demote low-prio cpu.weight",
                 p.some.avg10);
            event_logf("type=pressure resource=cpu some_avg10=%.2f "
                       "full_avg10=%.2f action=demote",
                       p.some.avg10, p.full.avg10);
            demote_cpu_weight_low_prio(d, 30);
            d->last_action_ms = now;
        }
    } else if (strcmp(resource, "io") == 0) {
        if (p.full.avg10 >= 30.0) {
            struct job *v = pick_victim_high_mem(d, 30);
            if (v && v->state == JOB_RUNNING) {
                LOGW("io.full.avg10=%.2f -> FREEZE [%s] prio=%d",
                     p.full.avg10, v->id, v->priority);
                event_logf("type=pressure resource=io some_avg10=%.2f "
                           "full_avg10=%.2f action=freeze target=%s",
                           p.some.avg10, p.full.avg10, v->id);
                if (cg_freeze(v->cgroup_path, 1) == 0) {
                    v->state = JOB_FROZEN;
                    event_logf("type=job.freeze id=%s reason=pressure_io "
                               "cgroup=%s",
                               v->id, v->cgroup_path);
                    d->last_action_ms = now;
                }
            }
        }
    }
}



static void tick(struct daemon *d) {
    struct psi pmem = {0}, pcpu = {0}, pio = {0};
    psi_read(d->root, "memory", &pmem);
    psi_read(d->root, "cpu",    &pcpu);
    psi_read(d->root, "io",     &pio);

    

    if (pmem.full.avg10 >= 1.0 || pmem.some.avg10 >= 1.0)
        backpressure_react(d, "memory");
    if (pcpu.some.avg10 >= 30.0)
        backpressure_react(d, "cpu");
    if (pio.full.avg10 >= 5.0 || pio.some.avg10 >= 10.0)
        backpressure_react(d, "io");

     
    if (pmem.full.avg60 < 1.0 && pmem.some.avg60 < 5.0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            struct job *j = &d->jobs[i];
            if (!j->used || j->state != JOB_FROZEN) continue;
            if (cg_freeze(j->cgroup_path, 0) == 0) {
                LOGI("[%s] thawed (mem pressure receded)", j->id);
                j->state = JOB_RUNNING;
                event_logf("type=job.thaw id=%s reason=pressure_receded "
                           "cgroup=%s",
                           j->id, j->cgroup_path);
            }
        }
    }
}

 

static int setup_signalfd(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) return -1;
    return signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

static int setup_listen(const char *path) {
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, strlen(path));
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0660);
    if (listen(fd, 64) < 0) { close(fd); return -1; }
    return fd;
}

static int setup_timer(void) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) return -1;
    struct itimerspec it = {0};
    it.it_value.tv_sec = 1;
    it.it_interval.tv_sec = 1;
    if (timerfd_settime(fd, 0, &it, NULL) < 0) { close(fd); return -1; }
    return fd;
}

static void epoll_add(int epfd, int fd, uint32_t events, void *tag) {
    struct epoll_event ev = {0};
    ev.events = events;
    ev.data.ptr = tag;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

 
static int TAG_SOCK, TAG_SIG, TAG_TIMER, TAG_PSI_MEM, TAG_PSI_CPU, TAG_PSI_IO;

static void usage(void) {
    fprintf(stderr,
"cgroupd - host-local cgroup v2 + PSI job runner\n"
"Usage: cgroupd [options]\n"
"  -r, --root PATH         cgroup root (default: auto-detect under self)\n"
"  -s, --socket PATH       UNIX socket (default: /run/cgroupd.sock)\n"
"  -d, --debug             enable debug logging\n"
"  -k, --mem-kill-avg10 X  memory.full.avg10 above which we KILL a victim (default 60.0)\n"
"      --mem-admit-some-avg10 X  reject new jobs at/above this memory.some.avg10 (default 80.0)\n"
"      --mem-admit-full-avg10 X  reject new jobs at/above this memory.full.avg10 (default 15.0)\n"
"      --cpu-admit-some-avg10 X  reject new jobs at/above this cpu.some.avg10 (default 95.0)\n"
"      --io-admit-full-avg10 X   reject new jobs at/above this io.full.avg10 (default 80.0)\n"
"  -L, --log-dir PATH      capture per-job stdout/stderr to <dir>/<id>.log\n"
"  -h, --help              show this help\n"
);
}

int main(int argc, char **argv) {
    static struct daemon d;
    memset(&d, 0, sizeof(d));
    d.mem_stall_us  = 150000;      
    d.mem_window_us = 1000000;     
    d.cpu_stall_us  = 200000;
    d.cpu_window_us = 1000000;
    d.io_stall_us   = 200000;
    d.io_window_us  = 1000000;
    d.mem_avg10_kill = 60.0;
    d.mem_admit_some_avg10 = 80.0;
    d.mem_admit_full_avg10 = 15.0;
    d.cpu_admit_some_avg10 = 95.0;
    d.io_admit_full_avg10 = 80.0;

    const char *root_hint = NULL;
    const char *sock_hint = NULL;

    const char *log_dir_hint = NULL;
    static struct option opts[] = {
        {"root", required_argument, 0, 'r'},
        {"socket", required_argument, 0, 's'},
        {"debug", no_argument, 0, 'd'},
        {"mem-kill-avg10", required_argument, 0, 'k'},
        {"mem-admit-some-avg10", required_argument, 0, 1001},
        {"mem-admit-full-avg10", required_argument, 0, 1002},
        {"cpu-admit-some-avg10", required_argument, 0, 1003},
        {"io-admit-full-avg10", required_argument, 0, 1004},
        {"log-dir", required_argument, 0, 'L'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "r:s:dk:L:h", opts, NULL)) != -1) {
        switch (c) {
        case 'r': root_hint = optarg; break;
        case 's': sock_hint = optarg; break;
        case 'd': g_log_level = LOG_DEBUG_; break;
        case 'k': d.mem_avg10_kill = atof(optarg); break;
        case 1001: d.mem_admit_some_avg10 = atof(optarg); break;
        case 1002: d.mem_admit_full_avg10 = atof(optarg); break;
        case 1003: d.cpu_admit_some_avg10 = atof(optarg); break;
        case 1004: d.io_admit_full_avg10 = atof(optarg); break;
        case 'L': log_dir_hint = optarg; break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }

    if (cg_resolve_root(root_hint, d.root, sizeof(d.root)) < 0) {
        LOGE("cannot resolve cgroup root");
        return 1;
    }
    LOGI("using cgroup root: %s", d.root);
    if (cg_setup_root(d.root) < 0) {
        LOGE("setup root failed");
        return 1;
    }

    if (sock_hint) {
        snprintf(d.sock_path, sizeof(d.sock_path), "%s", sock_hint);
    } else {
        uid_t uid = getuid();
        if (uid == 0) snprintf(d.sock_path, sizeof(d.sock_path), "%s",
                                CGROUPD_DEFAULT_SOCK);
        else {
            snprintf(d.sock_path, sizeof(d.sock_path),
                     "/run/user/%u/cgroupd.sock", uid);
             
            struct stat st;
            char dir[PATH_MAX];
            snprintf(dir, sizeof(dir), "/run/user/%u", uid);
            if (stat(dir, &st) < 0)
                snprintf(d.sock_path, sizeof(d.sock_path),
                         "/tmp/cgroupd-%u.sock", uid);
        }
    }
    LOGI("listening on: %s", d.sock_path);

    if (log_dir_hint) {
        snprintf(d.log_dir, sizeof(d.log_dir), "%s", log_dir_hint);
        if (mkdir_p(d.log_dir, 0755) < 0)
            LOGW("mkdir log dir %s: %s", d.log_dir, strerror(errno));
        else
            LOGI("per-job logs in: %s", d.log_dir);
    }

    d.sock_fd = setup_listen(d.sock_path);
    if (d.sock_fd < 0) { LOGE("listen: %s", strerror(errno)); return 1; }

    d.sigfd = setup_signalfd();
    if (d.sigfd < 0) { LOGE("signalfd: %s", strerror(errno)); return 1; }

    d.timerfd = setup_timer();
    if (d.timerfd < 0) { LOGE("timerfd: %s", strerror(errno)); return 1; }

    d.psi_mem_fd = psi_watch(d.root, "memory", "some",
                             d.mem_stall_us, d.mem_window_us);
    d.psi_cpu_fd = psi_watch(d.root, "cpu", "some",
                             d.cpu_stall_us, d.cpu_window_us);
    d.psi_io_fd  = psi_watch(d.root, "io", "some",
                             d.io_stall_us, d.io_window_us);

    d.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (d.epfd < 0) { LOGE("epoll: %s", strerror(errno)); return 1; }

    epoll_add(d.epfd, d.sock_fd,  EPOLLIN, &TAG_SOCK);
    epoll_add(d.epfd, d.sigfd,    EPOLLIN, &TAG_SIG);
    epoll_add(d.epfd, d.timerfd,  EPOLLIN, &TAG_TIMER);
    if (d.psi_mem_fd >= 0) epoll_add(d.epfd, d.psi_mem_fd, EPOLLPRI, &TAG_PSI_MEM);
    if (d.psi_cpu_fd >= 0) epoll_add(d.epfd, d.psi_cpu_fd, EPOLLPRI, &TAG_PSI_CPU);
    if (d.psi_io_fd  >= 0) epoll_add(d.epfd, d.psi_io_fd,  EPOLLPRI, &TAG_PSI_IO);

    LOGI("cgroupd ready");

    while (!g_stop) {
        struct epoll_event ev[16];
        int n = epoll_wait(d.epfd, ev, 16, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("epoll_wait: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            void *tag = ev[i].data.ptr;
            if (tag == &TAG_SOCK) {
                while (1) {
                    int cli = accept4(d.sock_fd, NULL, NULL, SOCK_CLOEXEC);
                    if (cli < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        LOGW("accept: %s", strerror(errno));
                        break;
                    }
                    int keep = handle_client(&d, cli);
                    if (!keep) close(cli);
                }
            } else if (tag == &TAG_SIG) {
                struct signalfd_siginfo si;
                while (read(d.sigfd, &si, sizeof(si)) == sizeof(si)) {
                    if (si.ssi_signo == SIGCHLD) reap_children(&d);
                    else if (si.ssi_signo == SIGTERM ||
                             si.ssi_signo == SIGINT) {
                        LOGI("received signal %u, shutting down", si.ssi_signo);
                        g_stop = 1;
                    }
                }
            } else if (tag == &TAG_TIMER) {
                uint64_t expirations;
                while (read(d.timerfd, &expirations, sizeof(expirations)) > 0) {}
                tick(&d);
            } else if (tag == &TAG_PSI_MEM) {
                LOGW("PSI memory threshold tripped");
                backpressure_react(&d, "memory");
            } else if (tag == &TAG_PSI_CPU) {
                LOGW("PSI cpu threshold tripped");
                backpressure_react(&d, "cpu");
            } else if (tag == &TAG_PSI_IO) {
                LOGW("PSI io threshold tripped");
                backpressure_react(&d, "io");
            } else {
                 
                struct job *j = (struct job *)tag;
                 
                if ((char *)j >= (char *)&d.jobs[0] &&
                    (char *)j <  (char *)&d.jobs[MAX_JOBS] &&
                    j->used && j->mem_events_fd >= 0) {
                    uint64_t oom_now = 0;
                    if (cg_read_memory_events(j->cgroup_path, &oom_now, NULL) == 0) {
                        if (oom_now > j->prev_oom_kill) {
                            LOGW("[%s] memory.events oom_kill: %llu -> %llu",
                                 j->id,
                                 (unsigned long long)j->prev_oom_kill,
                                 (unsigned long long)oom_now);
                            j->oom_killed = 1;
                            j->prev_oom_kill = oom_now;
                            event_logf("type=job.oom id=%s cgroup=%s "
                                       "oom_kill_count=%llu",
                                       j->id, j->cgroup_path,
                                       (unsigned long long)oom_now);
                        }
                    }
                }
            }
        }
    }

    LOGI("shutdown: killing remaining jobs");
    for (int i = 0; i < MAX_JOBS; i++) {
        struct job *j = &d.jobs[i];
        if (!j->used) continue;
        if (j->state == JOB_RUNNING || j->state == JOB_FROZEN) {
            cg_freeze(j->cgroup_path, 0);
            cg_kill_all(j->cgroup_path);
        }
    }
     
    for (int i = 0; i < 50; i++) {
        int status;
        if (waitpid(-1, &status, WNOHANG) <= 0) {
            struct timespec ts = {0, 50 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    for (int i = 0; i < MAX_JOBS; i++) {
        if (d.jobs[i].used) job_release(&d, &d.jobs[i]);
    }
    unlink(d.sock_path);
    LOGI("bye");
    return 0;
}
