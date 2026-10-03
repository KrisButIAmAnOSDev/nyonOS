
#include "syscall_msr.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/gdt/gdt.h"

extern void syscall_entry(void);

uint64_t syscall_user_rsp;
struct tss *tss_rsp0_ptr;

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)(value & 0xFFFFFFFFULL)), "d"((uint32_t)(value >> 32)));
}

void syscall_msr_init(void) {
    tss_rsp0_ptr = (struct tss *)tss_addr();
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1ULL);

    write_msr(MSR_SYSENTER_CS, GDT_KERNEL_CODE);
    write_msr(MSR_STAR, ((uint64_t)0x10ULL << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));

    write_msr(MSR_SYSCALL_KERNEL_GS, 0);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
    write_msr(MSR_SYSCALL_SFMASK, SYSCALL_SFMASK_VALUE);

    kprintf(PRINT_SERIAL, "SYSCALL msr: lstar=0x%llx star=0x%llx sfmask=0x%llx\n", (unsigned long long)read_msr(MSR_LSTAR), (unsigned long long)read_msr(MSR_STAR), (unsigned long long)read_msr(MSR_SYSCALL_SFMASK));
}
