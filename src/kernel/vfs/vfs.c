#include "kernel/vfs/vfs.h"
#include "kernel/kprintf/kprintf.h"
#include "abi/abi.h"
#include "kernel/mm/pagecache/pcache.h"

#include "kernel/lib/string.h"
#include "kernel/lib/string.h"

#define VFS_MAX_MOUNTS 8

typedef struct {
    char prefix[FS_MAX_NAME];
    size_t plen;
    const struct fs_ops *ops;
} vfs_mount_t;

static vfs_mount_t mounts[VFS_MAX_MOUNTS];
static uint32_t mount_count;
static const struct fs_ops *vfs_default_ops;

static bool path_has_prefix(const char *path, const char *prefix, size_t plen) {
    if (!path || !prefix) return false;
    if (strncasecmp(path, prefix, plen) != 0) return false;
    char p = path[plen];
    return p == 0 || p == '/';
}

bool vfs_set_default(const struct fs_ops *ops) {
    if (!ops) return false;
    vfs_default_ops = ops;
    return true;
}

bool vfs_mount(const char *prefix, const struct fs_ops *ops) {
    if (mount_count >= VFS_MAX_MOUNTS || !prefix || !ops) return false;
    size_t n = strlen(prefix);
    while (n > 1 && prefix[n - 1] == '/') n--;
    if (n == 0 || (n == 1 && prefix[0] == '/')) return false;
    if (n >= FS_MAX_NAME) return false;
    memcpy(mounts[mount_count].prefix, prefix, n);
    mounts[mount_count].prefix[n] = 0;
    mounts[mount_count].plen = n;
    mounts[mount_count].ops = ops;
    mount_count++;
    return true;
}

bool vfs_lookup(const char *path, fs_node_t *out) {
    if (!path || !out) return false;

    size_t best = 0;
    int best_i = -1;
    for (uint32_t i = 0; i < mount_count; i++) {
        size_t L = mounts[i].plen;
        if (path_has_prefix(path, mounts[i].prefix, L)) {
            if (L > best) {
                best = L;
                best_i = (int)i;
            }
        }
    }

    if (best_i >= 0) {
        return mounts[best_i].ops->lookup(path + best, out);
    }

    if (vfs_default_ops && vfs_default_ops->lookup)
        return vfs_default_ops->lookup(path, out);
    return false;
}

static uint32_t default_entry_count(const fs_node_t *dir) {
    fs_node_t tmp;
    uint32_t n = 0;
    while (vfs_default_ops && vfs_default_ops->iterate &&
           vfs_default_ops->iterate(dir, n, &tmp))
        n++;
    return n;
}

bool vfs_iterate(const fs_node_t *dir, uint32_t index, fs_node_t *out) {
    if (!dir || !out || !dir->ops) return false;

    if (dir->ops->iterate && dir->ops->iterate(dir, index, out)) return true;

    if (dir->is_root && dir->ops == vfs_default_ops) {
        uint32_t base = default_entry_count(dir);
        if (index < base) return false;
        uint32_t m = index - base;
        if (m >= mount_count) return false;
        const char *p = mounts[m].prefix;
        while (*p == '/') p++;
        uint32_t n = (uint32_t)strnlen(p, FS_MAX_NAME - 1);
        if (vfs_default_ops && vfs_default_ops->lookup) {
            char bare[FS_MAX_NAME];
            memcpy(bare, p, n);
            bare[n] = 0;
            fs_node_t probe;
            if (vfs_default_ops->lookup(bare, &probe)) return false;
        }
        memset(out->name, 0, FS_MAX_NAME);
        memcpy(out->name, p, n);
        out->attr = 0x10;
        out->cluster = 0;
        out->size = 0;
        out->is_dir = true;
        out->is_root = false;
        out->dir_cluster = 0;
        out->dir_index = 0;
        out->ops = mounts[m].ops;
        out->dev_id = 0;
        return true;
    }

    return false;
}

bool vfs_read(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len) {
    if (!node || !node->ops || !node->ops->read) return false;
    return node->ops->read(node, offset, buf, len);
}

int vfs_write(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len) {
    if (!node || !node->ops || !node->ops->write) return -1;
    pcache_write_through(node, offset, buf, len);
    return node->ops->write(node, offset, buf, len);
}

