#include "acpi.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/lib/string.h"

static uint64_t acpi_hhdm;
static struct acpi_sdt_header *acpi_tables[ACPI_MAX_TABLES];
static uint32_t acpi_count;
static uint8_t acpi_version;

static void *phys(uint64_t p) {
    return (void *)(acpi_hhdm + p);
}

static bool checksum_ok(const void *ptr, uint32_t len) {
    const uint8_t *b = (const uint8_t *)ptr;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

struct acpi_sdt_header *acpi_find(const char *sig) {
    for (uint32_t i = 0; i < acpi_count; i++) {
        struct acpi_sdt_header *h = acpi_tables[i];
        if (h && !memcmp(h->sig, sig, 4)) return h;
    }
    return NULL;
}

uint32_t acpi_table_count(void) {
    return acpi_count;
}

void acpi_dump(void) {
    kprintf(PRINT_SERIAL, "ACPI: version %u, %u tables\n", acpi_version, acpi_count);
    for (uint32_t i = 0; i < acpi_count; i++) {
        struct acpi_sdt_header *h = acpi_tables[i];
        char sig[5];
        memcpy(sig, h->sig, 4);
        sig[4] = 0;
        kprintf(PRINT_SERIAL, "ACPI:   %s rev=%u len=%u oem=%.6s\n",
                sig, h->rev, h->length, h->oemid);
    }
}

static void add_table(uint64_t addr) {
    if (!addr || acpi_count >= ACPI_MAX_TABLES) return;
    struct acpi_sdt_header *h = phys(addr);
    if (h->length < sizeof(struct acpi_sdt_header)) return;
    if (h->length > 1024 * 1024) return;
    if (!checksum_ok(h, h->length)) {
        char sig[5];
        memcpy(sig, h->sig, 4);
        sig[4] = 0;
        kprintf(PRINT_SERIAL, "ACPI: bad checksum on %.4s at %llx\n", sig,
                (unsigned long long)addr);
        return;
    }
    for (uint32_t i = 0; i < acpi_count; i++) {
        if (acpi_tables[i] == h) return;
    }
    acpi_tables[acpi_count++] = h;
}

bool acpi_init(uint64_t rsdp_phys, uint64_t hhdm) {
    if (!rsdp_phys || !hhdm) return false;
    acpi_hhdm = hhdm;
    acpi_count = 0;
    acpi_version = 0;

    struct acpi_rsdp *rsdp = (struct acpi_rsdp *)rsdp_phys;
    if (memcmp(rsdp->sig, "RSD PTR ", 8)) {
        kprintf(PRINT_SERIAL, "ACPI: no RSD PTR signature at %llx\n",
                (unsigned long long)rsdp_phys);
        return false;
    }
    if (!checksum_ok(rsdp, 20)) {
        kprintf(PRINT_SERIAL, "ACPI: bad RSDP checksum\n");
        return false;
    }

    acpi_version = rsdp->rev;
    if (rsdp->rev >= 2 && rsdp->length >= 36 && !checksum_ok(rsdp, rsdp->length)) {
        kprintf(PRINT_SERIAL, "ACPI: bad RSDP extended checksum\n");
        return false;
    }

    uint64_t table_addr = 0;
    bool wide = false;
    if (rsdp->rev >= 2 && rsdp->xsdt_addr) {
        table_addr = rsdp->xsdt_addr;
        wide = true;
    } else if (rsdp->rsdt_addr) {
        table_addr = rsdp->rsdt_addr;
    } else {
        kprintf(PRINT_SERIAL, "ACPI: RSDP has neither RSDT nor XSDT\n");
        return false;
    }

    struct acpi_sdt_header *root = phys(table_addr);
    if (memcmp(root->sig, wide ? "XSDT" : "RSDT", 4)) {
        kprintf(PRINT_SERIAL, "ACPI: root table signature mismatch\n");
        return false;
    }
    if (!checksum_ok(root, root->length)) {
        kprintf(PRINT_SERIAL, "ACPI: bad root table checksum\n");
        return false;
    }

    uint64_t entries_off = sizeof(struct acpi_sdt_header);
    uint64_t count = (root->length - entries_off) / (wide ? 8 : 4);
    if (count > ACPI_MAX_TABLES) count = ACPI_MAX_TABLES;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t addr;
        if (wide) {
            addr = *(uint64_t *)((uint8_t *)root + entries_off + i * 8);
        } else {
            addr = *(uint32_t *)((uint8_t *)root + entries_off + i * 4);
        }
        add_table(addr);
    }

    acpi_dump();
    return acpi_count > 0;
}

struct acpi_sdt_header *acpi_get_fadt(void) { return acpi_find("FACP"); }
struct acpi_sdt_header *acpi_get_madt(void) { return acpi_find("APIC"); }
struct acpi_hpet *acpi_get_hpet(void) {
    return (struct acpi_hpet *)acpi_find("HPET");
}

uint64_t acpi_lapic_phys(void) {
    struct acpi_madt *madt = (struct acpi_madt *)acpi_get_madt();
    if (madt) return madt->lapic_phys;
    return 0;
}

bool acpi_parse_madt(struct acpi_cpu_info *cpus, struct acpi_ioapic_info *ioapics,
                     struct acpi_override_info *overrides) {
    struct acpi_madt *madt = (struct acpi_madt *)acpi_get_madt();
    if (!madt) return false;

    if (cpus) {
        cpus->count = 0;
        for (uint32_t i = 0; i < ACPI_MAX_CPUS; i++) cpus->enabled[i] = false;
    }
    if (ioapics) ioapics->count = 0;
    if (overrides) overrides->count = 0;

    uint8_t *ptr = (uint8_t *)madt + sizeof(struct acpi_madt);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr + sizeof(struct acpi_madt_entry) <= end) {
        struct acpi_madt_entry *e = (struct acpi_madt_entry *)ptr;
        if (e->length < sizeof(struct acpi_madt_entry) || ptr + e->length > end) break;

        switch (e->type) {
        case 0: {
            struct acpi_madt_lapic *l = (struct acpi_madt_lapic *)ptr;
            if (cpus && cpus->count < ACPI_MAX_CPUS && e->length >= sizeof(struct acpi_madt_lapic)) {
                uint32_t i = cpus->count++;
                cpus->apic_ids[i] = l->apic_id;
                cpus->processor_ids[i] = l->processor_id;
                cpus->enabled[i] = (l->flags & 1) != 0;
            }
            break;
        }
        case 1: {
            struct acpi_madt_ioapic *io = (struct acpi_madt_ioapic *)ptr;
            if (ioapics && ioapics->count < ACPI_MAX_IOAPICS && e->length >= sizeof(struct acpi_madt_ioapic)) {
                uint32_t i = ioapics->count++;
                ioapics->apic_ids[i] = io->apic_id;
                ioapics->addresses[i] = io->address;
                ioapics->gsi_bases[i] = io->gsi_base;
            }
            break;
        }
        case 2: {
            struct acpi_madt_override *ov = (struct acpi_madt_override *)ptr;
            if (overrides && overrides->count < ACPI_MAX_OVERRIDES && e->length >= sizeof(struct acpi_madt_override)) {
                uint32_t i = overrides->count++;
                overrides->entries[i].source = ov->source;
                overrides->entries[i].gsi = ov->gsi;
                overrides->entries[i].flags = ov->flags;
            }
            break;
        }
        default:
            break;
        }

        ptr += e->length;
    }

    return true;
}
