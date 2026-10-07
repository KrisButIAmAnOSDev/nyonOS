#ifndef USER_SYS_H
#define USER_SYS_H

#include <stdint.h>
#include <stddef.h>

#define SYS_WRITE 2
#define SYS_EXIT  16

#define STDOUT 1
#define STDERR 2

long sys_write(int fd, const void *buf, unsigned long len);
void sys_exit(int code);

static inline long write(int fd, const void *buf, unsigned long len) {
    return sys_write(fd, buf, len);
}

static inline void exit(int code) {
    sys_exit(code);
    for (;;) { }
}

static inline unsigned long strlen_(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static inline int write_str(int fd, const char *s) {
    return (int)sys_write(fd, s, strlen_(s));
}

#endif
