#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ATA_SECTOR_SIZE 512
#define ATA_MAX_LBA28   0x0FFFFFFFULL
#define ATA_MAX_DRIVES  4
#define ATA_MAX_CHUNK   256

typedef struct {
    bool present;
    bool atapi;
    char model[41];
    char serial[21];
    uint32_t sectors;
    bool lba48;
} ata_drive_t;

bool ata_init(void);
ata_drive_t *ata_get_drive(int index);
int ata_drive_count(void);
bool ata_read_sectors(int drive, uint32_t lba, uint16_t count, void *buf);
bool ata_write_sectors(int drive, uint32_t lba, uint16_t count, const void *buf);

#endif