#ifndef CGROUPD_PSI_H
#define CGROUPD_PSI_H

#include <stdint.h>

 
struct psi_avg {
    double avg10, avg60, avg300;
    uint64_t total;
};



struct psi {
    struct psi_avg some;
    struct psi_avg full;
    int has_full;
};




int psi_read(const char *cgroup_path, const char *resource, struct psi *out);




int psi_watch(const char *cgroup_path, const char *resource,
              const char *level, uint64_t stall_us, uint64_t window_us);

#endif
