#include "crypto.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/sync/preempt.h"
#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/cpu/cpu.h"
#include "arch/x86_64/syscall/syscall_msr.h"

#define CHACHA_ROUNDS 20
#define CHACHA_WORDS 16
#define CHACHA_BLOCK 64
#define POOL_WORDS 64
#define POOL_SIZE (POOL_WORDS * 8)
#define RDSEED_RETRIES 32
#define RDTSC_JITTER 64

typedef struct {
    uint32_t state[CHACHA_WORDS];
    uint32_t keystream[CHACHA_BLOCK / 4];
    uint32_t counter;
    size_t produced;
} chacha_ctx;

static chacha_ctx rng[MAX_CPUS];
static uint32_t pool[MAX_CPUS][POOL_WORDS];
static bool pool_ready[MAX_CPUS];
static bool seeded_once;
static uint8_t crypto_seed_key[CRYPTO_MAX_ENTROPY];

static inline uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static uint32_t load32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void quarter_round(uint32_t *s, int a, int b, int c, int d) {
    s[a] += s[b]; s[d] = rotl32(s[d] ^ s[a], 16);
    s[c] += s[d]; s[b] = rotl32(s[b] ^ s[c], 12);
    s[a] += s[b]; s[d] = rotl32(s[d] ^ s[a], 8);
    s[c] += s[d]; s[b] = rotl32(s[b] ^ s[c], 7);
}

static void chacha_block(chacha_ctx *c) {
    uint32_t x[CHACHA_WORDS];
    for (int i = 0; i < CHACHA_WORDS; i++) x[i] = c->state[i];

    for (int i = 0; i < CHACHA_ROUNDS; i += 2) {
        quarter_round(x, 0, 4, 8, 12);
        quarter_round(x, 1, 5, 9, 13);
        quarter_round(x, 2, 6, 10, 14);
        quarter_round(x, 3, 7, 11, 15);
        quarter_round(x, 0, 5, 10, 15);
        quarter_round(x, 1, 6, 11, 12);
        quarter_round(x, 2, 7, 8, 13);
        quarter_round(x, 3, 4, 9, 14);
    }

    for (int i = 0; i < CHACHA_WORDS; i++) c->keystream[i] = x[i] + c->state[i];
}

static void chacha_init(chacha_ctx *c, const uint8_t seed[32], uint64_t counter) {
    c->state[0] = 0x61707865;
    c->state[1] = 0x3320646e;
    c->state[2] = 0x79622d32;
    c->state[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) c->state[4 + i] = load32_le(seed + i * 4);
    c->state[12] = (uint32_t)counter;
    c->state[13] = (uint32_t)(counter >> 32);
    c->state[14] = 0;
    c->state[15] = 0;
    c->counter = (uint32_t)counter;
    c->produced = CHACHA_BLOCK;
}

static void chacha_refill(chacha_ctx *c) {
    uint64_t n = ((uint64_t)c->counter + 1) & 0xFFFFFFFFFFFFULL;
    c->state[12] = (uint32_t)n;
    c->state[13] = (uint32_t)(n >> 32);
    chacha_block(c);
    c->counter = (uint32_t)n;
    c->produced = 0;
}

static void chacha_bytes(chacha_ctx *c, void *out, size_t len) {
    uint8_t *p = (uint8_t *)out;
    while (len) {
        if (c->produced >= CHACHA_BLOCK) chacha_refill(c);
        size_t take = CHACHA_BLOCK - c->produced;
        if (take > len) take = len;
        for (size_t i = 0; i < take; i++) p[i] = (uint8_t)(c->keystream[(c->produced + i) / 4] >> (8 * ((c->produced + i) % 4)));
        c->produced += take;
        p += take;
        len -= take;
    }
}

#define ENT_RDRAND 1
#define ENT_RDSEED 2
#define ENT_TSC    4
#define ENT_PIT    8

static unsigned entropy;

static size_t gather_entropy(uint8_t *out, size_t len) {
    size_t got = 0;

    if (cpu_has_rdrand()) {
        while (got + 8 <= len) {
            uint64_t v;
            if (!cpu_rdrand64(&v)) break;
            for (int i = 0; i < 8; i++) out[got + i] = (uint8_t)(v >> (8 * i));
            got += 8;
            entropy |= ENT_RDRAND;
        }
    }

    if (cpu_has_rdseed()) {
        for (int attempt = 0; attempt < RDSEED_RETRIES && got + 8 <= len; attempt++) {
            uint64_t v;
            if (!cpu_rdseed64(&v)) break;
            for (int i = 0; i < 8; i++) out[got + i] = (uint8_t)(v >> (8 * i));
            got += 8;
            entropy |= ENT_RDSEED;
        }
    }

    for (int i = 0; i < RDTSC_JITTER && got + 8 <= len; i++) {
        uint64_t a = cpu_rdtsc();
        for (volatile int spin = 0; spin < 32; spin++) { }
        uint64_t b = cpu_rdtsc();
        for (int j = 0; j < 8; j++) out[got + j] = (uint8_t)((a >> (8 * j)) ^ (b >> (8 * j)) ^ (uint8_t)i);
        got += 8;
        entropy |= ENT_TSC;
    }

    for (int i = 0; i < 64 && got + 8 <= len; i++) {
        uint64_t t = pit_get_ticks();
        for (int j = 0; j < 8; j++) out[got + j] = (uint8_t)(t >> (8 * j));
        got += 8;
        entropy |= ENT_PIT;
    }

    return got;
}

