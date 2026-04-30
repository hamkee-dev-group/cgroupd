#define _GNU_SOURCE
#include "proto.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int connect_sock(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
    return fd;
}

static const char *default_sock(void) {
    static char buf[256];
    const char *env = getenv("CGROUPD_SOCKET");
    if (env) return env;
    uid_t uid = getuid();
    if (uid == 0) return "/run/cgroupd.sock";
    snprintf(buf, sizeof(buf), "/run/user/%u/cgroupd.sock", uid);
    struct stat st;
    if (stat(buf, &st) == 0) return buf;
    snprintf(buf, sizeof(buf), "/tmp/cgroupd-%u.sock", uid);
    return buf;
}

static int read_response_buf(int fd, char **out_buf) {
    size_t cap = 4096, off = 0;
    char *buf = malloc(cap);
    if (!buf) return -1;
    while (1) {
        if (off + 1 >= cap) {
            size_t next = cap * 2;
            char *nb = realloc(buf, next);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
            cap = next;
        }
        ssize_t n = read(fd, buf + off, cap - 1 - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return -1;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
    buf[off] = '\0';
    *out_buf = buf;
    return 0;
}

static int response_status(const char *buf) {
    const char *s = strstr(buf, "STATUS: ");
    if (!s) return 1;
    return (strncmp(s + 8, "ok", 2) == 0) ? 0 : 1;
}

static int read_response(int fd, int print) {
    char *buf = NULL;
    if (read_response_buf(fd, &buf) < 0) return -1;
    if (print) fputs(buf, stdout);
    int rc = response_status(buf);
    free(buf);
    return rc;
}

static int response_field_int(const char *buf, const char *key, int *out) {
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\n%s: ", key);
    if (n < 0 || (size_t)n >= sizeof(needle)) return -1;
    const char *p = strstr(buf, needle);
    if (p) {
        p += strlen(needle);
    } else if (strncmp(buf, key, strlen(key)) == 0 && buf[strlen(key)] == ':') {
        p = buf + strlen(key) + 2;
    } else {
        return -1;
    }
    *out = atoi(p);
    return 0;
}

static int cmd_wait(const char *sock, const char *id) {
    int fd = connect_sock(sock);
    if (fd < 0) { perror("connect"); return 1; }

    char req[512];
    int n = snprintf(req, sizeof(req), "WAIT\nid: %s\n\n", id);
    if (n < 0 || (size_t)n >= sizeof(req)) {
        close(fd);
        fprintf(stderr, "wait request too large\n");
        return 1;
    }
    if (proto_write(fd, req, (size_t)n) < 0) {
        perror("write");
        close(fd);
        return 1;
    }
    shutdown(fd, SHUT_WR);

    char *buf = NULL;
    if (read_response_buf(fd, &buf) < 0) {
        perror("read");
        close(fd);
        return 1;
    }
    close(fd);

    fputs(buf, stdout);
    if (response_status(buf) != 0) {
        free(buf);
        return 1;
    }

    int exit_code = 0;
    int sig = 0;
    int have_exit = (response_field_int(buf, "exit", &exit_code) == 0);
    int have_sig = (response_field_int(buf, "signal", &sig) == 0);
    free(buf);

    if (have_sig && sig > 0)
        return 128 + sig;
    if (have_exit)
        return exit_code & 255;
    return 1;
}

 

struct run_opts {
    const char *id;
    const char *cpu_max;
    int   cpu_weight;
    const char *memory_max;
    const char *memory_high;
    const char *memory_low;
    const char *memory_min;
    const char *memory_swap_max;
    int   io_weight;
    const char *pids_max;
    const char *cpuset_cpus;
    const char *cpuset_mems;
    int   priority;
    const char *cwd;
    char **env;
    int    env_n;
    char **io_max_rules;
    int    io_max_rulec;
    char **require_paths;
    int    require_n;
    char **services;
    int    service_n;
};

static int cmd_run(const char *sock, int argc, char **argv) {
    struct run_opts o = {0};
    o.priority = -1;
    char *envs[64];
    int env_n = 0;
    char *io_max_rules[16];
    int io_max_rulec = 0;
    char *require_paths[16];
    int require_n = 0;
    char *services[16];
    int service_n = 0;

    static struct option opts[] = {
        {"id", required_argument, 0, 'I'},
        {"cpu-max", required_argument, 0, 1001},
        {"cpu-weight", required_argument, 0, 1002},
        {"memory-max", required_argument, 0, 1003},
        {"memory-high", required_argument, 0, 1004},
        {"memory-low", required_argument, 0, 1005},
        {"memory-min", required_argument, 0, 1006},
        {"memory-swap-max", required_argument, 0, 1007},
        {"io-weight", required_argument, 0, 1008},
        {"io-max", required_argument, 0, 1009},
        {"pids-max", required_argument, 0, 1010},
        {"cpuset-cpus", required_argument, 0, 1011},
        {"cpuset-mems", required_argument, 0, 1012},
        {"require-path", required_argument, 0, 1013},
        {"service", required_argument, 0, 1014},
        {"priority", required_argument, 0, 'p'},
        {"cwd", required_argument, 0, 'C'},
        {"env", required_argument, 0, 'e'},
        {0,0,0,0}
    };
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "+I:p:C:e:", opts, NULL)) != -1) {
        switch (c) {
        case 'I': o.id = optarg; break;
        case 1001: o.cpu_max = optarg; break;
        case 1002: o.cpu_weight = atoi(optarg); break;
        case 1003: o.memory_max = optarg; break;
        case 1004: o.memory_high = optarg; break;
        case 1005: o.memory_low = optarg; break;
        case 1006: o.memory_min = optarg; break;
        case 1007: o.memory_swap_max = optarg; break;
        case 1008: o.io_weight = atoi(optarg); break;
        case 1009:
            if (io_max_rulec < 16) io_max_rules[io_max_rulec++] = optarg;
            break;
        case 1010: o.pids_max = optarg; break;
        case 1011: o.cpuset_cpus = optarg; break;
        case 1012: o.cpuset_mems = optarg; break;
        case 1013:
            if (require_n < 16) require_paths[require_n++] = optarg;
            break;
        case 1014:
            if (service_n < 16) services[service_n++] = optarg;
            break;
        case 'p': o.priority = atoi(optarg); break;
        case 'C': o.cwd = optarg; break;
        case 'e':
            if (env_n < 64) envs[env_n++] = optarg;
            break;
        default: return 2;
        }
    }
    o.env = envs; o.env_n = env_n;
    o.io_max_rules = io_max_rules; o.io_max_rulec = io_max_rulec;
    o.require_paths = require_paths; o.require_n = require_n;
    o.services = services; o.service_n = service_n;

    if (optind >= argc) {
        fprintf(stderr, "cgroupctl run: missing argv\n");
        return 2;
    }

    int fd = connect_sock(sock);
    if (fd < 0) { perror("connect"); return 1; }

    char hdr[16384];
    size_t off = 0;
    off += snprintf(hdr + off, sizeof(hdr) - off, "RUN\n");
    if (o.id)              off += snprintf(hdr + off, sizeof(hdr) - off, "id: %s\n", o.id);
    if (o.cpu_max)         off += snprintf(hdr + off, sizeof(hdr) - off, "cpu_max: %s\n", o.cpu_max);
    if (o.cpu_weight)      off += snprintf(hdr + off, sizeof(hdr) - off, "cpu_weight: %d\n", o.cpu_weight);
    if (o.memory_max)      off += snprintf(hdr + off, sizeof(hdr) - off, "memory_max: %s\n", o.memory_max);
    if (o.memory_high)     off += snprintf(hdr + off, sizeof(hdr) - off, "memory_high: %s\n", o.memory_high);
    if (o.memory_low)      off += snprintf(hdr + off, sizeof(hdr) - off, "memory_low: %s\n", o.memory_low);
    if (o.memory_min)      off += snprintf(hdr + off, sizeof(hdr) - off, "memory_min: %s\n", o.memory_min);
    if (o.memory_swap_max) off += snprintf(hdr + off, sizeof(hdr) - off, "memory_swap_max: %s\n", o.memory_swap_max);
    if (o.io_weight)       off += snprintf(hdr + off, sizeof(hdr) - off, "io_weight: %d\n", o.io_weight);
    for (int i = 0; i < o.io_max_rulec; i++)
        off += snprintf(hdr + off, sizeof(hdr) - off, "io_max: %s\n", o.io_max_rules[i]);
    if (o.pids_max)        off += snprintf(hdr + off, sizeof(hdr) - off, "pids_max: %s\n", o.pids_max);
    if (o.cpuset_cpus)     off += snprintf(hdr + off, sizeof(hdr) - off, "cpuset_cpus: %s\n", o.cpuset_cpus);
    if (o.cpuset_mems)     off += snprintf(hdr + off, sizeof(hdr) - off, "cpuset_mems: %s\n", o.cpuset_mems);
    if (o.priority >= 0)   off += snprintf(hdr + off, sizeof(hdr) - off, "priority: %d\n", o.priority);
    if (o.cwd)             off += snprintf(hdr + off, sizeof(hdr) - off, "cwd: %s\n", o.cwd);
    for (int i = 0; i < o.env_n; i++)
        off += snprintf(hdr + off, sizeof(hdr) - off, "env: %s\n", o.env[i]);
    for (int i = 0; i < o.require_n; i++)
        off += snprintf(hdr + off, sizeof(hdr) - off, "require_path: %s\n", o.require_paths[i]);
    for (int i = 0; i < o.service_n; i++)
        off += snprintf(hdr + off, sizeof(hdr) - off, "service: %s\n", o.services[i]);
    for (int i = optind; i < argc; i++)
        off += snprintf(hdr + off, sizeof(hdr) - off, "arg: %s\n", argv[i]);
    off += snprintf(hdr + off, sizeof(hdr) - off, "\n");

    if (proto_write(fd, hdr, off) < 0) { perror("write"); close(fd); return 1; }
    shutdown(fd, SHUT_WR);
    int rc = read_response(fd, 1);
    close(fd);
    return rc;
}

