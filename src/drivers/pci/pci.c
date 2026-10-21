#include "drivers/pci/pci.h"

#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/vmm/vmm.h"
#include "abi/abi.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static pci_device_t devices[PCI_MAX_DEVICES];
static int device_count;

static uint32_t cfg_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (uint32_t)(off & 0xFC);
}

static uint32_t cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CFG_ADDR, cfg_address(bus, slot, func, off));
    return inl(PCI_CFG_DATA);
}

static void cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    outl(PCI_CFG_ADDR, cfg_address(bus, slot, func, off));
    outl(PCI_CFG_DATA, val);
}

static uint16_t cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = cfg_read32(bus, slot, func, off);
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFF);
}

static void cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t val) {
    uint32_t v = cfg_read32(bus, slot, func, off);
    uint32_t shift = (uint32_t)((off & 2) * 8);
    v = (v & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    cfg_write32(bus, slot, func, off, v);
}

static uint8_t cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = cfg_read32(bus, slot, func, off);
    return (uint8_t)((v >> ((off & 3) * 8)) & 0xFF);
}

static bool probe_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t id = cfg_read32(bus, slot, func, PCI_HDR_VENDOR);
    uint16_t vendor = (uint16_t)(id & 0xFFFF);

    if (vendor == 0xFFFF) return false;

    if (device_count >= PCI_MAX_DEVICES) return true;

    pci_device_t *d = &devices[device_count];
    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor_id = vendor;
    d->device_id = (uint16_t)(id >> 16);

    uint32_t cls = cfg_read32(bus, slot, func, PCI_HDR_REVISION);
    d->revision = (uint8_t)(cls & 0xFF);
    d->prog_if = (uint8_t)((cls >> 8) & 0xFF);
    d->subclass = (uint8_t)((cls >> 16) & 0xFF);
    d->class_code = (uint8_t)((cls >> 24) & 0xFF);

    d->header_type = cfg_read8(bus, slot, func, PCI_HDR_HEADTYPE);
    d->command = cfg_read16(bus, slot, func, PCI_HDR_COMMAND);

    d->interrupt_line = cfg_read8(bus, slot, func, PCI_HDR_INTRLINE);
    d->interrupt_pin = cfg_read8(bus, slot, func, PCI_HDR_INTRPIN);

    d->is_bridge = (d->class_code == PCI_CLASS_BRIDGE &&
                    d->subclass == PCI_SUBCLASS_PCI_BRIDGE);
    if (d->is_bridge)
        d->secondary_bus = cfg_read8(bus, slot, func, 0x19);

    
    uint16_t saved = d->command;
    cfg_write16(bus, slot, func, PCI_HDR_COMMAND, saved & ~(PCI_CMD_IO | PCI_CMD_MEMORY));

    for (int i = 0; i < PCI_MAX_BARS; i++) {
        uint8_t off = (uint8_t)(PCI_HDR_BAR0 + i * 4);
        uint32_t orig = cfg_read32(bus, slot, func, off);

        if (orig == 0 || orig == 0xFFFFFFFFu) {
            d->bar_present[i] = false;
            continue;
        }

        d->bar_present[i] = true;
        d->bar_is_io[i] = (orig & PCI_BAR_IO) != 0;

        cfg_write32(bus, slot, func, off, 0xFFFFFFFFu);
        uint32_t mask = cfg_read32(bus, slot, func, off);
        cfg_write32(bus, slot, func, off, orig);

        bool mem64 = !d->bar_is_io[i] && (orig & PCI_BAR_MEM64) == PCI_BAR_MEM64;

        if (d->bar_is_io[i]) {
            d->bar_size[i] = (uint64_t)((~(mask & 0xFFFFFFFCu)) + 1);
        } else if (mem64) {
            uint8_t off2 = (uint8_t)(off + 4);
            uint32_t orig2 = cfg_read32(bus, slot, func, off2);
            cfg_write32(bus, slot, func, off2, 0xFFFFFFFFu);
            uint32_t mask2 = cfg_read32(bus, slot, func, off2);
            cfg_write32(bus, slot, func, off2, orig2);
            uint64_t m = ((uint64_t)mask2 << 32) | (uint64_t)mask;
            d->bar_size[i] = ((~m) & 0xFFFFFFFFFFFFFFF0ull) + 1;
            d->bar[i + 1] = orig2;
            d->bar_present[i + 1] = false;
        } else {
            d->bar_size[i] = (uint64_t)((~(mask & 0xFFFFFFF0u)) + 1);
        }
    }

    cfg_write16(bus, slot, func, PCI_HDR_COMMAND, saved);

    device_count++;
    return true;
}