int vfs_truncate(fs_node_t *node) {
    if (!node || !node->ops || !node->ops->truncate) return -1;
    return node->ops->truncate(node);
}

int vfs_resize(fs_node_t *node, uint32_t size) {
    if (!node || !node->ops || !node->ops->resize) return -1;
    return node->ops->resize(node, size);
}

static int vfs_mount_for(const char *path, const struct fs_ops **ops_out) {
    size_t best = 0;
    int best_i = -1;
    for (uint32_t i = 0; i < mount_count; i++) {
        size_t L = mounts[i].plen;
        if (path_has_prefix(path, mounts[i].prefix, L)) {
            if (L > best) {
                best = L;
                best_i = (int)i;
            }
        }
    }

    if (best_i >= 0) {
        *ops_out = mounts[best_i].ops;
        return (int)best;
    }

    if (vfs_default_ops) {
        *ops_out = vfs_default_ops;
        return 0;
    }
    return -1;
}

int vfs_create(const char *path, fs_node_t *out) {
    const struct fs_ops *ops = NULL;
    int strip = vfs_mount_for(path, &ops);
    if (strip < 0 || !ops || !ops->create) return -ABI_ENOENT;
    return ops->create(path + strip, out);
}

int vfs_mkdir(const char *path, fs_node_t *out) {
    const struct fs_ops *ops = NULL;
    int strip = vfs_mount_for(path, &ops);
    if (strip < 0 || !ops || !ops->mkdir) return -ABI_ENOENT;
    return ops->mkdir(path + strip, out);
}

int vfs_unlink(const char *path) {
    const struct fs_ops *ops = NULL;
    int strip = vfs_mount_for(path, &ops);
    if (strip < 0 || !ops || !ops->unlink) return -ABI_ENOENT;
    return ops->unlink(path + strip);
}

int vfs_rmdir(const char *path) {
    const struct fs_ops *ops = NULL;
    int strip = vfs_mount_for(path, &ops);
    if (strip < 0 || !ops || !ops->rmdir) return -ABI_ENOENT;
    return ops->rmdir(path + strip);
}

int vfs_utimens(const char *path, int64_t atime, int64_t mtime) {
    const struct fs_ops *ops = NULL;
    int strip = vfs_mount_for(path, &ops);
    if (strip < 0 || !ops || !ops->utimens) return -ABI_EROFS;
    return ops->utimens(path + strip, atime, mtime);
}

int vfs_node_utimens(fs_node_t *node, int64_t atime, int64_t mtime) {
    if (!node || !node->ops || !node->ops->node_utimens) return -ABI_EROFS;
    return node->ops->node_utimens(node, atime, mtime);
}

int vfs_link(const char *oldpath, const char *newpath) {
    const struct fs_ops *ops = NULL;
    int strip = vfs_mount_for(newpath, &ops);
    if (strip < 0 || !ops || !ops->link) return -ABI_EXDEV;
    return ops->link(oldpath, newpath + strip);
}

int vfs_rename(const char *oldpath, const char *newpath) {
    const struct fs_ops *ops = NULL;
    const struct fs_ops *nops = NULL;
    int ostrip = vfs_mount_for(oldpath, &ops);
    int nstrip = vfs_mount_for(newpath, &nops);
    if (ostrip < 0 || nstrip < 0 || !ops || !ops->rename) return -ABI_EXDEV;
    if (ops != nops) return -ABI_EXDEV;
    return ops->rename(oldpath + ostrip, newpath + nstrip);
}

int vfs_chown(const char *path, int uid, int gid) {
    const struct fs_ops *ops = NULL;
    int strip = vfs_mount_for(path, &ops);
    if (strip < 0 || !ops || !ops->chown) return -ABI_EROFS;
    fs_node_t node;
    if (!ops->lookup(path + strip, &node)) return -ABI_ENOENT;
    return ops->chown(&node, uid, gid);
}

int vfs_node_chown(fs_node_t *node, int uid, int gid) {
    if (!node || !node->ops || !node->ops->chown) return -ABI_EROFS;
    return node->ops->chown(node, uid, gid);
}

