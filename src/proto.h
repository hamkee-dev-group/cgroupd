#ifndef CGROUPD_PROTO_H
#define CGROUPD_PROTO_H

#include <stddef.h>

#define CGROUPD_DEFAULT_SOCK "/run/cgroupd.sock"
#define CGROUPD_USER_SOCK_FMT "/run/user/%u/cgroupd.sock"


































int proto_read_msg(int fd, char **out_buf, size_t *out_len);



int proto_write_status(int fd, const char *status, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

 
int proto_write(int fd, const char *buf, size_t len);



const char *proto_verb(const char *buf, char *verb_out, size_t verb_out_len);






int proto_next_header(const char **cursor, char *scratch, size_t scratch_len,
                      char **key, char **val);

#endif
