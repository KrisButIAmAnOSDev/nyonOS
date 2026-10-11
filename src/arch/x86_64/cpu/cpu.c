#include "cpu.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/syscall/syscall_msr.h"
#include "kernel/mm/vmm/vmm.h"
#include "arch/x86_64/gdt/gdt.h"
#include "arch/x86_64/lapic/lapic.h"
#include "arch/x86_64/idt/idt.h"
#include "arch/x86_64/syscall/syscall.h"

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

static struct percpu percpu_table[MAX_CPUS] __attribute__((aligned(CPU_CACHELINE)));
static bool gs_ready;
static uint32_t detected_cpus = 1;

static inline struct percpu *feats(void) { return cpu_self(); }

static void percpu_slot_init(uint32_t id) {
    struct percpu *s = &percpu_table[id];
    s->self = s;
    s->processor_id = id;
    s->current_slot = 0;
    s->in_scheduler = false;
    s->sched_enabled = false;
    s->switch_count = 0;
    s->resched_count = 0;
    s->wake_seq = 0;
    s->earliest_wake = UINT64_MAX;
    s->syscall_kstack_top = 0;
    s->held_mask = 0;
    s->lock_depth = 0;
    s->preempt_count = 0;
    s->copy_abort = 0;
    s->copy_active = false;
    s->online = false;
}

static void percpu_gs_install(uint32_t id) {
    struct percpu *s = &percpu_table[id];
    msr_write(MSR_GS_BASE, (uint64_t)s);
    __asm__ volatile("mov %0, %%gs:0xfffffffffffffff8"
                     :: "r"((uint64_t)s) : "memory");
}

static inline void do_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static void detect_basic(void) {
    struct percpu *p = feats();
    struct cpu_features *feat = &p->feat;
    uint32_t a, b, c, d;
    do_cpuid(0, 0, &a, &b, &c, &d);

    do_cpuid(CPUID_FEATURES, 0, &a, &b, &c, &d);
    feat->tsc = (d >> 4) & 1;
    feat->fpu = (d >> 0) & 1;
    feat->apic = (d >> 9) & 1;
    feat->sse = (d >> 25) & 1;
    feat->sse2 = (d >> 26) & 1;
    feat->syscall = (d >> 11) & 1;
    feat->pcid = (c >> 22) & 1;
    p->pcid_bit_seen = feat->pcid;

    if (a >= CPUID_STRUCTURE) {
        uint32_t max = a;
        do_cpuid(CPUID_STRUCTURE, 0, &a, &b, &c, &d);
        feat->smep = (b >> 7) & 1;
        feat->fsgsbase = (b >> 0) & 1;
        feat->invpcid = (b >> 10) & 1;
        if (max >= 1) {
            do_cpuid(CPUID_STRUCTURE, 1, &a, &b, &c, &d);
            feat->smap = (c >> 20) & 1;
        }
    }
}

static void detect_extended(void) {
    struct percpu *p = feats();
    struct cpu_features *feat = &p->feat;

    uint32_t a, b, c, d;
    do_cpuid(CPUID_MAX_EXTENDED, 0, &a, &b, &c, &d);
    if (a < CPUID_EXTENDED_FEATURES) return;

    do_cpuid(CPUID_EXTENDED_FEATURES, 0, &a, &b, &c, &d);
    feat->nx = (d >> 20) & 1;
    feat->rdrand = (c >> 30) & 1;
    feat->la57 = (c >> 16) & 1;

    if (a < CPUID_EXTENDED_FEATURES2) return;
    do_cpuid(CPUID_EXTENDED_FEATURES2, 0, &a, &b, &c, &d);
    feat->rdseed = (c >> 18) & 1;
    feat->invariant_tsc = (d >> 8) & 1;
}