static int simple_cmd(const char *sock, const char *verb, const char *id,
                      int sig) {
    int fd = connect_sock(sock);
    if (fd < 0) { perror("connect"); return 1; }
    char buf[512];
    int n;
    if (id && sig)
        n = snprintf(buf, sizeof(buf), "%s\nid: %s\nsignal: %d\n\n",
                     verb, id, sig);
    else if (id)
        n = snprintf(buf, sizeof(buf), "%s\nid: %s\n\n", verb, id);
    else
        n = snprintf(buf, sizeof(buf), "%s\n\n", verb);
    proto_write(fd, buf, (size_t)n);
    shutdown(fd, SHUT_WR);
    int rc = read_response(fd, 1);
    close(fd);
    return rc;
}

static void usage(void) {
    fprintf(stderr,
"cgroupctl - cgroupd client\n"
"Usage: cgroupctl [--socket PATH] <subcommand> [args]\n"
"\nSubcommands:\n"
"  run --id ID [opts] -- argv...   Submit a job\n"
"  list                            List all jobs\n"
"  kill ID [signal N]              Send signal (default SIGKILL)\n"
"  freeze ID                       Freeze a job's cgroup\n"
"  thaw ID                         Thaw a job's cgroup\n"
"  inspect ID                      Per-job stats + pressure\n"
"  stats                           Host pressure summary\n"
"  remove ID                       Remove an exited job's cgroup\n"
"  wait ID                         Block until the job exits, print exit\n"
"  logs ID [--follow]              Print the captured stdout/stderr\n"
"  ping                            Health check\n"
"  quit                            Ask daemon to shut down\n"
"\nrun options:\n"
"  --cpu-max Q/P (or 'max' or '<q> <p>')\n"
"  --cpu-weight N (1..10000)\n"
"  --memory-max SZ, --memory-high SZ, --memory-low SZ, --memory-min SZ\n"
"  --memory-swap-max SZ\n"
"  --io-weight N --io-max RULE --pids-max N\n"
"  --cpuset-cpus CPUS --cpuset-mems MEMS\n"
"  --priority N (0..100, lower = first to evict)\n"
"  --cwd DIR --env K=V (repeatable)\n"
"  --require-path PATH (repeatable pre-launch existence checks)\n"
"  --service CMD (repeatable shell command started in-job before argv)\n"
"\nEnv:\n"
"  CGROUPD_SOCKET overrides socket path.\n"
);
}

