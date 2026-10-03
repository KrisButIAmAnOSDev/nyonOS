#include "ata.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/pic/pic.h"
#include "arch/x86_64/pit/pit.h"

#define ATA_IO_PRIMARY   0x1F0
#define ATA_IO_SECONDARY 0x170

#define REG_DATA       0
#define REG_ERR        1
#define REG_SECTOR_CNT 2
#define REG_LBA_LOW    3
#define REG_LBA_MID    4
#define REG_LBA_HIGH   5
#define REG_DRIVE_HEAD 6
#define REG_STATUS     7

#define DEVCTL(base) ((base) + 0x206)

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_IDENTIFY 0xEC
#define CMD_FLUSH    0xE7

#define READY_TIMEOUT_MS 5000
#define DRQ_TIMEOUT_MS   500

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static ata_drive_t drives[ATA_MAX_DRIVES];
static int drive_count = 0;

static uint16_t port_for(int index) {
    return (index < 2) ? ATA_IO_PRIMARY : ATA_IO_SECONDARY;
}

static uint8_t head_for(int index) {
    return (index & 1) ? 0xF0 : 0xE0;
}

static uint8_t status_of(uint16_t base) {
    return inb(base + REG_STATUS);
}

static bool wait_status(uint16_t base, uint8_t want, uint64_t timeout_ms) {
    uint64_t deadline = pit_get_ticks() + timeout_ms;
    for (;;) {
        uint8_t status = status_of(base);
        if (status & ST_ERR) return false;
        if ((status & want) && !(status & ST_BSY)) return true;
        if (pit_get_ticks() >= deadline) return false;
        io_wait();
    }
}

static void select_drive(uint16_t base, uint8_t sel) {
    outb(base + REG_DRIVE_HEAD, sel);
    for (int i = 0; i < 4; i++) {
        io_wait();
        (void)status_of(base);
    }
}

static void reset_channel(uint16_t base) {
    outb(DEVCTL(base), 0x04);
    for (int i = 0; i < 8; i++) io_wait();
    outb(DEVCTL(base), 0x00);
    for (int i = 0; i < 8; i++) io_wait();
    wait_status(base, ST_DRDY, READY_TIMEOUT_MS);
}

static void trim_trailing(char *s, int last) {
    for (int i = last; i >= 0 && s[i] == ' '; i--) s[i] = 0;
}

static void swap_words(char *dst, const uint16_t *src, int words) {
    for (int i = 0; i < words; i++) {
        dst[i * 2]     = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
}

static bool ata_identify(int index) {
    uint16_t base = port_for(index);

    reset_channel(base);
    select_drive(base, head_for(index));

    if (status_of(base) == 0xFF) return false;

    outb(base + REG_SECTOR_CNT, 0);
    outb(base + REG_LBA_LOW, 0);
    outb(base + REG_LBA_MID, 0);
    outb(base + REG_LBA_HIGH, 0);
    io_wait();
    outb(base + REG_STATUS, CMD_IDENTIFY);
    io_wait();

    uint8_t mid = inb(base + REG_LBA_MID);
    uint8_t hi  = inb(base + REG_LBA_HIGH);

    if ((mid == 0x14 && hi == 0xEB) || (mid == 0x69 && hi == 0x96) ||
        (mid == 0x3C && hi == 0xC3)) {
        drives[index].present = true;
        drives[index].atapi = true;
        return false;
    }

    if (!wait_status(base, ST_DRQ, DRQ_TIMEOUT_MS)) return false;

    uint16_t data[256];
    for (int i = 0; i < 256; i++) data[i] = inw(base + REG_DATA);
    io_wait();

    ata_drive_t *d = &drives[index];
    d->present = true;
    d->atapi = false;

    swap_words(d->serial, &data[10], 10);
    swap_words(d->model, &data[27], 20);
    d->serial[20] = 0;
    d->model[40] = 0;
    trim_trailing(d->serial, 19);
    trim_trailing(d->model, 39);

    d->lba48 = (data[83] & 0x10) != 0;
    d->sectors = data[60] | ((uint32_t)data[61] << 16);
    if (d->sectors == 0) d->sectors = data[60];

    return true;
}

bool ata_init(void) {
    drive_count = 0;

    pic_set_mask(14);
    pic_set_mask(15);

    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        if (ata_identify(i)) drive_count++;
    }

    return drive_count > 0;
}

