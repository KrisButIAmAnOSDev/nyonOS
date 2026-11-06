#ifndef EVENTFD_H
#define EVENTFD_H

#include <stdint.h>

int eventfd_create(uint32_t initval, uint32_t flags, uint16_t *out_flags);

#endif
