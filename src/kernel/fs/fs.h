#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel/block/block.h"
#include "kernel/vfs/vfs.h"
#include "abi/abi.h"

#define FS_SECTOR_SIZE   512
#define FS_DIRENT_SIZE   32
#define FS_FIRST_CLUSTER 2
#define FS_EOC_MIN       0xFFF8
#define FS_BAD_CLUSTER   0xFFF7

#define FS_ATTR_READ_ONLY 0x01
#define FS_ATTR_HIDDEN    0x02
#define FS_ATTR_SYSTEM    0x04
#define FS_ATTR_VOLUME_ID 0x08
#define FS_ATTR_DIRECTORY 0x10
#define FS_ATTR_LFN       0x0F
#define FS_ATTR_ARCHIVE   0x20

typedef struct {
    block_device_t *dev;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint32_t total_sectors;
    uint32_t sectors_per_fat;
    uint32_t fat_start;
    uint32_t root_start;
    uint32_t root_sectors;
    uint32_t data_start;
    uint32_t cluster_count;
    uint16_t root_capacity;
    char label[12];
    char fs_type[9];
    bool mounted;
} fs_volume_t;

bool fs_mount(block_device_t *dev);
bool fs_is_mounted(void);
fs_volume_t *fs_get(void);

bool fs_name_eq(const char *a, const char *b);
const char *fs_fat_name(uint32_t cluster_count);

extern const struct fs_ops fat_ops;

#endif