void cpu_detect(void) {
    struct percpu *p = feats();
    if (p->detected) return;

    detect_basic();
    detect_extended();

    if (cpu_cr4_read() & CR4_LA57) feats()->feat.la57 = true;

    p->detected = true;

    kprintf(PRINT_SERIAL, "CPU: fpu=%d sse2=%d tsc=%d apic=%d syscall=%d\n",
            feats()->feat.fpu, feats()->feat.sse2, feats()->feat.tsc, feats()->feat.apic, feats()->feat.syscall);
    kprintf(PRINT_SERIAL, "CPU: nx=%d smep=%d smap=%d pcid=%d la57=%d\n",
            feats()->feat.nx, feats()->feat.smep, feats()->feat.smap, feats()->feat.pcid, feats()->feat.la57);
    kprintf(PRINT_SERIAL, "CPU: rdrand=%d rdseed=%d fsgsbase=%d invariant_tsc=%d\n",
            feats()->feat.rdrand, feats()->feat.rdseed, feats()->feat.fsgsbase, feats()->feat.invariant_tsc);
}

const struct cpu_features *cpu_features_get(void) {
    return &feats()->feat;
}

bool cpu_has_nx(void) { return feats()->feat.nx; }
bool cpu_has_smep(void) { return feats()->feat.smep; }
bool cpu_has_smap(void) { return feats()->feat.smap; }
bool cpu_has_rdrand(void) { return feats()->feat.rdrand; }
bool cpu_has_rdseed(void) { return feats()->feat.rdseed; }
bool cpu_has_pcid(void) { return feats()->pcid_enabled; }
bool cpu_has_la57(void) { return feats()->feat.la57; }

bool cpu_rdrand64(uint64_t *out) {
    if (!feats()->feat.rdrand || !out) return false;
    uint64_t v = 0;
    uint8_t ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
    if (!ok) return false;
    *out = v;
    return true;
}

bool cpu_rdseed64(uint64_t *out) {
    if (!feats()->feat.rdseed || !out) return false;
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
    if (!feats()->feat.nx) return;
    msr_write(MSR_EFER, msr_read(MSR_EFER) | MSR_EFER_NXE);
}

bool cpu_enable_smep_smap(void) {
    if (!feats()->feat.smep && !feats()->feat.smap) return false;

    uint64_t cr4 = cpu_cr4_read();
    if (feats()->feat.smep) cr4 |= CR4_SMEP;
    if (feats()->feat.smap) cr4 |= CR4_SMAP;
    cpu_cr4_write(cr4);

    feats()->smap_on = feats()->feat.smap;
    return feats()->feat.smep;
}

bool cpu_smap_enabled(void) {
    return feats()->smap_on;
}

bool cpu_enable_pcid(void) {
    if (!feats()->pcid_bit_seen) return false;
    if (!feats()->feat.invpcid) {
        kprintf(PRINT_SERIAL, "CPU: pcid present but invpcid missing, leaving it off\n");
        return false;
    }
    if (feats()->pcid_enabled) return true;

    cpu_cr4_write(cpu_cr4_read() | CR4_PCIDE);
    msr_write(MSR_EFER, msr_read(MSR_EFER) | MSR_EFER_PCIDE);
    feats()->pcid_enabled = true;
    return true;
}

void cpu_flush_pcid(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

struct percpu *cpu_self(void) {
    if (!gs_ready) return &percpu_table[0];

    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(MSR_GS_BASE));
    if (((uint64_t)high << 32 | low) == 0) return &percpu_table[0];

    struct percpu *p;
    __asm__ volatile("mov %0, %%gs:0xfffffffffffffff8" : "=r"(p) : : "memory");
    if (!p) return &percpu_table[0];
    return p;
}

uint32_t cpu_id(void) { return cpu_self()->processor_id; }
uint32_t cpu_count(void) { return detected_cpus; }

uint32_t cpu_online_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (__atomic_load_n(&percpu_table[i].online, __ATOMIC_ACQUIRE)) n++;
    }
    return n;
}

bool cpu_online_by_id(uint32_t id) {
    if (id >= MAX_CPUS) return false;
    return __atomic_load_n(&percpu_table[id].online, __ATOMIC_ACQUIRE);
}