int vfs_statfs(fs_statfs_t *out) {
    if (!out) return -ABI_EINVAL;
    memset(out, 0, sizeof(*out));
    if (vfs_default_ops && vfs_default_ops->statfs) return vfs_default_ops->statfs(out);
    return -ABI_ENOSYS;
}

int fs_node_opened(const fs_node_t *node) {
    if (!node || !node->ops || !node->ops->open) return 0;
    return node->ops->open(node);
}

void fs_node_released(const fs_node_t *node) {
    if (!node || !node->ops || !node->ops->release) return;
    node->ops->release(node);
}

bool vfs_sync(void) {
    pcache_sync_all();
    if (vfs_default_ops && vfs_default_ops->sync)
        return vfs_default_ops->sync();
    return false;
}

uint64_t fs_node_lock_key(const fs_node_t *node) {
    if (!node) return 0;
    if (node->cache_key) return node->cache_key;
    return ((uint64_t)(uintptr_t)node->ops << 32) ^ (uint64_t)node->cluster;
}

uint32_t fs_node_read_all(const fs_node_t *node, void *buf, uint32_t max) {
    if (!node || !buf) return 0;
    uint32_t want = node->size;
    if (want > max) want = max;
    if (want == 0) return 0;
    if (!vfs_read(node, 0, buf, want)) return 0;
    return want;
}

bool fs_lookup(const char *path, fs_node_t *out) {
    return vfs_lookup(path, out);
}

bool fs_iterate(const fs_node_t *dir, uint32_t index, fs_node_t *out) {
    return vfs_iterate(dir, index, out);
}

bool fs_node_read(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len) {
    return vfs_read(node, offset, buf, len);
}

int fs_node_write(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len) {
    return vfs_write(node, offset, buf, len);
}

int fs_node_truncate(fs_node_t *node) {
    return vfs_truncate(node);
}

int fs_node_resize(fs_node_t *node, uint32_t size) {
    return vfs_resize(node, size);
}

int fs_create(const char *path, fs_node_t *out) {
    return vfs_create(path, out);
}

int fs_mkdir(const char *path, fs_node_t *out) {
    return vfs_mkdir(path, out);
}

int fs_unlink(const char *path) {
    return vfs_unlink(path);
}

int fs_rmdir(const char *path) {
    return vfs_rmdir(path);
}

int fs_utimens(const char *path, int64_t atime, int64_t mtime) {
    return vfs_utimens(path, atime, mtime);
}

int fs_node_utimens(fs_node_t *node, int64_t atime, int64_t mtime) {
    return vfs_node_utimens(node, atime, mtime);
}

int fs_link(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return ABI_EINVAL;
    return vfs_link(oldpath, newpath);
}

int fs_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return ABI_EINVAL;
    return vfs_rename(oldpath, newpath);
}

int fs_chown(const char *path, int uid, int gid) {
    if (!path) return ABI_EINVAL;
    return vfs_chown(path, uid, gid);
}

int fs_node_chown(fs_node_t *node, int uid, int gid) {
    return vfs_node_chown(node, uid, gid);
}

int fs_statfs(fs_statfs_t *out) {
    return vfs_statfs(out);
}

bool fs_sync(void) {
    return vfs_sync();
}


bool vfs_readlink(const char *path, char *buf, uint32_t size) {
    if (!path || !buf || !size) return false;

    const struct fs_ops *ops = NULL;
    int strip = vfs_mount_for(path, &ops);
    if (strip < 0 || !ops || !ops->readlink) return false;

    return ops->readlink(path + strip, buf, size);
}

int vfs_symlink(const char *target, const char *path) {
    const struct fs_ops *ops = NULL;
    int strip = vfs_mount_for(path, &ops);
    if (strip < 0 || !ops || !ops->symlink) return -ABI_EOPNOTSUPP;
    return ops->symlink(target, path + strip, NULL);
}

int fs_symlink(const char *target, const char *path) {
    if (!target || !path) return -ABI_EINVAL;
    return vfs_symlink(target, path);
}

int fs_readlink(const char *path, char *buf, uint32_t size) {
    if (!path || !buf || !size) return ABI_EINVAL;
    if (!vfs_readlink(path, buf, size)) return ABI_EINVAL;
    return (int)strnlen(buf, size);
}
