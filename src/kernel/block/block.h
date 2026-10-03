#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BLOCK_MAX_PARTITIONS 16

typedef struct block_device block_device_t;

typedef bool (*block_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);
typedef bool (*block_write_fn)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
typedef bool (*block_flush_fn)(void *ctx);

struct block_device {
    uint32_t sector_size;
    uint32_t sector_shift;
    uint64_t sector_count;
    bool read_only;
    void *ctx;
    block_read_fn read;
    block_write_fn write;
    block_flush_fn flush;
};

bool block_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf);
bool block_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf);
bool block_flush(block_device_t *dev);
bool block_valid(const block_device_t *dev, uint64_t lba, uint32_t count);

bool block_bind_ata(block_device_t *out, int drive);

typedef struct {
    block_device_t dev;
    block_device_t *parent;
    uint64_t start;
} partition_t;

bool partition_init(partition_t *part, block_device_t *parent, uint64_t start_lba, uint64_t sector_count);

typedef struct {
    uint8_t bootable;
    uint8_t type;
    uint64_t start_lba;
    uint64_t sector_count;
} mbr_entry_t;

int mbr_parse(block_device_t *dev, mbr_entry_t *out, int max);
bool mbr_is_fat(uint8_t type);

#endif