int main(int argc, char **argv) {
    const char *sock = default_sock();
    int i = 1;
    while (i < argc && strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
        sock = argv[i+1];
        i += 2;
    }
    if (i >= argc) { usage(); return 2; }
    const char *cmd = argv[i++];

    if (strcmp(cmd, "run") == 0) {
        return cmd_run(sock, argc - i + 1, argv + i - 1);
    } else if (strcmp(cmd, "list") == 0) {
        return simple_cmd(sock, "LIST", NULL, 0);
    } else if (strcmp(cmd, "kill") == 0) {
        if (i >= argc) { usage(); return 2; }
        const char *id = argv[i++];
        int sig = 0;
        if (i + 1 < argc &&
            (strcmp(argv[i], "signal") == 0 ||
             strcmp(argv[i], "--signal") == 0) ) {
            sig = atoi(argv[i+1]);
        }
        return simple_cmd(sock, "KILL", id, sig);
    } else if (strcmp(cmd, "freeze") == 0) {
        if (i >= argc) { usage(); return 2; }
        return simple_cmd(sock, "FREEZE", argv[i], 0);
    } else if (strcmp(cmd, "thaw") == 0) {
        if (i >= argc) { usage(); return 2; }
        return simple_cmd(sock, "THAW", argv[i], 0);
    } else if (strcmp(cmd, "inspect") == 0) {
        if (i >= argc) { usage(); return 2; }
        return simple_cmd(sock, "INSPECT", argv[i], 0);
    } else if (strcmp(cmd, "stats") == 0) {
        return simple_cmd(sock, "STATS", NULL, 0);
    } else if (strcmp(cmd, "remove") == 0) {
        if (i >= argc) { usage(); return 2; }
        return simple_cmd(sock, "REMOVE", argv[i], 0);
    } else if (strcmp(cmd, "wait") == 0) {
        if (i >= argc) { usage(); return 2; }
        return cmd_wait(sock, argv[i]);
    } else if (strcmp(cmd, "logs") == 0) {
        if (i >= argc) { usage(); return 2; }
        const char *id = argv[i++];
        int follow = 0;
        for (; i < argc; i++)
            if (strcmp(argv[i], "--follow") == 0 || strcmp(argv[i], "-f") == 0)
                follow = 1;
         
        int fd = connect_sock(sock);
        if (fd < 0) { perror("connect"); return 1; }
        char buf[8192];
        int n = snprintf(buf, sizeof(buf), "INSPECT\nid: %s\n\n", id);
        proto_write(fd, buf, (size_t)n);
        shutdown(fd, SHUT_WR);
        char rbuf[16384];
        size_t off = 0;
        ssize_t rr;
        while (off + 1 < sizeof(rbuf) &&
               (rr = read(fd, rbuf + off, sizeof(rbuf) - 1 - off)) > 0)
            off += (size_t)rr;
        close(fd);
        rbuf[off] = '\0';
        const char *p = strstr(rbuf, "\nlog: ");
        if (!p) { fprintf(stderr, "no log path for %s\n", id); return 1; }
        p += 6;
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        char path[4096];
        size_t l = (size_t)(eol - p);
        if (l == 0 || l >= sizeof(path)) {
            fprintf(stderr, "no log captured for %s (start daemon with -L)\n", id);
            return 1;
        }
        memcpy(path, p, l); path[l] = '\0';
        FILE *fp = fopen(path, "r");
        if (!fp) { perror(path); return 1; }
        char line[4096];
        while (fgets(line, sizeof(line), fp)) fputs(line, stdout);
        if (follow) {
            for (;;) {
                if (fgets(line, sizeof(line), fp)) {
                    fputs(line, stdout);
                    fflush(stdout);
                } else {
                    clearerr(fp);
                    struct timespec ts = {0, 200 * 1000 * 1000};
                    nanosleep(&ts, NULL);
                }
            }
        }
        fclose(fp);
        return 0;
    } else if (strcmp(cmd, "ping") == 0) {
        return simple_cmd(sock, "PING", NULL, 0);
    } else if (strcmp(cmd, "quit") == 0) {
        return simple_cmd(sock, "QUIT", NULL, 0);
    } else {
        usage();
        return 2;
    }
}
