#include "block.h"
#include "drivers/ata/ata.h"

static uint32_t shift_for(uint32_t size) {
    uint32_t s = 0;
    while (((uint32_t)1 << s) < size) s++;
    return s;
}

bool block_valid(const block_device_t *dev, uint64_t lba, uint32_t count) {
    if (!dev || !dev->read || dev->sector_size == 0) return false;
    if (count == 0) return false;
    if (lba > dev->sector_count) return false;
    if ((uint64_t)count > dev->sector_count - lba) return false;
    return true;
}

bool block_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!block_valid(dev, lba, count)) return false;
    return dev->read(dev->ctx, lba, count, buf);
}

bool block_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (dev->read_only) return false;
    if (!dev->write) return false;
    if (!block_valid(dev, lba, count)) return false;
    return dev->write(dev->ctx, lba, count, buf);
}

bool block_flush(block_device_t *dev) {
    if (dev->read_only || !dev->flush) return true;
    return dev->flush(dev->ctx);
}

static bool ata_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    int drive = (int)(intptr_t)ctx;
    uint32_t done = 0;
    uint8_t *p = (uint8_t *)buf;

    if (lba > ATA_MAX_LBA28 || (uint64_t)count > ATA_MAX_LBA28 - lba) return false;

    while (done < count) {
        uint32_t n = count - done;
        if (n > ATA_MAX_CHUNK) n = ATA_MAX_CHUNK;
        if (!ata_read_sectors(drive, (uint32_t)(lba + done), (uint16_t)n, p + (uint32_t)done * ATA_SECTOR_SIZE)) return false;
        done += n;
    }
    return true;
}

static bool ata_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    int drive = (int)(intptr_t)ctx;
    uint32_t done = 0;
    const uint8_t *p = (const uint8_t *)buf;

    if (lba > ATA_MAX_LBA28 || (uint64_t)count > ATA_MAX_LBA28 - lba) return false;

    while (done < count) {
        uint32_t n = count - done;
        if (n > ATA_MAX_CHUNK) n = ATA_MAX_CHUNK;
        if (!ata_write_sectors(drive, (uint32_t)(lba + done), (uint16_t)n, p + (uint32_t)done * ATA_SECTOR_SIZE)) return false;
        done += n;
    }
    return true;
}

static bool ata_flush(void *ctx) {
    return ata_flush_sectors((int)(intptr_t)ctx);
}

bool block_bind_ata(block_device_t *out, int drive) {
    ata_drive_t *d = ata_get_drive(drive);
    if (!out || !d || !d->present || d->atapi) return false;

    out->sector_size = ATA_SECTOR_SIZE;
    out->sector_shift = shift_for(ATA_SECTOR_SIZE);
    out->sector_count = d->sectors;
    out->read_only = false;
    out->ctx = (void *)(intptr_t)drive;
    out->read = ata_read;
    out->write = ata_write;
    out->flush = ata_flush;
    return true;
}

static bool part_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    partition_t *p = (partition_t *)ctx;
    return block_read(p->parent, p->start + lba, count, buf);
}

static bool part_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    partition_t *p = (partition_t *)ctx;
    return block_write(p->parent, p->start + lba, count, buf);
}

static bool part_flush(void *ctx) {
    partition_t *p = (partition_t *)ctx;
    return block_flush(p->parent);
}

bool partition_init(partition_t *part, block_device_t *parent, uint64_t start_lba, uint64_t sector_count) {
    if (!part || !parent || !parent->read) return false;
    if (start_lba > parent->sector_count) return false;
    if (sector_count == 0 || sector_count > parent->sector_count - start_lba) return false;

    part->parent = parent;
    part->start = start_lba;
    part->dev.sector_size = parent->sector_size;
    part->dev.sector_shift = parent->sector_shift;
    part->dev.sector_count = sector_count;
    part->dev.read_only = parent->read_only;
    part->dev.ctx = part;
    part->dev.read = part_read;
    part->dev.write = parent->write ? part_write : NULL;
    part->dev.flush = parent->flush ? part_flush : NULL;
    return true;
}

bool mbr_is_fat(uint8_t type) {
    switch (type) {
        case 0x01:
        case 0x04:
        case 0x06:
        case 0x0B:
        case 0x0C:
        case 0x0E:
        case 0x11:
        case 0x14:
        case 0x16:
        case 0x1B:
        case 0x1C:
            return true;
        default:
            return false;
    }
}

int mbr_parse(block_device_t *dev, mbr_entry_t *out, int max) {
    if (!dev || !out || max <= 0) return 0;
    if (dev->sector_size < 512) return 0;

    uint8_t sec[512];
    if (!block_read(dev, 0, 1, sec)) return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;

    int found = 0;
    for (int i = 0; i < 4 && found < max; i++) {
        const uint8_t *e = sec + 446 + i * 16;

        uint8_t type = e[4];
        if (type == 0x00 || type == 0xEE) continue;

        uint32_t start = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                         ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t count = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                         ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);

        if (count == 0) continue;
        if ((uint64_t)start >= dev->sector_count) continue;
        if ((uint64_t)count > dev->sector_count - start) continue;

        out[found].bootable = (e[0] == 0x80) ? 1 : 0;
        out[found].type = type;
        out[found].start_lba = start;
        out[found].sector_count = count;
        found++;
    }
    return found;
}
