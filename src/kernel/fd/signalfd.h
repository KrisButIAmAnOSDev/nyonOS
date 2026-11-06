#ifndef SIGNALFD_H
#define SIGNALFD_H

#include <stdint.h>
#include <stdbool.h>

struct task;

bool task_signalfd_suppress(struct task *t, int sig);
int signalfd_create(uint64_t mask, uint32_t flags, uint16_t *out_flags);
int signalfd_setmask(int fd, uint64_t mask);

#endif
