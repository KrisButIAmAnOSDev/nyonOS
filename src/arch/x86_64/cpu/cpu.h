#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct cpu_features {
    bool nx;
    bool smep;
    bool smap;
    bool rdrand;
    bool rdseed;
    bool pcid;
    bool tsc;
    bool invariant_tsc;
    bool apic;
    bool fpu;
    bool sse;
    bool sse2;
    bool fsgsbase;
    bool la57;
    bool syscall;
    bool invpcid;
};

void cpu_detect(void);
const struct cpu_features *cpu_features_get(void);

bool cpu_has_nx(void);
bool cpu_has_smep(void);
bool cpu_has_smap(void);
bool cpu_has_rdrand(void);
bool cpu_has_rdseed(void);
bool cpu_has_pcid(void);
bool cpu_has_la57(void);

bool cpu_rdrand64(uint64_t *out);
bool cpu_rdseed64(uint64_t *out);
uint64_t cpu_rdtsc(void);
void cpu_rdtsc_ordered(void);

void cpu_fxsave64_to(void *buf, size_t len);
void cpu_cr0(void);
uint64_t cpu_cr0_read(void);
uint64_t cpu_cr4_read(void);
void cpu_cr4_write(uint64_t value);

void cpu_enable_nx(void);
bool cpu_enable_smep_smap(void);
bool cpu_enable_pcid(void);
bool cpu_smap_enabled(void);
void cpu_flush_pcid(void);

#endif
