#include "ata.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/pic/pic.h"
#include "arch/x86_64/pit/pit.h"
#include "kernel/sync/sync.h"

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
#define ST_DF   0x20
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define ATA_SPIN_LIMIT    20000000ULL
#define ATA_RESET_SPINS   2000000ULL

#define ERR_AMNF  0x01
#define ERR_TK0NF 0x02
#define ERR_ABRT  0x04
#define ERR_MCR   0x08
#define ERR_IDNF  0x10
#define ERR_MC    0x20
#define ERR_UNC   0x40
#define ERR_BBK   0x80

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_IDENTIFY 0xEC
#define CMD_FLUSH    0xE7

#define READY_TIMEOUT_MS 5000
#define DRQ_TIMEOUT_MS   500
#define SPINUP_TIMEOUT_MS 30000

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

static spinlock_t ata_lock = SPINLOCK_INIT;

static ata_drive_t drives[ATA_MAX_DRIVES];
static int drive_count = 0;

static ata_error_t last_error = ATA_OK;
static char last_detail[48];

static bool fail(ata_error_t err) {
    last_error = err;
    return false;
}

static void detail(const char *s) {
    uint32_t i = 0;
    for (; s[i] && i < sizeof(last_detail) - 1; i++) last_detail[i] = s[i];
    last_detail[i] = 0;
}

static void decode_err(uint8_t reg) {
    char buf[48];
    uint32_t n = 0;
    const char *s = "none";

    if (reg & ERR_UNC) s = "uncorrectable data";
    else if (reg & ERR_BBK) s = "bad block";
    else if (reg & ERR_IDNF) s = "id not found";
    else if (reg & ERR_MC) s = "media changed";
    else if (reg & ERR_MCR) s = "media change requested";
    else if (reg & ERR_ABRT) s = "command aborted";
    else if (reg & ERR_TK0NF) s = "track 0 not found";
    else if (reg & ERR_AMNF) s = "address mark not found";
    else if (reg) s = "unknown";

    while (*s && n < sizeof(buf) - 8) buf[n++] = *s++;

    uint32_t v = reg;
    buf[n++] = ' ';
    buf[n++] = '(';
    if (v >= 0x10) { buf[n++] = (char)('0' + ((v >> 4) & 0xF)); buf[n++] = (char)('0' + (v & 0xF)); }
    else { buf[n++] = (char)('0' + v); }
    buf[n++] = ')';
    buf[n] = 0;

    detail(buf);
}

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
    uint64_t spins = 0;
    for (;;) {
        uint8_t status = status_of(base);

        if (!(status & ST_BSY)) {
            if (status & (ST_ERR | ST_DF)) {
                decode_err(inb(base + REG_ERR));
                return fail(ATA_EIO);
            }
            if (status & want) return true;
        }

        if (pit_get_ticks() >= deadline || ++spins > ATA_SPIN_LIMIT) {
            detail("status timeout");
            return fail(ATA_ETIMEOUT);
        }
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

static void zero_task_file(uint16_t base) {
    for (int i = 0; i < 5; i++) outb(base + REG_SECTOR_CNT + i, 0);
    io_wait();
}

static void reset_channel(uint16_t base) {
    outb(DEVCTL(base), 0x04);
    for (int i = 0; i < 8; i++) io_wait();
    outb(DEVCTL(base), 0x00);
    for (int i = 0; i < 8; i++) io_wait();

    for (uint64_t i = 0; i < ATA_RESET_SPINS; i++) {
        if (!(status_of(base) & ST_BSY)) break;
        io_wait();
    }
}

static bool recover(uint16_t base) {
    reset_channel(base);
    return true;
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

    uint8_t st = status_of(base);
    for (uint64_t i = 0; st == 0x00 && i < ATA_RESET_SPINS; i++) {
        io_wait();
        st = status_of(base);
    }
    if (st == 0xFF || st == 0x00) return fail(ATA_ENODEV);

    if (st & ST_BSY) {
        if (!wait_status(base, ST_DRDY, SPINUP_TIMEOUT_MS)) {
            reset_channel(base);
            if (!wait_status(base, ST_DRDY, READY_TIMEOUT_MS)) return fail(ATA_ETIMEOUT);
        }
    }

    zero_task_file(base);
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

    d->lba48 = (data[83] & 0x10) != 0 && (data[82] & 0x10) != 0;

    uint32_t cap_lo = (uint32_t)data[60] | ((uint32_t)data[61] << 16);
    uint32_t cap_hi = data[82] & 0x0F;
    cap_hi |= (data[83] & 0x10) ? ((uint32_t)(data[86] & 0x0F) << 16) : 0;
    d->sectors = (cap_hi && ((cap_hi << 16) | cap_lo) != 0)
                     ? (((uint64_t)cap_hi << 32) | cap_lo)
                     : (cap_lo ? (uint64_t)cap_lo : (uint64_t)data[60]);

    d->lba48_sectors = d->lba48
        ? ((uint64_t)data[100]) | ((uint64_t)data[101] << 16) |
          ((uint64_t)data[102] << 32) | ((uint64_t)data[103] << 48)
        : d->sectors;

    return true;
}

bool ata_init(void) {
    lock_acquire(LOCK_ATA, &ata_lock);

    drive_count = 0;
    last_error = ATA_OK;
    detail("");

    pic_set_mask(14);
    pic_set_mask(15);

    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        bool ok = ata_identify(i);
        if (ok) drive_count++;
    }

    lock_release(LOCK_ATA, &ata_lock);

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
    if (((uint64_t)buf & 1) != 0) return fail(ATA_EALIGN);

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
        if (!wait_status(base, ST_DRQ, DRQ_TIMEOUT_MS)) {
            recover(base);
            return false;
        }

        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
            if (writing) {
                outw(base + REG_DATA, data[s * (ATA_SECTOR_SIZE / 2) + i]);
            } else {
                data[s * (ATA_SECTOR_SIZE / 2) + i] = inw(base + REG_DATA);
            }
        }
        io_wait();
    }

    if (!wait_status(base, ST_DRDY, READY_TIMEOUT_MS)) {
        recover(base);
        return false;
    }

    return true;
}

