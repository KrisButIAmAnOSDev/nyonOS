#ifndef RES_H
#define RES_H

#include "sys.h"

#define RES_VIRT       0x60000000UL
#define RES_DONE       0
#define RES_CHECK_BASE 1
#define RES_GOT_BASE   192
#define RES_FAIL_BASE  320

#define RES_MAX_CHECKS (RES_GOT_BASE - RES_CHECK_BASE)

#define RES_STDIN_REQ 448
#define RES_STDIN_ACK 456

#define RES_SLOTS 512

#define RES_PENDING 0xdeadbeefdeadbeefUL
#define RES_ALLPASS 1
#define RES_FAILED  2

#endif