ata_drive_t *ata_get_drive(int index) {
    if (index < 0 || index >= ATA_MAX_DRIVES) return NULL;
    return &drives[index];
}

int ata_drive_count(void) {
    return drive_count;
}

static bool ata_pio(uint16_t base, uint8_t sel, uint8_t cmd, uint32_t lba, uint16_t count, void *buf, bool writing) {
    select_drive(base, sel);

    uint8_t regs[5];
    regs[0] = (uint8_t)count;
    regs[1] = (uint8_t)(lba & 0xFF);
    regs[2] = (uint8_t)((lba >> 8) & 0xFF);
    regs[3] = (uint8_t)((lba >> 16) & 0xFF);
    regs[4] = sel;
    for (int i = 0; i < 5; i++) outb(base + REG_SECTOR_CNT + i, regs[i]);
    io_wait();
    outb(base + REG_STATUS, cmd);
    io_wait();

    uint16_t *data = (uint16_t *)buf;

    for (uint16_t s = 0; s < count; s++) {
        if (!wait_status(base, ST_DRQ, DRQ_TIMEOUT_MS)) return false;

        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
            if (writing) {
                outw(base + REG_DATA, data[s * (ATA_SECTOR_SIZE / 2) + i]);
            } else {
                data[s * (ATA_SECTOR_SIZE / 2) + i] = inw(base + REG_DATA);
            }
        }
        io_wait();
    }

    return wait_status(base, ST_DRDY, READY_TIMEOUT_MS);
}

static void ata_flush(uint16_t base, uint8_t sel) {
    select_drive(base, sel);
    outb(base + REG_STATUS, CMD_FLUSH);
    io_wait();
    wait_status(base, ST_DRDY, READY_TIMEOUT_MS);
}

bool ata_read_sectors(int drive, uint32_t lba, uint16_t count, void *buf) {
    if (drive < 0 || drive >= ATA_MAX_DRIVES || !drives[drive].present) return false;
    if (drives[drive].atapi) return false;
    if (count == 0 || buf == NULL) return false;
    if ((uint64_t)lba + count > ATA_MAX_LBA28) return false;
    if ((uint64_t)lba + count > drives[drive].sectors) return false;

    uint16_t base = port_for(drive);
    uint8_t sel = head_for(drive) | (uint8_t)((lba >> 24) & 0x0F);
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;

    while (done < count) {
        uint16_t n = count - done;
        if (n > ATA_MAX_CHUNK) n = ATA_MAX_CHUNK;
        uint8_t s2 = head_for(drive) | (uint8_t)(((lba + done) >> 24) & 0x0F);
        if (!ata_pio(base, s2, CMD_READ, lba + done, n, p + (uint32_t)done * ATA_SECTOR_SIZE, false)) return false;
        done += n;
    }

    return true;
}

bool ata_write_sectors(int drive, uint32_t lba, uint16_t count, const void *buf) {
    if (drive < 0 || drive >= ATA_MAX_DRIVES || !drives[drive].present) return false;
    if (drives[drive].atapi) return false;
    if (count == 0 || buf == NULL) return false;
    if ((uint64_t)lba + count > ATA_MAX_LBA28) return false;
    if ((uint64_t)lba + count > drives[drive].sectors) return false;

    uint16_t base = port_for(drive);
    uint8_t sel = head_for(drive) | (uint8_t)((lba >> 24) & 0x0F);
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;

    while (done < count) {
        uint16_t n = count - done;
        if (n > ATA_MAX_CHUNK) n = ATA_MAX_CHUNK;
        uint8_t s2 = head_for(drive) | (uint8_t)(((lba + done) >> 24) & 0x0F);
        if (!ata_pio(base, s2, CMD_WRITE, lba + done, n, (void *)(p + (uint32_t)done * ATA_SECTOR_SIZE), true)) return false;
        done += n;
    }

    ata_flush(base, sel);
    return true;
}