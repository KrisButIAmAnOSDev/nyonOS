#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CRYPTO_MAX_ENTROPY 32

void crypto_init(void);
bool crypto_seeded(void);

void crypto_random_bytes(void *out, size_t len);
uint64_t crypto_random_u64(void);

uint32_t crypto_random_below(uint32_t bound);

#endif
