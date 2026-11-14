#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include <stdbool.h>
#include "kernel/block/block.h"
#include "kernel/vfs/vfs.h"

extern const struct fs_ops ext2_ops;

bool ext2_mount(block_device_t *dev);
bool ext2_mount_file(const char *path);

#endif
