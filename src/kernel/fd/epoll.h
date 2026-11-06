#ifndef EPOLL_H
#define EPOLL_H

#include <stdint.h>

struct task;

int epoll_create1(uint32_t flags, uint16_t *out_flags);
int epoll_ctl(int epfd, int op, int fd, const void *event);
int epoll_wait(int epfd, void *events, int maxevents, int timeout_ms);
void epoll_remove_fd(struct task *t, int fd);

#endif
