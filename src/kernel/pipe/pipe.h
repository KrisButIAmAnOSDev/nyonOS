#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

#include "abi/abi.h"

#define PIPE_CAPACITY ABI_PIPE_CAPACITY

struct task;

int pipe_create(struct task *t, int fds[2]);

#endif
