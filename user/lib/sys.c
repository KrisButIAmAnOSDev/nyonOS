#include "sys.h"

long sys_write(int fd, const void *buf, unsigned long len) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void sys_exit(int code) {
    __asm__ volatile(
        "syscall"
        :
        : "a"((long)SYS_EXIT), "D"((long)code)
        : "rcx", "r11", "memory"
    );
    for (;;) { }
}
