#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ATA_SECTOR_SIZE 512
#define ATA_MAX_LBA28   0x10000000ULL
#define ATA_MAX_DRIVES  4
#define ATA_MAX_CHUNK   256

typedef enum {
    ATA_OK = 0,
    ATA_EINVAL,
    ATA_ENODEV,
    ATA_EATAPI,
    ATA_ERANGE,
    ATA_ETIMEOUT,
    ATA_EIO,
    ATA_EALIGN
} ata_error_t;

typedef struct {
    bool present;
    bool atapi;
    char model[41];
    char serial[21];
    uint64_t sectors;
    bool lba48;
    uint64_t lba48_sectors;
} ata_drive_t;

bool ata_init(void);
ata_drive_t *ata_get_drive(int index);
int ata_drive_count(void);
bool ata_read_sectors(int drive, uint32_t lba, uint16_t count, void *buf);
bool ata_write_sectors(int drive, uint32_t lba, uint16_t count, const void *buf);
bool ata_flush_sectors(int drive);

ata_error_t ata_last_error(void);
const char *ata_error_name(ata_error_t err);
const char *ata_error_detail(void);

#endif