static void check_bus_recursive(uint8_t bus) {
    for (uint8_t slot = 0; slot < PCI_MAX_SLOTS; slot++) {
        for (uint8_t func = 0; func < PCI_MAX_FUNCS; func++) {
            uint32_t id = cfg_read32(bus, slot, func, PCI_HDR_VENDOR);
            if ((id & 0xFFFF) == 0xFFFF) continue;

            uint8_t header = cfg_read8(bus, slot, func, PCI_HDR_HEADTYPE);
            bool multifunction = (header & 0x80) != 0;

            probe_function(bus, slot, func);

            uint8_t cls = cfg_read8(bus, slot, func, PCI_HDR_CLASS);
            uint8_t sub = cfg_read8(bus, slot, func, PCI_HDR_SUBCLASS);
            if (cls == PCI_CLASS_BRIDGE && sub == PCI_SUBCLASS_PCI_BRIDGE) {
                uint8_t secondary = cfg_read8(bus, slot, func, 0x19);
                if (secondary != 0)
                    check_bus_recursive(secondary);
            }

            if (!multifunction) break;
        }
    }
}

static const char *class_name(uint8_t cls) {
    switch (cls) {
    case 0x00: return "unclassified";
    case 0x01: return "storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x05: return "memory";
    case 0x06: return "bridge";
    case 0x07: return "communication";
    case 0x08: return "system peripheral";
    case 0x09: return "input";
    case 0x0C: return "serial bus";
    case 0x0D: return "wireless";
    default: return "other";
    }
}


#define PCI_MMIO_BASE   0xC0000000ull
#define PCI_MMIO_LIMIT  0xFEC00000ull
#define PCI_IO_BASE     0xC000ull
#define PCI_IO_LIMIT    0xFFFFull
#define PCI_KINDLE_BASE 0xFFFF800000000000ull

static uint64_t mmio_next = PCI_MMIO_BASE;
static uint16_t io_next = PCI_IO_BASE;

bool pci_assign_bars(void) {
    bool ok = true;

    for (int i = 0; i < device_count; i++) {
        pci_device_t *d = &devices[i];

        for (int b = 0; b < PCI_MAX_BARS; b++) {
            if (!d->bar_present[b]) continue;

            uint64_t size = d->bar_size[b];
            if (size == 0) continue;

            if (d->bar_is_io[b]) {
                uint64_t aligned = ((uint64_t)io_next + size - 1) & ~(size - 1);
                if (aligned + size > PCI_IO_LIMIT) { ok = false; continue; }
                io_next = (uint16_t)(aligned + size);
                d->bar_addr[b] = aligned;
            } else {
                uint64_t aligned = (mmio_next + size - 1) & ~(size - 1);
                if (aligned + size > PCI_MMIO_LIMIT) { ok = false; continue; }
                mmio_next = aligned + size;
                d->bar_addr[b] = aligned;
            }
        }
    }

    return ok;
}

