#include "kernel/vfs/vfs.h"
#include "kernel/kprintf/kprintf.h"

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
    for (size_t i = 0; i < plen; i++) {
        char a = path[i], b = prefix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return false;
        if (path[i] == 0) return false;
    }
    char p = path[plen];
    if (p >= 'a' && p <= 'z') p = (char)(p - 32);
    return p == 0 || p == '/';
}

bool vfs_set_default(const struct fs_ops *ops) {
    if (!ops) return false;
    vfs_default_ops = ops;
    return true;
}

bool vfs_mount(const char *prefix, const struct fs_ops *ops) {
    if (mount_count >= VFS_MAX_MOUNTS || !prefix || !ops) return false;
    size_t n = 0;
    while (prefix[n]) n++;
    while (n > 1 && prefix[n - 1] == '/') n--;
    if (n == 0 || (n == 1 && prefix[0] == '/')) return false;
    if (n >= FS_MAX_NAME) return false;
    for (size_t i = 0; i < n; i++) mounts[mount_count].prefix[i] = prefix[i];
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
        uint32_t n = 0;
        while (p[n] && n < FS_MAX_NAME - 1) n++;
        if (vfs_default_ops && vfs_default_ops->lookup) {
            char bare[FS_MAX_NAME];
            for (uint32_t i = 0; i < n; i++) bare[i] = p[i];
            bare[n] = 0;
            fs_node_t probe;
            if (vfs_default_ops->lookup(bare, &probe)) return false;
        }
        for (uint32_t i = 0; i < FS_MAX_NAME; i++) out->name[i] = 0;
        for (uint32_t i = 0; i < n; i++) out->name[i] = p[i];
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

int vfs_create(const char *path, fs_node_t *out) {
    if (vfs_default_ops && vfs_default_ops->create)
        return vfs_default_ops->create(path, out);
    return -1;
}

int vfs_mkdir(const char *path, fs_node_t *out) {
    if (vfs_default_ops && vfs_default_ops->mkdir)
        return vfs_default_ops->mkdir(path, out);
    return -1;
}

int vfs_unlink(const char *path) {
    if (vfs_default_ops && vfs_default_ops->unlink)
        return vfs_default_ops->unlink(path);
    return -1;
}

int vfs_rmdir(const char *path) {
    if (vfs_default_ops && vfs_default_ops->rmdir)
        return vfs_default_ops->rmdir(path);
    return -1;
}

int vfs_utimens(const char *path, int64_t mtime) {
    if (vfs_default_ops && vfs_default_ops->utimens)
        return vfs_default_ops->utimens(path, mtime);
    return -1;
}

int vfs_node_utimens(fs_node_t *node, int64_t mtime) {
    if (!node || !node->ops || !node->ops->node_utimens) return -1;
    return node->ops->node_utimens(node, mtime);
}

bool vfs_sync(void) {
    if (vfs_default_ops && vfs_default_ops->sync)
        return vfs_default_ops->sync();
    return false;
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

int fs_utimens(const char *path, int64_t mtime) {
    return vfs_utimens(path, mtime);
}

int fs_node_utimens(fs_node_t *node, int64_t mtime) {
    return vfs_node_utimens(node, mtime);
}

bool fs_sync(void) {
    return vfs_sync();
}