static bool ata_flush(uint16_t base, uint8_t sel) {
    select_drive(base, sel);
    (void)wait_status(base, ST_DRDY, READY_TIMEOUT_MS);
    zero_task_file(base);
    outb(base + REG_STATUS, CMD_FLUSH);
    io_wait();
    return wait_status(base, ST_DRDY, READY_TIMEOUT_MS);
}

bool ata_flush_sectors(int drive) {
    if (drive < 0 || drive >= ATA_MAX_DRIVES || !drives[drive].present) return fail(ATA_ENODEV);
    if (drives[drive].atapi) return fail(ATA_EATAPI);

    lock_acquire(LOCK_ATA, &ata_lock);
    bool ok = ata_flush(port_for(drive), head_for(drive));
    lock_release(LOCK_ATA, &ata_lock);
    return ok;
}

static bool transfer(int drive, uint32_t lba, uint16_t count, void *buf, bool writing) {
    if (drive < 0 || drive >= ATA_MAX_DRIVES) return fail(ATA_EINVAL);
    if (!drives[drive].present) return fail(ATA_ENODEV);
    if (drives[drive].atapi) return fail(ATA_EATAPI);
    if (count == 0 || buf == NULL) return fail(ATA_EINVAL);
    if ((uint64_t)lba + count > ATA_MAX_LBA28) return fail(ATA_ERANGE);
    if ((uint64_t)lba + count > drives[drive].sectors) return fail(ATA_ERANGE);
    if (writing && lba + (uint64_t)count > drives[drive].sectors) return fail(ATA_ERANGE);

    uint16_t base = port_for(drive);
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    bool ok = true;

    lock_acquire(LOCK_ATA, &ata_lock);

    while (done < count) {
        uint16_t n = count - done;
        if (n > ATA_MAX_CHUNK) n = ATA_MAX_CHUNK;

        uint8_t sel = head_for(drive) | (uint8_t)(((lba + done) >> 24) & 0x0F);
        void *dst = p + (uint32_t)done * ATA_SECTOR_SIZE;

        ok = ata_pio(base, sel, writing ? CMD_WRITE : CMD_READ, lba + done, n, dst, writing);
        if (!ok) break;
        done += n;
    }

    if (ok && writing) ok = ata_flush(base, head_for(drive));

    lock_release(LOCK_ATA, &ata_lock);
    return ok;
}

bool ata_read_sectors(int drive, uint32_t lba, uint16_t count, void *buf) {
    return transfer(drive, lba, count, buf, false);
}

bool ata_write_sectors(int drive, uint32_t lba, uint16_t count, const void *buf) {
    return transfer(drive, lba, count, (void *)(uintptr_t)buf, true);
}

ata_error_t ata_last_error(void) {
    return last_error;
}

const char *ata_error_name(ata_error_t err) {
    switch (err) {
        case ATA_OK: return "ok";
        case ATA_EINVAL: return "invalid argument";
        case ATA_ENODEV: return "no such device";
        case ATA_EATAPI: return "not a disk";
        case ATA_ERANGE: return "out of range";
        case ATA_ETIMEOUT: return "timeout";
        case ATA_EIO: return "io error";
        case ATA_EALIGN: return "misaligned buffer";
    }
    return "unknown";
}

const char *ata_error_detail(void) {
    return last_detail;
}
