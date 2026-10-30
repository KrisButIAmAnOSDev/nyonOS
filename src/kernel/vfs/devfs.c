#include "kernel/vfs/vfs.h"
#include "graphics/video/video.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/uaccess/uaccess.h"

#define DEVFS_MAX_ENTRIES 8

typedef struct {
    const char *name;
    uint32_t dev_id;
    bool is_dir;
    uint64_t size;
} devfs_entry_t;

static const devfs_entry_t devfs_root_entries[] = {
    { "FB0", DEVFS_DEV_FB0, false, 0 },
    { "ZERO", DEVFS_DEV_ZERO, false, 0 },
    { "NULL", DEVFS_DEV_NULL, false, 0 },
    { "CONSOLE", DEVFS_DEV_CONSOLE, false, 0 },
    { "TTY0", DEVFS_DEV_TTY0, false, 0 },
    { "TTY", DEVFS_DEV_TTY, false, 0 },
    { "INPUT", 0, true, 0 },
};

static const devfs_entry_t devfs_input_entries[] = {
    { "EVENT0", DEVFS_DEV_EVENT0, false, 0 },
};

static void devfs_fill(fs_node_t *out, const devfs_entry_t *e) {
    for (uint32_t i = 0; i < FS_MAX_NAME; i++) out->name[i] = 0;
    for (uint32_t i = 0; i < FS_MAX_NAME - 1 && e->name[i]; i++) out->name[i] = e->name[i];
    out->attr = e->is_dir ? 0x10 : 0x20;
    out->cluster = 0;
    out->size = (uint32_t)e->size;
    out->is_dir = e->is_dir;
    out->is_root = false;
    out->dir_cluster = 0;
    out->dir_index = 0;
    out->ops = devfs_ops();
    out->dev_id = e->dev_id;
}

static const devfs_entry_t *devfs_table(const fs_node_t *dir, uint32_t *n_out) {
    if (!dir || !dir->is_dir) return NULL;
    if (dir->is_root) {
        *n_out = 7;
        return devfs_root_entries;
    }
    *n_out = 1;
    return devfs_input_entries;
}

static bool devfs_find(const fs_node_t *dir, const char *name, fs_node_t *out) {
    if (!dir || !out) return false;

    uint32_t n;
    const devfs_entry_t *tab = devfs_table(dir, &n);
    if (!tab) return false;

    for (uint32_t i = 0; i < n; i++) {
        const char *a = tab[i].name;
        const char *b = name;
        uint32_t j = 0;
        while (a[j] && b[j] && (a[j] | 0x20) == (b[j] | 0x20)) j++;
        if (!a[j] && !b[j]) {
            devfs_fill(out, &tab[i]);
            return true;
        }
    }
    return false;
}

static bool devfs_iterate(const fs_node_t *dir, uint32_t index, fs_node_t *out) {
    if (!dir || !out) return false;

    uint32_t n;
    const devfs_entry_t *tab = devfs_table(dir, &n);
    if (!tab) return false;

    if (index >= n) return false;
    devfs_fill(out, &tab[index]);
    return true;
}

static bool devfs_lookup(const char *path, fs_node_t *out) {
    if (!out) return false;

    fs_node_t cur;
    cur.name[0] = 0;
    cur.attr = 0x10;
    cur.cluster = 0;
    cur.size = 0;
    cur.is_dir = true;
    cur.is_root = true;
    cur.dir_cluster = 0;
    cur.dir_index = 0;
    cur.ops = devfs_ops();
    cur.dev_id = 0;

    const char *p = path;
    while (*p == '/') p++;
    if (!*p) {
        *out = cur;
        return true;
    }

    char comp[FS_MAX_NAME + 2];
    for (;;) {
        uint32_t n = 0;
        while (p[n] && p[n] != '/') n++;
        if (n == 0 || n >= FS_MAX_NAME + 1) return false;
        for (uint32_t i = 0; i < n; i++) comp[i] = p[i];
        comp[n] = 0;
        p += n;
        while (*p == '/') p++;

        fs_node_t found;
        if (!devfs_find(&cur, comp, &found)) return false;
        cur = found;

        if (!*p) {
            *out = cur;
            return true;
        }
        if (!cur.is_dir) return false;
    }
}

static bool devfs_read(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len) {
    if (!node || !buf) return false;
    if (node->is_dir) return false;
    if (len == 0) return true;

    switch (node->dev_id) {
        case DEVFS_DEV_ZERO: {
            uint8_t *b = (uint8_t *)buf;
            for (uint32_t i = 0; i < len; i++) b[i] = 0;
            return true;
        }
        case DEVFS_DEV_NULL:
            return false;
        case DEVFS_DEV_FB0: {
            struct sys_fb info;
            if (!video_query(&info)) return false;
            uint64_t total = (uint64_t)info.pitch * info.height;
            if ((uint64_t)offset >= total) return false;
            if ((uint64_t)len > total - offset) len = (uint32_t)(total - offset);
            const uint8_t *fb = (const uint8_t *)(vmm_hhdm_offset +
                                (info.phys & ~(uint64_t)(PAGE_SIZE - 1)));
            return copyout(buf, fb + offset, len);
        }
        default:
            return false;
    }
}

static int devfs_write(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len) {
    if (!node || !buf) return -1;
    if (node->is_dir) return -1;

    switch (node->dev_id) {
        case DEVFS_DEV_ZERO:
        case DEVFS_DEV_NULL:
            return (int)len;
        case DEVFS_DEV_FB0: {
            struct sys_fb info;
            if (!video_query(&info)) return -1;
            uint64_t total = (uint64_t)info.pitch * info.height;
            if ((uint64_t)offset >= total) return -1;
            if ((uint64_t)len > total - offset) len = (uint32_t)(total - offset);
            uint8_t *fb = (uint8_t *)(vmm_hhdm_offset +
                          (info.phys & ~(uint64_t)(PAGE_SIZE - 1)));
            if (!copyin(fb + offset, buf, len)) return -1;
            return (int)len;
        }
        default:
            return -1;
    }
}

static const struct fs_ops devfs_ops_table = {
    .iterate = devfs_iterate,
    .lookup = devfs_lookup,
    .read = devfs_read,
    .write = devfs_write,
};

const struct fs_ops *devfs_ops(void) {
    return &devfs_ops_table;
}

bool devfs_init(void) {
    return vfs_mount("/DEV", &devfs_ops_table);
}
