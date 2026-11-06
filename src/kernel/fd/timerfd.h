#ifndef TIMERFD_H
#define TIMERFD_H

#include <stdint.h>

void timerfd_tick(uint64_t now);
int timerfd_create(int clockid, uint32_t flags, uint16_t *out_flags);
int timerfd_settime(int fd, unsigned flags, const void *newv, void *oldv);
int timerfd_gettime(int fd, void *val);

#endif
