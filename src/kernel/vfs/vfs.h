#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FS_MAX_NAME  13
#define FS_PATH_MAX  256

typedef struct {
    char name[FS_MAX_NAME];
    uint8_t attr;
    uint32_t cluster;
    uint32_t size;
    bool is_dir;
    bool is_root;
    uint32_t dir_cluster;
    uint32_t dir_index;
    const struct fs_ops *ops;
    uint32_t dev_id;
} fs_node_t;

struct fs_ops {
    bool (*iterate)(const fs_node_t *dir, uint32_t index, fs_node_t *out);
    bool (*lookup)(const char *path, fs_node_t *out);
    bool (*read)(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len);
    int (*write)(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len);
    int (*truncate)(fs_node_t *node);
    int (*resize)(fs_node_t *node, uint32_t size);
    int (*create)(const char *path, fs_node_t *out);
    int (*mkdir)(const char *path, fs_node_t *out);
    int (*unlink)(const char *path);
    int (*rmdir)(const char *path);
    int (*utimens)(const char *path, int64_t mtime);
    int (*node_utimens)(fs_node_t *node, int64_t mtime);
    bool (*sync)(void);
};

bool vfs_mount(const char *prefix, const struct fs_ops *ops);
bool vfs_set_default(const struct fs_ops *ops);
uint32_t fs_node_read_all(const fs_node_t *node, void *buf, uint32_t max);

bool fs_lookup(const char *path, fs_node_t *out);
bool fs_iterate(const fs_node_t *dir, uint32_t index, fs_node_t *out);
bool fs_node_read(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len);
int fs_node_write(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len);
int fs_node_truncate(fs_node_t *node);
int fs_node_resize(fs_node_t *node, uint32_t size);
int fs_create(const char *path, fs_node_t *out);
int fs_mkdir(const char *path, fs_node_t *out);
int fs_unlink(const char *path);
int fs_rmdir(const char *path);
int fs_utimens(const char *path, int64_t mtime);
int fs_node_utimens(fs_node_t *node, int64_t mtime);
bool fs_sync(void);

int fs_node_chattr(fs_node_t *node, uint8_t attr);
fs_node_t *fs_node_ref(const fs_node_t *n);
void fs_node_unref(fs_node_t *n);
bool vfs_lookup(const char *path, fs_node_t *out);
bool vfs_iterate(const fs_node_t *dir, uint32_t index, fs_node_t *out);
bool vfs_read(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len);
int vfs_write(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len);
int vfs_truncate(fs_node_t *node);
int vfs_resize(fs_node_t *node, uint32_t size);
int vfs_create(const char *path, fs_node_t *out);
int vfs_mkdir(const char *path, fs_node_t *out);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_utimens(const char *path, int64_t mtime);
int vfs_node_utimens(fs_node_t *node, int64_t mtime);
bool vfs_sync(void);

bool devfs_init(void);
const struct fs_ops *devfs_ops(void);

#define DEVFS_DEV_FB0   1
#define DEVFS_DEV_EVENT0 2
#define DEVFS_DEV_ZERO  3
#define DEVFS_DEV_NULL  4
#define DEVFS_DEV_CONSOLE 5
#define DEVFS_DEV_TTY0 6

#endif