bool pci_map_bars(pci_device_t *dev) {
    bool ok = true;

    for (int b = 0; b < PCI_MAX_BARS; b++) {
        if (!dev->bar_present[b]) continue;

        uint64_t size = dev->bar_size[b];
        if (size == 0) continue;

        if (dev->bar_is_io[b]) {
            dev->bar_vaddr[b] = dev->bar_addr[b];
            continue;
        }

        uint64_t vaddr = PCI_KINDLE_BASE + dev->bar_addr[b];
        size_t pages = (size_t)(size / 4096);
        if (pages == 0) pages = 1;

        if (!vmm_map(vaddr, dev->bar_addr[b], pages, PAGE_PRESENT | PAGE_WRITE)) {
            ok = false;
            continue;
        }
        dev->bar_vaddr[b] = vaddr;
    }

    return ok;
}

void pci_program_bars(pci_device_t *d) {
    bool skip = false;

    for (int b = 0; b < PCI_MAX_BARS; b++) {
        if (!d->bar_present[b]) continue;
        if (d->bar_size[b] == 0) continue;

        if (d->class_code == PCI_CLASS_DISPLAY && !d->bar_is_io[b]) {
            skip = true;
            continue;
        }

        uint8_t off = (uint8_t)(PCI_HDR_BAR0 + b * 4);
        if (d->bar_is_io[b]) {
            cfg_write32(d->bus, d->slot, d->func, off, (uint32_t)d->bar_addr[b]);
        } else {
            cfg_write32(d->bus, d->slot, d->func, off,
                        (uint32_t)(d->bar_addr[b] & 0xFFFFFFF0u));
            if ((d->bar[b] & PCI_BAR_MEM64) == PCI_BAR_MEM64) {
                cfg_write32(d->bus, d->slot, d->func, (uint8_t)(off + 4),
                            (uint32_t)(d->bar_addr[b] >> 32));
            }
        }
    }

    if (skip) return;

    uint16_t cmd = d->command | PCI_CMD_IO | PCI_CMD_MEMORY;
    cfg_write16(d->bus, d->slot, d->func, PCI_HDR_COMMAND, cmd);
    d->command = cmd;
}

int pci_init(void) {
    device_count = 0;

    
    uint8_t header = cfg_read8(0, 0, 0, PCI_HDR_HEADTYPE);
    if (header & 0x80) {
        for (uint8_t func = 0; func < PCI_MAX_FUNCS; func++) {
            uint32_t id = cfg_read32(0, 0, func, PCI_HDR_VENDOR);
            if ((id & 0xFFFF) == 0xFFFF) break;
            check_bus_recursive(func);
        }
    } else {
        check_bus_recursive(0);
    }

    kprintf(PRINT_SERIAL, "PCI: %d device(s) found\n", device_count);

    for (int i = 0; i < device_count; i++) {
        pci_device_t *d = &devices[i];
        kprintf(PRINT_SERIAL, "PCI: %02x:%02x.%u %04x:%04x class %02x (%s) hdr %02x",
                d->bus, d->slot, d->func, d->vendor_id, d->device_id,
                d->class_code, class_name(d->class_code), d->header_type);
        if (d->is_bridge)
            kprintf(PRINT_SERIAL, " bridge->bus %02x", d->secondary_bus);
        if (d->interrupt_pin)
            kprintf(PRINT_SERIAL, " int%u line %02x", d->interrupt_pin, d->interrupt_line);
        kprintf(PRINT_SERIAL, "\n");

        for (int b = 0; b < PCI_MAX_BARS; b++) {
            if (!d->bar_present[b]) continue;
            kprintf(PRINT_SERIAL, "PCI:   BAR%d %s size 0x%llx\n", b,
                    d->bar_is_io[b] ? "io  " : "mem ",
                    (unsigned long long)d->bar_size[b]);
        }
    }

    return device_count;
}

int pci_device_count(void) {
    return device_count;
}

const pci_device_t *pci_device_at(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

const pci_device_t *pci_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < device_count; i++)
        if (devices[i].vendor_id == vendor && devices[i].device_id == device)
            return &devices[i];
    return NULL;
}

const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < device_count; i++)
        if (devices[i].class_code == class_code && devices[i].subclass == subclass)
            return &devices[i];
    return NULL;
}
