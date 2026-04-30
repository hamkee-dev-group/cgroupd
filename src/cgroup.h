#ifndef CGROUPD_CGROUP_H
#define CGROUPD_CGROUP_H

#include <stdint.h>
#include <sys/types.h>

#define CGV2_ROOT "/sys/fs/cgroup"
#define CG_IO_MAX_RULES 8

 
struct cg_limits {
     
    const char *cpu_max;
     
    int cpu_weight;
     
    const char *cpuset_cpus;
     
    const char *cpuset_mems;
     
    uint64_t memory_max;
    uint64_t memory_high;
    uint64_t memory_low;
    uint64_t memory_min;
    

    uint64_t memory_swap_max;
    int      memory_swap_max_set;
     
    int io_weight;
     
    const char *io_max_rules[CG_IO_MAX_RULES];
    int io_max_rulec;
     
    uint64_t pids_max;
};





int cg_resolve_root(const char *root_hint, char *dst, size_t dstlen);



int cg_setup_root(const char *root_path);



int cg_create_child(const char *parent, const char *name, char *dst, size_t dstlen);



int cg_apply_limits(const char *path, const struct cg_limits *lim);

 
int cg_attach_pid(const char *path, pid_t pid);



int cg_kill_all(const char *path);

 
int cg_freeze(const char *path, int freeze);

 
uint64_t cg_memory_current(const char *path);
uint64_t cg_memory_swap_current(const char *path);

 
uint64_t cg_pids_current(const char *path);



int cg_populated(const char *path);

 
int cg_remove(const char *path);

 
int cg_open_dir(const char *path);



int cg_open_memory_events(const char *path);



int cg_read_memory_events(const char *path, uint64_t *out_oom_kill,
                          uint64_t *out_oom);

 
int cg_open_cgroup_events(const char *path);

 
int cg_read_io_aggregate(const char *path, uint64_t *out_rbytes,
                         uint64_t *out_wbytes);

 
uint64_t cg_cpu_usage_usec(const char *path);

#endif