void cpu_mark_online(uint32_t processor_id, uint32_t lapic_id) {
    if (processor_id >= MAX_CPUS) return;
    percpu_table[processor_id].lapic_id = lapic_id;
    __atomic_store_n(&percpu_table[processor_id].online, true, __ATOMIC_RELEASE);
}

void cpu_set_detected(uint32_t n) {
    if (n > MAX_CPUS) n = MAX_CPUS;
    detected_cpus = n;
}

size_t cpu_current_slot_of(uint32_t id) {
    if (id >= MAX_CPUS) return SIZE_MAX;
    if (!cpu_online_by_id(id)) return SIZE_MAX;
    return percpu_table[id].current_slot;
}

uint64_t cpu_resched_count(uint32_t id) {
    if (id >= MAX_CPUS) return 0;
    return percpu_table[id].resched_count;
}

uint64_t cpu_switch_count(uint32_t id) {
    if (id >= MAX_CPUS) return 0;
    return percpu_table[id].switch_count;
}

void *percpu_self_addr(void) { return cpu_self()->self; }
uint64_t *percpu_syscall_kstack_top(void) { return &cpu_self()->syscall_kstack_top; }
uint64_t *percpu_copy_abort(void) { return &cpu_self()->copy_abort; }
bool *percpu_copy_active(void) { return &cpu_self()->copy_active; }

void percpu_copy_begin(void) {
    struct percpu *s = cpu_self();
    s->copy_abort = 0;
    s->copy_active = true;
}

void percpu_copy_end(void) {
    struct percpu *s = cpu_self();
    s->copy_active = false;
}

void percpu_copy_abort_set(void) {
    __atomic_store_n(&cpu_self()->copy_abort, 1, __ATOMIC_RELEASE);
}

uint64_t percpu_copy_abort_get(void) {
    return __atomic_load_n(&cpu_self()->copy_abort, __ATOMIC_ACQUIRE);
}

void cpu_bootstrap(void) {
    for (uint32_t i = 0; i < MAX_CPUS; i++) percpu_slot_init(i);

    percpu_gs_install(0);
    gs_ready = true;
    detected_cpus = 1;

    kprintf(PRINT_SERIAL, "CPU: percpu at %p, gs slot %d, max %u\n",
            (void *)percpu_table, -8, (unsigned)MAX_CPUS);
}

void cpu_bootstrap_ap(uint32_t processor_id, uint32_t lapic_id, uint64_t hhdm_offset) {
    if (processor_id >= MAX_CPUS) return;

    percpu_slot_init(processor_id);
    percpu_table[processor_id].lapic_id = lapic_id;
    percpu_gs_install(processor_id);

    gdt_init(hhdm_offset, processor_id);
    percpu_gs_install(processor_id);

    cpu_detect();
    cpu_enable_nx();
    cpu_enable_smep_smap();
    cpu_enable_pcid();

    idt_reload();
    syscall_msr_init();

    uint64_t lapic = hhdm_offset + LAPIC_DEFAULT_PHYS;
    percpu_table[processor_id].lapic_base = lapic;
    lapic_spurious_enable(lapic, 0xFF);
    lapic_mask_all_lvt(lapic);
    lapic_write_lvt_timer(lapic, LAPIC_RESCHED_VECTOR);

    percpu_table[processor_id].online = true;
}

void cpu_init_cpu(void) {
    struct percpu *s = cpu_self();
    s->current_slot = 0;
    s->in_scheduler = false;
    s->sched_enabled = true;
    s->switch_count = 0;
    s->earliest_wake = UINT64_MAX;
}

static uint32_t limine_paging_mode;
static bool limine_paging_known;

void cpu_paging_mode_report(uint64_t mode, bool known) {
    limine_paging_mode = (uint32_t)mode;
    limine_paging_known = known;
}

const char *cpu_paging_mode_name(void) {
    if (!limine_paging_known) return "unknown";
    switch (limine_paging_mode) {
        case 0: return "4-level";
        case 1: return "5-level";
        default: return "unknown";
    }
}

int cpu_paging_mode_matches(void) {
    const char *want = cpu_paging_mode_name();
    return vmm_5level ? (want[0] == '5') : (want[0] == '4');
}
