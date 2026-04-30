#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: memhog <megabytes> [seconds]\n");
        return 2;
    }
    size_t mb = (size_t)atoll(argv[1]);
    unsigned secs = (argc > 2) ? (unsigned)atoi(argv[2]) : 10;
    size_t bytes = mb * 1024 * 1024;
    char *p = malloc(bytes);
    if (!p) { perror("malloc"); return 1; }
     
    long pg = sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < bytes; i += (size_t)pg) p[i] = (char)(i & 0xff);
    fprintf(stderr, "memhog: holding %zu MiB for %u s\n", mb, secs);
    sleep(secs);
     
    volatile char sink = 0;
    for (size_t i = 0; i < bytes; i += (size_t)pg) sink ^= p[i];
    (void)sink;
    free(p);
    return 0;
}
