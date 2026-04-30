#ifndef CGROUPD_UTIL_H
#define CGROUPD_UTIL_H

#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>



ssize_t read_file(const char *path, char *buf, size_t buflen);

 
int write_file(const char *path, const char *s);

 
int write_filef(const char *path, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

 
void rstrip(char *s);



uint64_t parse_size(const char *s);

 
void format_size(uint64_t bytes, char *dst, size_t dstlen);

 
int mkdir_p(const char *path, mode_t mode);

 
int set_cloexec(int fd);
int set_nonblock(int fd);

 
uint64_t now_ms(void);

 
uint64_t now_real_ms(void);

#endif
