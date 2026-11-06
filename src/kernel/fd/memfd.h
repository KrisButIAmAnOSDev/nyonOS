#ifndef MEMFD_H
#define MEMFD_H

#include <stdint.h>
#include <stddef.h>

int memfd_create(uint32_t flags, uint16_t *out_flags);
int memfd_truncate(void *obj, uint64_t len);

#endif
