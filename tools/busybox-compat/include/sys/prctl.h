#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H

#define PR_SET_NAME 15
#define PR_GET_NAME 16

static inline int prctl(int option, unsigned long a2, unsigned long a3,
                        unsigned long a4, unsigned long a5) {
    (void)option; (void)a2; (void)a3; (void)a4; (void)a5;
    return 0;
}

#endif
