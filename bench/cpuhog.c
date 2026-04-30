#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile int g_stop = 0;

static void *worker(void *arg) {
    (void)arg;
    volatile unsigned long x = 0;
    while (!g_stop) {
        for (int i = 0; i < 100000; i++) x = x * 1103515245u + 12345u;
    }
    return (void *)(unsigned long)x;
}

int main(int argc, char **argv) {
    int nthreads = (argc > 1) ? atoi(argv[1]) : 1;
    int seconds  = (argc > 2) ? atoi(argv[2]) : 5;
    if (nthreads < 1) nthreads = 1;
    pthread_t *t = calloc((size_t)nthreads, sizeof(*t));
    for (int i = 0; i < nthreads; i++) pthread_create(&t[i], NULL, worker, NULL);
    sleep((unsigned)seconds);
    g_stop = 1;
    for (int i = 0; i < nthreads; i++) pthread_join(t[i], NULL);
    free(t);
    return 0;
}
