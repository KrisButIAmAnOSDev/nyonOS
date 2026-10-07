#include "syscall_msr.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/gdt/gdt.h"
#include "arch/x86_64/cpu/cpu.h"

extern void syscall_entry(void);

bool syscall_msr_init(void) {
    const struct cpu_features *f = cpu_features_get();
    if (!f->syscall) {
        kprintf(PRINT_SERIAL, "SYSCALL msr: cpu has no syscall instruction, int 0x80 only\n");
        return false;
    }

    msr_write(MSR_STAR, ((uint64_t)(GDT_USER_DATA - 8) << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    msr_write(MSR_LSTAR, (uint64_t)syscall_entry);
    msr_write(MSR_SYSCALL_SFMASK, SYSCALL_SFMASK_VALUE);
    msr_write(MSR_EFER, msr_read(MSR_EFER) | MSR_EFER_SCE);

    kprintf(PRINT_SERIAL, "SYSCALL msr: lstar=0x%llx star=0x%llx sfmask=0x%llx\n",
            (unsigned long long)msr_read(MSR_LSTAR), (unsigned long long)msr_read(MSR_STAR),
            (unsigned long long)msr_read(MSR_SYSCALL_SFMASK));
    return true;
}
