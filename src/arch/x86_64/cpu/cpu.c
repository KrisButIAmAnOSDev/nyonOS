#include "cpu.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/syscall/syscall_msr.h"
#include "kernel/mm/vmm/vmm.h"

#define CPUID_MAX_EXTENDED 0x80000000u
#define CPUID_EXTENDED_FEATURES 0x80000001u
#define CPUID_EXTENDED_FEATURES2 0x80000007u
#define CPUID_FEATURES 0x00000001u
#define CPUID_STRUCTURE 0x00000007u

#define MSR_EFER_NXE       (1ULL << 11)
#define MSR_EFER_PCIDE     (1ULL << 17)

#define CR4_LA57           (1ULL << 12)
#define CR4_PCIDE          (1ULL << 17)
#define CR4_SMEP           (1ULL << 20)
#define CR4_SMAP           (1ULL << 21)

static struct cpu_features feat;
static bool detected;
static bool pcid_enabled;
static bool smap_on;

static inline void do_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static void detect_basic(void) {
    uint32_t a, b, c, d;
    do_cpuid(0, 0, &a, &b, &c, &d);

    do_cpuid(CPUID_FEATURES, 0, &a, &b, &c, &d);
    feat.tsc = (d >> 4) & 1;
    feat.fpu = (d >> 0) & 1;
    feat.apic = (d >> 9) & 1;
    feat.sse = (d >> 25) & 1;
    feat.sse2 = (d >> 26) & 1;
    feat.syscall = (d >> 11) & 1;
    feat.pcid = (c >> 22) & 1;

    if (a >= CPUID_STRUCTURE) {
        do_cpuid(CPUID_STRUCTURE, 0, &a, &b, &c, &d);
        feat.smap = (c >> 20) & 1;
        feat.smep = (c >> 7) & 1;
    }
}

static void detect_extended(void) {
    uint32_t a, b, c, d;
    do_cpuid(CPUID_MAX_EXTENDED, 0, &a, &b, &c, &d);
    if (a < CPUID_EXTENDED_FEATURES) return;

    do_cpuid(CPUID_EXTENDED_FEATURES, 0, &a, &b, &c, &d);
    feat.nx = (d >> 20) & 1;
    feat.rdrand = (c >> 30) & 1;
    feat.la57 = (c >> 16) & 1;

    if (a < CPUID_EXTENDED_FEATURES2) return;
    do_cpuid(CPUID_EXTENDED_FEATURES2, 0, &a, &b, &c, &d);
    feat.rdseed = (c >> 18) & 1;
    feat.fsgsbase = (b >> 0) & 1;
    feat.invariant_tsc = (d >> 8) & 1;
}

void cpu_detect(void) {
    if (detected) return;

    detect_basic();
    detect_extended();

    if (cpu_cr4_read() & CR4_LA57) feat.la57 = true;

    detected = true;

    kprintf(PRINT_SERIAL, "CPU: fpu=%d sse2=%d tsc=%d apic=%d syscall=%d\n",
            feat.fpu, feat.sse2, feat.tsc, feat.apic, feat.syscall);
    kprintf(PRINT_SERIAL, "CPU: nx=%d smep=%d smap=%d pcid=%d la57=%d\n",
            feat.nx, feat.smep, feat.smap, feat.pcid, feat.la57);
    kprintf(PRINT_SERIAL, "CPU: rdrand=%d rdseed=%d fsgsbase=%d invariant_tsc=%d\n",
            feat.rdrand, feat.rdseed, feat.fsgsbase, feat.invariant_tsc);
}

const struct cpu_features *cpu_features_get(void) {
    return &feat;
}

bool cpu_has_nx(void) { return feat.nx; }
bool cpu_has_smep(void) { return feat.smep; }
bool cpu_has_smap(void) { return feat.smap; }
bool cpu_has_rdrand(void) { return feat.rdrand; }
bool cpu_has_rdseed(void) { return feat.rdseed; }
bool cpu_has_pcid(void) { return feat.pcid; }
bool cpu_has_la57(void) { return feat.la57; }

bool cpu_rdrand64(uint64_t *out) {
    if (!feat.rdrand || !out) return false;
    uint64_t v = 0;
    uint8_t ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
    if (!ok) return false;
    *out = v;
    return true;
}

bool cpu_rdseed64(uint64_t *out) {
    if (!feat.rdseed || !out) return false;
    uint64_t v = 0;
    uint8_t ok;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
    if (!ok) return false;
    *out = v;
    return true;
}

uint64_t cpu_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void cpu_rdtsc_ordered(void) {
    uint32_t a, b, c, d;
    do_cpuid(0, 0, &a, &b, &c, &d);
    (void)cpu_rdtsc();
    do_cpuid(0, 0, &a, &b, &c, &d);
}

void cpu_fxsave64_to(void *buf, size_t len) {
    if (!buf || len < 512) return;
    __asm__ volatile("fxsave64 %0" : : "m"(*(uint8_t (*)[512])buf) : "memory");
}

uint64_t cpu_cr0_read(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

uint64_t cpu_cr4_read(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

void cpu_cr4_write(uint64_t value) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(value) : "memory");
}

void cpu_enable_nx(void) {
    if (!feat.nx) return;
    msr_write(MSR_EFER, msr_read(MSR_EFER) | MSR_EFER_NXE);
}

bool cpu_enable_smep_smap(void) {
    if (!feat.smep && !feat.smap) return false;

    uint64_t cr4 = cpu_cr4_read();
    if (feat.smep) cr4 |= CR4_SMEP;
    if (feat.smap) cr4 |= CR4_SMAP;
    cpu_cr4_write(cr4);

    smap_on = feat.smap;
    return feat.smep;
}

bool cpu_smap_enabled(void) {
    return smap_on;
}

bool cpu_enable_pcid(void) {
    if (!feat.pcid) return false;
    if (pcid_enabled) return true;

    cpu_cr4_write(cpu_cr4_read() | CR4_PCIDE);
    msr_write(MSR_EFER, msr_read(MSR_EFER) | MSR_EFER_PCIDE);
    pcid_enabled = true;
    return true;
}

void cpu_flush_pcid(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