#define CRYPTO_GATHER 1024

#ifndef CRYPTO_FORCE_SEED
#define CRYPTO_FORCE_SEED 0
#endif

static uint64_t mix64(uint64_t h, uint8_t b) {
    return (h ^ b) * 0x100000001b3ULL;
}

static void crypto_seed_cpu(uint32_t id) {
    if (id >= MAX_CPUS) return;

    uint8_t seed[CRYPTO_MAX_ENTROPY];
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < sizeof(seed); i++) {
        h = mix64(h, crypto_seed_key[i]);
        h = mix64(h, (uint8_t)id);
        h = mix64(h, (uint8_t)(id >> 8));
        seed[i] = (uint8_t)(h >> 32);
    }

    chacha_init(&rng[id], seed, (uint64_t)id + 1);
    for (size_t i = 0; i < sizeof(pool[id]); i++) ((uint8_t *)pool[id])[i] = 0;
    chacha_bytes(&rng[id], pool[id], sizeof(pool[id]));
    pool_ready[id] = true;
}

void crypto_init(void) {
    static uint8_t raw[CRYPTO_GATHER];
    for (size_t i = 0; i < sizeof(raw); i++) raw[i] = 0;

    uint8_t seed[CRYPTO_MAX_ENTROPY];

    if (CRYPTO_FORCE_SEED) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < sizeof(seed); i++) {
            h = mix64(h, (uint8_t)(CRYPTO_FORCE_SEED >> ((i % 8) * 8)));
            h = mix64(h, (uint8_t)i);
            seed[i] = (uint8_t)(h >> 32);
        }
    } else {
        size_t got = gather_entropy(raw, sizeof(raw));

        __attribute__((aligned(16))) uint8_t fxsave[512];
        cpu_fxsave64_to(fxsave, sizeof(fxsave));

        for (size_t i = 0; i < sizeof(seed); i++) {
            uint64_t h = 0xcbf29ce484222325ULL;
            for (size_t j = i; j < got; j += CRYPTO_MAX_ENTROPY) h = mix64(h, raw[j]);
            for (size_t j = i; j < sizeof(fxsave); j += 71) h = mix64(h, fxsave[j]);
            seed[i] = (uint8_t)(h >> 32);
        }
    }

    for (size_t i = 0; i < sizeof(crypto_seed_key); i++) crypto_seed_key[i] = seed[i];

    for (uint32_t id = 0; id < MAX_CPUS; id++) crypto_seed_cpu(id);
    seeded_once = true;

    kprintf(PRINT_SERIAL, "CRYPTO: chacha20 seeded, entropy: %s%s%s%s%s\n",
            entropy & ENT_RDRAND ? "rdrand " : "",
            entropy & ENT_RDSEED ? "rdseed " : "",
            entropy & ENT_TSC ? "tsc " : "",
            entropy & ENT_PIT ? "pit" : "",
            CRYPTO_FORCE_SEED ? "forced " : "",
            (entropy & (ENT_RDRAND | ENT_RDSEED)) || CRYPTO_FORCE_SEED
                ? "" : " (no hardware rng, aslr is guessable)");
}

bool crypto_seeded(void) {
    uint32_t id = cpu_id();
    return seeded_once && id < MAX_CPUS && pool_ready[id];
}

void crypto_random_bytes(void *out, size_t len) {
    uint8_t *p = (uint8_t *)out;
    uint32_t id = cpu_id();

    if (id >= MAX_CPUS || !pool_ready[id]) {
        for (size_t i = 0; i < len; i++) p[i] = 0;
        return;
    }

    while (len) {
        preempt_disable();
        size_t i;
        for (i = 0; i < POOL_WORDS; i++) {
            if (__atomic_load_n(&pool[id][i], __ATOMIC_RELAXED)) break;
        }
        if (i == POOL_WORDS) {
            preempt_enable();
            chacha_bytes(&rng[id], pool[id], sizeof(pool[id]));
            continue;
        }
        uint32_t v = __atomic_exchange_n(&pool[id][i], 0, __ATOMIC_ACQUIRE);
        preempt_enable();

        if (!v) continue;

        size_t take = len < 4 ? len : 4;
        for (size_t j = 0; j < take; j++) p[j] = (uint8_t)(v >> (8 * j));
        p += take;
        len -= take;
    }
}

uint64_t crypto_random_u64(void) {
    uint64_t v = 0;
    crypto_random_bytes(&v, sizeof(v));
    return v;
}

uint32_t crypto_random_below(uint32_t bound) {
    if (bound == 0) return 0;
    uint32_t limit = UINT32_MAX - (UINT32_MAX % bound);
    uint32_t v;
    do {
        v = (uint32_t)crypto_random_u64();
    } while (v >= limit);
    return v % bound;
}
