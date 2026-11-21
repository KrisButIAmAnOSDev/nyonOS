#include "hpet.h"
#include "arch/x86_64/acpi/acpi.h"
#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/pic/pic.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/vmm/vmm.h"

static volatile uint64_t *hpet_mmio;
static uint64_t hpet_ns_per_tick;
static uint32_t hpet_hz;

static inline uint64_t rd(uint64_t off) {
    return *(volatile uint64_t *)((uint8_t *)hpet_mmio + off);
}

static inline void wr(uint64_t off, uint64_t val) {
    *(volatile uint64_t *)((uint8_t *)hpet_mmio + off) = val;
}

uint64_t hpet_counter(void) {
    if (!hpet_mmio) return 0;
    return rd(HPET_COUNTER);
}

bool hpet_available(void) {
    return hpet_mmio != NULL;
}

uint64_t hpet_nanoseconds(void) {
    if (!hpet_mmio) return 0;
    return (rd(HPET_COUNTER) * hpet_ns_per_tick);
}

uint32_t hpet_frequency(void) {
    return hpet_hz;
}

bool hpet_init(uint32_t target_hz) {
    struct acpi_hpet *t = acpi_get_hpet();
    if (!t) {
        kprintf(PRINT_SERIAL, "HPET: no ACPI HPET table\n");
        return false;
    }
    uint64_t phys = t->address.address;
    kprintf(PRINT_SERIAL, "HPET: GAS space=%u width=%u offset=%u addr=%llx\n",
            t->address.space_id, t->address.bit_width, t->address.bit_offset,
            (unsigned long long)phys);
    if (!phys) return false;
    if (t->address.space_id != 0) {
        kprintf(PRINT_SERIAL, "HPET: unsupported address space %u\n",
                t->address.space_id);
        return false;
    }

    uint64_t page = phys & ~((uint64_t)PAGE_SIZE - 1);
    vaddr_t virt = vmm_hhdm_offset + phys;
    if (!vmm_map(vmm_hhdm_offset + page, page, 1,
                 PAGE_PRESENT | PAGE_WRITE | PAGE_PCD)) {
        kprintf(PRINT_SERIAL, "HPET: failed to map MMIO at %llx\n",
                (unsigned long long)phys);
        return false;
    }
    hpet_mmio = (volatile uint64_t *)virt;

    uint64_t cap = rd(HPET_CAP);
    uint64_t period_fs = cap >> 32;
    if (!period_fs || period_fs > 100000000ULL) {
        kprintf(PRINT_SERIAL, "HPET: bad clock period %llu fs\n",
                (unsigned long long)period_fs);
        hpet_mmio = NULL;
        return false;
    }

    uint32_t ntimers = (uint32_t)((cap >> 8) & 0x1F) + 1;
    hpet_hz = (uint32_t)(1000000000000000ULL / period_fs);
    hpet_ns_per_tick = period_fs / 1000000ULL;

    kprintf(PRINT_SERIAL, "HPET: %u timers, %u Hz, period %llu fs at %llx "
            "cap=%llx leg=%u size=%u\n",
            ntimers, hpet_hz, (unsigned long long)period_fs,
            (unsigned long long)phys, (unsigned long long)cap,
            (unsigned)((cap >> 15) & 1), (unsigned)((cap >> 13) & 1));

    if (!hpet_hz || target_hz == 0 || hpet_hz < target_hz) {
        hpet_mmio = NULL;
        return false;
    }

    uint64_t ticks_per_irq = hpet_hz / target_hz;
    if (!ticks_per_irq) ticks_per_irq = 1;

    wr(HPET_CONF, 0);
    wr(HPET_INT_STATUS, ~0ULL);

    uint64_t tconf = HPET_TN_PERIODIC;
    wr(HPET_TIMER0_CONF, tconf);
    wr(HPET_TIMER0_CMP, rd(HPET_COUNTER) + ticks_per_irq);

    wr(HPET_CONF, HPET_CONF_ENABLE | HPET_CONF_LEGACY);
    wr(HPET_TIMER0_CONF, tconf | HPET_TN_ENABLE);

    pit_set_frequency(target_hz);
    pic_clear_mask(0);

    kprintf(PRINT_SERIAL, "HPET: timer 0 driving IRQ0 at %u Hz\n", target_hz);
    return true;
}
