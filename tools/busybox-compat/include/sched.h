#ifndef _BUSYBOX_COMPAT_SCHED_H
#define _BUSYBOX_COMPAT_SCHED_H

#include_next <sched.h>

/* The kernel is single-cpu, so the affinity mask is always one bit. */

static inline int sched_getaffinity(int __pid, unsigned long __size, void *__mask) {
	(void)__pid;
	(void)__size;
	(void)__mask;
	return 0;
}

#endif
