#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_CPUS 8

#define CPU_CACHELINE 64

#define MSR_GS_BASE 0xC0000101

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

struct percpu {
    char pad0[CPU_CACHELINE];
    struct cpu_features feat;
    bool detected;
    bool pcid_enabled;
    bool smap_on;
    bool pcid_bit_seen;
    uint32_t processor_id;
    uint64_t lapic_base;
    uint32_t lapic_id;
    bool online;
    void *self;

    size_t current_slot;
    bool in_scheduler;
    bool sched_enabled;
    uint64_t switch_count;
    uint64_t resched_count;
    volatile uint64_t wake_seq;
    uint64_t earliest_wake;
    uint64_t syscall_kstack_top;

    uint32_t held_mask;
    uint32_t lock_depth;
    uint32_t preempt_count;

    uint64_t copy_abort;
    bool copy_active;

    char pad1[CPU_CACHELINE];
} __attribute__((aligned(CPU_CACHELINE)));

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

void cpu_bootstrap(void);
void cpu_bootstrap_ap(uint32_t processor_id, uint32_t lapic_id, uint64_t hhdm_offset);
void cpu_set_detected(uint32_t n);
struct percpu *cpu_self(void);
size_t cpu_current_slot_of(uint32_t id);
uint64_t cpu_resched_count(uint32_t id);
bool cpu_online_by_id(uint32_t id);
uint64_t cpu_switch_count(uint32_t id);
void *percpu_self_addr(void);
uint64_t *percpu_syscall_kstack_top(void);
uint64_t *percpu_copy_abort(void);
bool *percpu_copy_active(void);
void percpu_copy_begin(void);
void percpu_copy_end(void);
void percpu_copy_abort_set(void);
uint64_t percpu_copy_abort_get(void);
uint32_t cpu_id(void);
uint32_t cpu_count(void);
uint32_t cpu_online_count(void);
void cpu_mark_online(uint32_t processor_id, uint32_t lapic_id);
void cpu_paging_mode_report(uint64_t mode, bool known);
const char *cpu_paging_mode_name(void);
int cpu_paging_mode_matches(void);

#endif
