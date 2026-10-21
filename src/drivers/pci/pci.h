#ifndef _PCI_H
#define _PCI_H

#include <stdint.h>
#include <stdbool.h>

#define PCI_MAX_BUSES    256
#define PCI_MAX_SLOTS    32
#define PCI_MAX_FUNCS     8
#define PCI_MAX_BARS      6

#define PCI_CFG_ADDR     0xCF8
#define PCI_CFG_DATA     0xCFC

#define PCI_HDR_VENDOR   0x00
#define PCI_HDR_DEVICE   0x02
#define PCI_HDR_COMMAND  0x04
#define PCI_HDR_STATUS   0x06
#define PCI_HDR_REVISION 0x08
#define PCI_HDR_PROGIF   0x09
#define PCI_HDR_SUBCLASS 0x0A
#define PCI_HDR_CLASS    0x0B
#define PCI_HDR_HEADTYPE 0x0E
#define PCI_HDR_BAR0     0x10
#define PCI_HDR_CAPLIST  0x34
#define PCI_HDR_INTRLINE 0x3C
#define PCI_HDR_INTRPIN  0x3D

#define PCI_CMD_IO       0x0001
#define PCI_CMD_MEMORY   0x0002
#define PCI_CMD_MASTER   0x0004

#define PCI_BAR_IO       0x00000001u
#define PCI_BAR_MEM64    0x00000004u
#define PCI_BAR_PREFETCH 0x00000008u

#define PCI_CLASS_DISPLAY 0x03
#define PCI_CLASS_BRIDGE 0x06
#define PCI_SUBCLASS_PCI_BRIDGE 0x04

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t header_type;

    uint16_t command;

    uint32_t bar[PCI_MAX_BARS];
    uint64_t bar_size[PCI_MAX_BARS];
    bool bar_is_io[PCI_MAX_BARS];
    bool bar_present[PCI_MAX_BARS];
    uint64_t bar_addr[PCI_MAX_BARS];
    uint64_t bar_vaddr[PCI_MAX_BARS];

    uint8_t interrupt_line;
    uint8_t interrupt_pin;

    bool is_bridge;
    uint8_t secondary_bus;
} pci_device_t;

#define PCI_MAX_DEVICES 64

int pci_init(void);
bool pci_assign_bars(void);
void pci_program_bars(pci_device_t *dev);
bool pci_map_bars(pci_device_t *dev);
int pci_device_count(void);
const pci_device_t *pci_device_at(int index);
const pci_device_t *pci_find(uint16_t vendor, uint16_t device);
const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);

#endif
