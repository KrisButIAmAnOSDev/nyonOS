#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stdbool.h>

#define ACPI_MAX_TABLES 64

struct acpi_sdt_header {
    char sig[4];
    uint32_t length;
    uint8_t rev;
    uint8_t checksum;
    char oemid[6];
    char oemtableid[8];
    uint32_t oemrev;
    uint32_t creatorid;
    uint32_t creatorrev;
} __attribute__((packed));

struct acpi_rsdp {
    char sig[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t rev;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_generic_addr {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t reserved;
    uint64_t address;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t lapic_phys;
    uint32_t flags;
} __attribute__((packed));

struct acpi_madt_entry {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct acpi_madt_lapic {
    struct acpi_madt_entry header;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

struct acpi_madt_ioapic {
    struct acpi_madt_entry header;
    uint8_t apic_id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed));

struct acpi_madt_override {
    struct acpi_madt_entry header;
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

struct acpi_hpet {
    struct acpi_sdt_header header;
    uint8_t hardware_rev_id;
    uint8_t comparator_count : 5;
    uint8_t counter_size : 1;
    uint8_t reserved : 1;
    uint8_t legacy_replacement : 1;
    uint16_t pci_vendor_id;
    struct acpi_generic_addr address;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((packed));

bool acpi_init(uint64_t rsdp_phys, uint64_t hhdm);
struct acpi_sdt_header *acpi_find(const char *sig);
uint32_t acpi_table_count(void);
void acpi_dump(void);
struct acpi_sdt_header *acpi_get_fadt(void);
struct acpi_sdt_header *acpi_get_madt(void);
struct acpi_hpet *acpi_get_hpet(void);

#define ACPI_MAX_CPUS 64
#define ACPI_MAX_IOAPICS 8
#define ACPI_MAX_OVERRIDES 16

struct acpi_cpu_info {
    uint32_t count;
    uint8_t apic_ids[ACPI_MAX_CPUS];
    uint8_t processor_ids[ACPI_MAX_CPUS];
    bool enabled[ACPI_MAX_CPUS];
};

struct acpi_ioapic_info {
    uint32_t count;
    uint8_t apic_ids[ACPI_MAX_IOAPICS];
    uint64_t addresses[ACPI_MAX_IOAPICS];
    uint32_t gsi_bases[ACPI_MAX_IOAPICS];
};

struct acpi_irq_override {
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
};

struct acpi_override_info {
    uint32_t count;
    struct acpi_irq_override entries[ACPI_MAX_OVERRIDES];
};

uint64_t acpi_lapic_phys(void);
bool acpi_parse_madt(struct acpi_cpu_info *cpus, struct acpi_ioapic_info *ioapics,
                     struct acpi_override_info *overrides);

#endif
