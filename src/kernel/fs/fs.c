#include "fs.h"
#include "kernel/block/block.h"
#include "kernel/sync/preempt.h"
#include "kernel/sync/sync.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/rtc/rtc.h"
#include "arch/x86_64/cpu/cpu.h"

#include "kernel/lib/string.h"
#include "kernel/lib/string.h"

int toupper(int c);

static void fat_stamp(int64_t unix_sec, uint16_t *out_time, uint16_t *out_date);

static spinlock_t fs_lock = SPINLOCK_INIT;
#define FS_SCAN_MAX 4096
#define FS_INODE_CACHE 64

static struct {
    bool used;
    fs_node_t node;
    uint32_t refs;
} fs_inodes[FS_INODE_CACHE];

static bool fs_inode_same(const fs_node_t *a, const fs_node_t *b) {
    if (a->is_root || b->is_root) return a->is_root == b->is_root;
    return a->dir_cluster == b->dir_cluster && a->dir_index == b->dir_index;
}

fs_node_t *fs_node_ref(const fs_node_t *n) {
    if (!n) return NULL;
    lock_acquire(LOCK_FS, &fs_lock);
    for (uint32_t i = 0; i < FS_INODE_CACHE; i++) {
        if (fs_inodes[i].used && fs_inode_same(&fs_inodes[i].node, n)) {
            fs_inodes[i].node = *n;
            fs_inodes[i].refs++;
            fs_node_t *r = &fs_inodes[i].node;
            lock_release(LOCK_FS, &fs_lock);
            return r;
        }
    }
    for (uint32_t i = 0; i < FS_INODE_CACHE; i++) {
        if (!fs_inodes[i].used) {
            fs_inodes[i].used = true;
            fs_inodes[i].node = *n;
            fs_inodes[i].refs = 1;
            fs_node_t *r = &fs_inodes[i].node;
            lock_release(LOCK_FS, &fs_lock);
            return r;
        }
    }
    lock_release(LOCK_FS, &fs_lock);
    return NULL;
}

void fs_node_unref(fs_node_t *n) {
    if (!n) return;
    lock_acquire(LOCK_FS, &fs_lock);
    for (uint32_t i = 0; i < FS_INODE_CACHE; i++) {
        if (&fs_inodes[i].node == n) {
            if (fs_inodes[i].refs > 0) fs_inodes[i].refs--;
            if (fs_inodes[i].refs == 0) fs_inodes[i].used = false;
            break;
        }
    }
    lock_release(LOCK_FS, &fs_lock);
}

static fs_volume_t fs;

static uint8_t fs_sectors[MAX_CPUS][FS_SECTOR_SIZE] __attribute__((aligned(4)));

#define sec (fs_sectors[cpu_id()])

#define FAT_RCACHE_SLOTS 8

struct fat_read_cache {
    const fs_node_t *node;
    uint32_t dir_cluster;
    uint32_t dir_index;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t cluster_index;
    uint32_t cluster;
    uint32_t generation;
};

static struct fat_read_cache fat_rcache[MAX_CPUS][FAT_RCACHE_SLOTS];
static uint32_t fat_cache_generation;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool is_pow2(uint32_t v) {
    return v && !(v & (v - 1));
}

static bool sector_read(uint32_t lba) {
    return block_read(fs.dev, lba, 1, sec);
}


static uint8_t dir_sector_cache[FS_SECTOR_SIZE] __attribute__((aligned(4)));
static uint32_t dir_sector_lba = 0xFFFFFFFFu;
static uint32_t dir_sector_base = 0xFFFFFFFFu;

static const uint8_t *dir_sector(uint32_t lba) {
    if (fs.bytes_per_sector != FS_SECTOR_SIZE) {
        return sector_read(lba) ? sec : NULL;
    }
    if (lba != dir_sector_lba || fs.data_start != dir_sector_base) {
        if (!sector_read(lba)) return NULL;
        memcpy(dir_sector_cache, sec, FS_SECTOR_SIZE);
        dir_sector_lba = lba;
        dir_sector_base = fs.data_start;
    }
    return dir_sector_cache;
}

const char *fs_fat_name(uint32_t cluster_count) {
    if (cluster_count < 4085) return "FAT12";
    if (cluster_count < 65525) return "FAT16";
    if (cluster_count < 4194304) return "FAT32";
    return "unknown";
}

bool fs_is_mounted(void) {
    return fs.mounted;
}

fs_volume_t *fs_get(void) {
    return &fs;
}

bool fs_mount_inner(block_device_t *dev) {
    fs_volume_t *f = &fs;

    f->mounted = false;
    f->dev = dev;

    if (!dev || !dev->read) return false;
    if (!sector_read(0)) return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;

    f->bytes_per_sector = rd16(sec + 0x0B);
    f->sectors_per_cluster = sec[0x0D];
    f->reserved_sectors = rd16(sec + 0x0E);
    f->fat_count = sec[0x10];
    f->root_entry_count = rd16(sec + 0x11);

    uint16_t total16 = rd16(sec + 0x13);
    uint32_t total32 = rd32(sec + 0x20);
    f->total_sectors = total16 ? total16 : total32;

    uint16_t spf16 = rd16(sec + 0x16);
    f->sectors_per_fat = spf16 ? spf16 : rd32(sec + 0x24);

    for (int i = 0; i < 11; i++) f->label[i] = (char)sec[0x2B + i];
    f->label[11] = 0;
    for (int i = 0; i < 8; i++) f->fs_type[i] = (char)sec[0x36 + i];
    f->fs_type[8] = 0;

    if (f->bytes_per_sector != FS_SECTOR_SIZE) return false;
    if (!is_pow2(f->sectors_per_cluster)) return false;
    if (f->sectors_per_cluster > 128) return false;
    if (f->reserved_sectors == 0) return false;
    if (f->fat_count == 0) return false;
    if (f->sectors_per_fat == 0) return false;
    if (f->total_sectors == 0) return false;
    if (f->root_entry_count == 0) return false;

    f->fat_start = f->reserved_sectors;
    f->root_start = f->fat_start + (uint32_t)f->fat_count * f->sectors_per_fat;
    f->root_sectors = ((uint32_t)f->root_entry_count * FS_DIRENT_SIZE + f->bytes_per_sector - 1) / f->bytes_per_sector;
    f->root_capacity = f->root_entry_count;
    f->data_start = f->root_start + f->root_sectors;

    if (f->data_start >= f->total_sectors) return false;

    f->cluster_count = (f->total_sectors - f->data_start) / f->sectors_per_cluster;
    if (f->cluster_count == 0) return false;
    if (f->cluster_count >= FS_BAD_CLUSTER) return false;
    if (f->cluster_count < 4085) return false;

    f->mounted = true;
    return true;
}

bool fs_mount(block_device_t *dev) {
    lock_acquire(LOCK_FS, &fs_lock);
    bool r = fs_mount_inner(dev);
    if (r) vfs_set_default(&fat_ops);
    lock_release(LOCK_FS, &fs_lock);
    if (r) devfs_init();
    return r;
}

static uint32_t cluster_lba(uint32_t cluster) {
    return fs.data_start + (cluster - FS_FIRST_CLUSTER) * fs.sectors_per_cluster;
}

static uint8_t fat_sector_cache[FS_SECTOR_SIZE] __attribute__((aligned(4)));
static uint32_t fat_sector_lba = 0xFFFFFFFFu;
static uint32_t fat_sector_base = 0xFFFFFFFFu;

static const uint8_t *fat_sector(uint32_t lba) {
    if (fs.bytes_per_sector != FS_SECTOR_SIZE) {
        return sector_read(lba) ? sec : NULL;
    }
    if (lba != fat_sector_lba || fs.fat_start != fat_sector_base) {
        if (!sector_read(lba)) return NULL;
        memcpy(fat_sector_cache, sec, FS_SECTOR_SIZE);
        fat_sector_lba = lba;
        fat_sector_base = fs.fat_start;
    }
    return fat_sector_cache;
}

static bool sector_write(uint32_t lba, const void *buf) {
    dir_sector_lba = 0xFFFFFFFFu;
    fat_sector_lba = 0xFFFFFFFFu;
    return block_write(fs.dev, lba, 1, buf);
}

static bool cluster_ok(uint32_t cluster) {
    if (cluster < FS_FIRST_CLUSTER || cluster >= FS_BAD_CLUSTER) return false;
    if (!fs.mounted) return false;
    return cluster < FS_FIRST_CLUSTER + fs.cluster_count;
}

static bool fat_entry(uint32_t cluster, uint16_t *out) {
    if (!cluster_ok(cluster)) return false;
    uint32_t byte = cluster * 2;
    uint32_t lba = fs.fat_start + byte / fs.bytes_per_sector;
    uint32_t off = byte % fs.bytes_per_sector;
    const uint8_t *p = fat_sector(lba);
    if (!p) return false;
    *out = rd16(p + off);
    return true;
}

static bool fat_next(uint32_t cluster, uint32_t *out) {
    uint16_t v;
    if (!fat_entry(cluster, &v)) return false;
    if (v >= FS_EOC_MIN) return false;
    if (v < FS_FIRST_CLUSTER) return false;
    *out = v;
    return true;
}

static bool fat_set(uint32_t cluster, uint16_t value) {
    if (cluster < FS_FIRST_CLUSTER || cluster >= FS_BAD_CLUSTER) return false;
    uint32_t byte = cluster * 2;
    uint32_t off = byte % fs.bytes_per_sector;
    uint8_t buf[FS_SECTOR_SIZE];

    for (uint8_t f = 0; f < fs.fat_count; f++) {
        uint32_t lba = fs.fat_start + (uint32_t)f * fs.sectors_per_fat +
                       byte / fs.bytes_per_sector;
        if (!block_read(fs.dev, lba, 1, buf)) return false;
        buf[off] = (uint8_t)(value & 0xFF);
    buf[off + 1] = (uint8_t)(value >> 8);
        if (!sector_write(lba, buf)) return false;
    }
    fat_cache_generation++;
    return true;
}

static bool fat_alloc(uint32_t *out) {
    uint32_t per_sector = fs.bytes_per_sector / 2;
    uint32_t limit = fs.cluster_count + FS_FIRST_CLUSTER;
    for (uint32_t s = 0; s < fs.sectors_per_fat; s++) {
        uint8_t buf[FS_SECTOR_SIZE];
        if (!block_read(fs.dev, fs.fat_start + s, 1, buf)) return false;
        for (uint32_t i = 0; i < per_sector; i++) {
            uint32_t cluster = s * per_sector + i;
            if (cluster < FS_FIRST_CLUSTER) continue;
            if (cluster >= limit) return false;
            uint16_t v = (uint16_t)(buf[i * 2] | (buf[i * 2 + 1] << 8));
            if (v == 0) { *out = cluster; return true; }
        }
    }
    return false;
}

static bool fat_free_chain(uint32_t cluster) {
    while (cluster >= FS_FIRST_CLUSTER && cluster < FS_BAD_CLUSTER) {
        uint16_t next;
        if (!fat_entry(cluster, &next)) return false;
        if (!fat_set(cluster, 0)) return false;
        if (next >= FS_EOC_MIN) break;
        cluster = next;
    }
    return true;
}

static bool cluster_nth(uint32_t cluster, uint32_t skip, uint32_t *out) {
    if (!cluster_ok(cluster)) return false;
    for (uint32_t i = 0; i < skip; i++) {
        uint32_t nx;
        if (!fat_next(cluster, &nx)) return false;
        cluster = nx;
    }
    *out = cluster;
    return true;
}

static bool fat_read_cluster(const fs_node_t *node, uint32_t target, uint32_t *out) {
    uint32_t slot = (node->dir_index * 2654435761u + node->dir_cluster + node->cluster) % FAT_RCACHE_SLOTS;
    struct fat_read_cache *c = &fat_rcache[cpu_id()][slot];

    bool match = c->generation == fat_cache_generation &&
                 c->node == node && c->dir_cluster == node->dir_cluster &&
                 c->dir_index == node->dir_index && c->first_cluster == node->cluster &&
                 c->size == node->size && c->cluster_index <= target;
    if (match) {
        uint32_t cluster = c->cluster;
        for (uint32_t i = c->cluster_index; i < target; i++) {
            uint32_t nx;
            if (!fat_next(cluster, &nx)) return false;
            cluster = nx;
        }
        c->cluster_index = target;
        c->cluster = cluster;
        *out = cluster;
        return true;
    }

    uint32_t cluster;
    if (!cluster_nth(node->cluster, target, &cluster)) return false;
    c->node = node;
    c->dir_cluster = node->dir_cluster;
    c->dir_index = node->dir_index;
    c->first_cluster = node->cluster;
    c->size = node->size;
    c->cluster_index = target;
    c->cluster = cluster;
    c->generation = fat_cache_generation;
    *out = cluster;
    return true;
}

static bool dirent_locate(const fs_node_t *dir, uint32_t index, uint32_t *lba_out,
                           uint32_t *off_out) {
    if (!dir || !dir->is_dir) return false;
    if (index >= FS_SCAN_MAX) return false;

    uint32_t per_sector = fs.bytes_per_sector / FS_DIRENT_SIZE;

    if (dir->is_root) {
        if (index >= fs.root_entry_count) return false;
        *lba_out = fs.root_start + index / per_sector;
        *off_out = (index % per_sector) * FS_DIRENT_SIZE;
        return true;
    }

    if (dir->cluster == 0) return false;

    uint32_t per_cluster = per_sector * fs.sectors_per_cluster;
    uint32_t cl_index = index / per_cluster;
    uint32_t within = index % per_cluster;

    uint32_t cluster;
    if (!cluster_nth(dir->cluster, cl_index, &cluster)) return false;

    *lba_out = cluster_lba(cluster) + within / per_sector;
    *off_out = (within % per_sector) * FS_DIRENT_SIZE;
    return true;
}

static bool dirent_raw(const fs_node_t *dir, uint32_t index, uint8_t *out) {
    uint32_t lba, off;
    if (!dirent_locate(dir, index, &lba, &off)) return false;
    const uint8_t *p = dir_sector(lba);
    if (!p) return false;
    memcpy(out, p + off, FS_DIRENT_SIZE);
    return true;
}

static bool dirent_store(uint32_t lba, uint32_t off, const uint8_t *e) {
    uint8_t buf[FS_SECTOR_SIZE];
    if (!block_read(fs.dev, lba, 1, buf)) return false;
    memcpy(buf + off, e, FS_DIRENT_SIZE);
    return sector_write(lba, buf);
}

static void dirent_build(uint8_t *e, const char *name83, uint8_t attr, uint32_t cluster,
                         uint32_t size) {
    memset(e, 0, FS_DIRENT_SIZE);
    for (int i = 0; i < 11; i++) e[i] = (uint8_t)(name83[i] ? name83[i] : ' ');
    e[11] = attr;
    uint16_t ft, fdt;
    fat_stamp(rtc_unix_time(), &ft, &fdt);
    e[14] = (uint8_t)(ft & 0xFF);  e[15] = (uint8_t)(ft >> 8);
    e[16] = (uint8_t)(fdt & 0xFF); e[17] = (uint8_t)(fdt >> 8);
    e[18] = (uint8_t)(fdt & 0xFF); e[19] = (uint8_t)(fdt >> 8);
    e[22] = (uint8_t)(ft & 0xFF);  e[23] = (uint8_t)(ft >> 8);
    e[24] = (uint8_t)(fdt & 0xFF); e[25] = (uint8_t)(fdt >> 8);
    e[26] = (uint8_t)(cluster & 0xFF);
    e[27] = (uint8_t)((cluster >> 8) & 0xFF);
    e[28] = (uint8_t)(size & 0xFF);
    e[29] = (uint8_t)((size >> 8) & 0xFF);
    e[30] = (uint8_t)((size >> 16) & 0xFF);
    e[31] = (uint8_t)((size >> 24) & 0xFF);
}

static void trim83(const uint8_t *raw, char *out) {
    char tmp[12];
    memcpy(tmp, raw, 11);
    tmp[11] = 0;

    const char *sp = memchr(tmp, ' ', 8);
    int bn = sp ? (int)(sp - tmp) : 8;
    int en = 0;
    if (tmp[8] != ' ') {
        const char *ep = memchr(tmp + 8, ' ', 3);
        en = ep ? (int)(ep - (tmp + 8)) : 3;
    }

    if (en) snprintf(out, FS_MAX_NAME, "%.*s.%.*s", bn, tmp, en, tmp + 8);
    else snprintf(out, FS_MAX_NAME, "%.*s", bn, tmp);
}

static bool parse_dirent(const uint8_t *e, fs_node_t *out) {
    if (e[0] == 0x00 || e[0] == 0xE5) return false;

    uint8_t attr = e[11];
    if ((attr & FS_ATTR_LFN) == FS_ATTR_LFN) return false;
    if (attr & FS_ATTR_VOLUME_ID) return false;

    trim83(e, out->name);
    if (out->name[0] == 0) return false;
    if (out->name[0] == '.' && (out->name[1] == 0 || (out->name[1] == '.' && out->name[2] == 0))) return false;

    out->attr = attr;
    out->is_dir = (attr & FS_ATTR_DIRECTORY) != 0;
    out->cluster = (uint32_t)rd16(e + 26);
    if (fs.cluster_count >= 65525) out->cluster |= (uint32_t)rd16(e + 20) << 16;
    out->size = out->is_dir ? 0 : rd32(e + 28);
    out->is_root = false;
    out->ops = &fat_ops;
    out->dev_id = 0;

    if (!out->is_dir && !cluster_ok(out->cluster)) out->cluster = 0;
    return true;
}

#define FS_LFN_MAX FS_MAX_NAME

struct lfn_state {
    char name[FS_LFN_MAX];
    uint8_t checksum;
    uint32_t seen;
    uint8_t last;
    bool active;
};

static void lfn_reset(struct lfn_state *st) {
    memset(st, 0, sizeof(*st));
}

static uint8_t sfn_checksum(const uint8_t e[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + e[i]);
    return sum;
}

static bool lfn_parse_entry(const uint8_t *e, struct lfn_state *st) {
    if (e[11] != FS_ATTR_LFN) return false;
    if (e[0] == 0x00 || e[0] == 0xE5) return false;

    uint32_t ord = (uint32_t)(e[0] & 0x1F);
    if (ord < 1 || ord > 20) return false;

    if (e[0] & 0x40) {
        memset(st->name, 0, sizeof(st->name));
        st->checksum = e[13];
        st->seen = 0;
        st->last = (uint8_t)ord;
        st->active = true;
    } else if (!st->active || st->checksum != e[13]) {
        return false;
    }

    st->seen |= 1u << (ord - 1);
    uint32_t base = (ord - 1) * 13;
    for (int k = 0; k < 13; k++) {
        int off = (k < 5) ? (1 + k * 2) : (k < 11 ? (14 + (k - 5) * 2) : (28 + (k - 11) * 2));
        uint16_t u = (uint16_t)(e[off] | (e[off + 1] << 8));
        if (u == 0 || u == 0xFFFF) break;
        if (base + (uint32_t)k < FS_LFN_MAX - 1)
            st->name[base + (uint32_t)k] = (u < 0x80) ? (char)u : '?';
    }
    return true;
}

static bool lfn_valid(const struct lfn_state *st, const uint8_t *short_entry) {
    if (!st->active || st->last == 0) return false;
    if (sfn_checksum(short_entry) != st->checksum) return false;
    uint32_t need = (1u << st->last) - 1u;
    return (st->seen & need) == need;
}

static void lfn_apply(const struct lfn_state *st, fs_node_t *node, const uint8_t *short_entry) {
    if (!lfn_valid(st, short_entry)) return;
    memcpy(node->name, st->name, FS_MAX_NAME);
    node->name[FS_MAX_NAME - 1] = 0;
}

static bool fat_iterate(const fs_node_t *dir, uint32_t index, fs_node_t *out) {
    if (!fs.mounted || !dir || !out) return false;
    if (!dir->is_dir) return false;

    struct lfn_state lfn;
    lfn_reset(&lfn);
    uint32_t live = 0;
    for (uint32_t i = 0; i < FS_SCAN_MAX; i++) {
        uint8_t e[FS_DIRENT_SIZE];
        if (!dirent_raw(dir, i, e)) return false;
        if (e[11] == FS_ATTR_LFN) {
            lfn_parse_entry(e, &lfn);
            continue;
        }
        fs_node_t tmp;
        if (!parse_dirent(e, &tmp)) { lfn_reset(&lfn); continue; }
        lfn_apply(&lfn, &tmp, e);
        if (live == index) {
            *out = tmp;
            return true;
        }
        live++;
        lfn_reset(&lfn);
    }
    return false;
}

static bool root_node(fs_node_t *out) {
    if (!fs.mounted) return false;
    out->name[0] = '/';
    out->name[1] = 0;
    out->attr = FS_ATTR_DIRECTORY;
    out->cluster = 0;
    out->size = 0;
    out->is_dir = true;
    out->is_root = true;
    out->ops = &fat_ops;
    return true;
}

bool fs_name_eq(const char *a, const char *b);

static bool name_eq(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

static bool fat_lookup(const char *path, fs_node_t *out);
static int fs_node_write_inner(fs_node_t *node, uint32_t offset, const void *buf,
                               uint32_t len);

static bool dir_find(const fs_node_t *dir, const char *name, uint32_t *raw_index,
                     fs_node_t *out) {
    struct lfn_state lfn;
    lfn_reset(&lfn);
    for (uint32_t i = 0; i < FS_SCAN_MAX; i++) {
        uint8_t e[FS_DIRENT_SIZE];
        if (!dirent_raw(dir, i, e)) break;
        if (e[11] == FS_ATTR_LFN) {
            lfn_parse_entry(e, &lfn);
            continue;
        }
        fs_node_t tmp;
        if (!parse_dirent(e, &tmp)) { lfn_reset(&lfn); continue; }
        lfn_apply(&lfn, &tmp, e);
        if (name_eq(tmp.name, name)) {
            if (raw_index) *raw_index = i;
            if (out) *out = tmp;
            return true;
        }
        lfn_reset(&lfn);
    }
    return false;
}

static void dir_node(fs_node_t *out, uint32_t cluster) {
    out->name[0] = 0;
    out->name[1] = 0;
    out->attr = FS_ATTR_DIRECTORY;
    out->cluster = cluster;
    out->size = 0;
    out->is_dir = true;
    out->is_root = false;
    out->dir_cluster = 0;
    out->dir_index = 0;
    out->ops = &fat_ops;
    out->dev_id = 0;
}

static int fs_resolve_parent(const char *path, fs_node_t *parent, char *name,
                              uint32_t name_max) {
    if (!path || !parent || !name) return ABI_EINVAL;

    uint32_t n = (uint32_t)strnlen(path, FS_PATH_MAX);
    if (n > FS_PATH_MAX - 1) return ABI_ENAMETOOLONG;
    if (n == 0) return ABI_EINVAL;

    char buf[FS_PATH_MAX];
    memcpy(buf, path, n);
    buf[n] = 0;

    uint32_t cut = n;
    while (cut > 0 && buf[cut - 1] != '/') cut--;

    uint32_t name_len = n - cut;
    if (name_len == 0 || name_len >= name_max) return ABI_ENAMETOOLONG;

    memcpy(name, buf + cut, name_len);
    name[name_len] = 0;

    if (cut == 0) {
        if (!root_node(parent)) return ABI_ENOENT;
    } else {
        buf[cut - 1] = 0;
        if (!fat_lookup(buf, parent)) return ABI_ENOENT;
    }
    if (!parent->is_dir) return ABI_ENOTDIR;
    return 0;
}

static bool lfn_name_valid(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n >= FS_MAX_NAME) return false;
    if (name[n - 1] == ' ' || name[n - 1] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
        if (c >= 0x80) return false;
    }
    return true;
}

static char sfn_char(char c) {
    c = (char)toupper((unsigned char)c);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    if (c == '$' || c == '%' || c == '-' || c == '_' || c == '@' ||
        c == '~' || c == '`' || c == '!' || c == '(' || c == ')' ||
        c == '{' || c == '}' || c == '^' || c == '#' || c == '&')
        return c;
    return '_';
}

static bool sfn_exists(const fs_node_t *dir, const char n83[11]) {
    for (uint32_t i = 0; i < FS_SCAN_MAX; i++) {
        uint8_t e[FS_DIRENT_SIZE];
        if (!dirent_raw(dir, i, e)) break;
        if (e[0] == 0x00) break;
        if (e[11] == FS_ATTR_LFN) continue;
        if (memcmp(e, n83, 11) == 0) return true;
    }
    return false;
}

static int make_short_name(const fs_node_t *dir, const char *name, char out[11]) {
    char base[FS_MAX_NAME];
    char ext[4];
    uint32_t bn = 0, en = 0;
    const char *dot = strrchr(name, '.');
    const char *bend = dot ? dot : name + strlen(name);

    for (const char *p = name; p < bend; p++) {
        if (*p == ' ') continue;
        if (*p == '.') return ABI_EINVAL;
        if (bn >= FS_MAX_NAME - 1) break;
        base[bn++] = sfn_char(*p);
    }
    if (dot) {
        for (const char *p = dot + 1; *p && en < 3; p++)
            ext[en++] = sfn_char(*p);
    }
    if (bn == 0) return ABI_EINVAL;

    memset(out, ' ', 11);
    memcpy(out, base, bn < 8 ? bn : 8);
    memcpy(out + 8, ext, en);
    if (bn <= 8 && !sfn_exists(dir, out)) return 0;
    memset(out, ' ', 11);

    for (uint32_t n = 1; n < 1000000; n++) {
        char suffix[8];
        int sl = snprintf(suffix, sizeof suffix, "~%u", n);
        int keep = 8 - sl;
        if (keep < 1) keep = 1;
        if ((uint32_t)keep > bn) keep = (int)bn;
        memset(out, ' ', 11);
        memcpy(out, base, (size_t)keep);
        memcpy(out + keep, suffix, (size_t)sl);
        memcpy(out + 8, ext, en);
        if (!sfn_exists(dir, out)) return 0;
    }
    return ABI_ENOSPC;
}

static uint32_t lfn_entry_count(const char *name) {
    return (uint32_t)((strlen(name) + 12) / 13);
}

static void lfn_build_entry(uint8_t e[FS_DIRENT_SIZE], const char *name,
                            uint32_t len, uint32_t ord, uint32_t total,
                            uint8_t checksum) {
    memset(e, 0xFF, FS_DIRENT_SIZE);
    e[0] = (uint8_t)(ord | (ord == total ? 0x40 : 0));
    e[11] = FS_ATTR_LFN;
    e[12] = 0;
    e[13] = checksum;
    e[26] = 0;
    e[27] = 0;

    uint32_t base = (ord - 1) * 13;
    for (int k = 0; k < 13; k++) {
        int off = (k < 5) ? (1 + k * 2) : (k < 11 ? (14 + (k - 5) * 2) : (28 + (k - 11) * 2));
        uint32_t idx = base + (uint32_t)k;
        uint16_t u = (idx < len) ? (uint8_t)name[idx] : (idx == len ? 0x0000 : 0xFFFF);
        e[off] = (uint8_t)u;
        e[off + 1] = (uint8_t)(u >> 8);
    }
}

static int dirent_store_at(const fs_node_t *dir, uint32_t index, const uint8_t *e) {
    uint32_t lba, off;
    if (!dirent_locate(dir, index, &lba, &off)) return ABI_EIO;
    return dirent_store(lba, off, e) ? 0 : ABI_EIO;
}

static int write_named_entry(const fs_node_t *dir, uint32_t start, const char *name,
                             const uint8_t short_entry[FS_DIRENT_SIZE]) {
    uint32_t len = (uint32_t)strlen(name);
    uint32_t count = lfn_entry_count(name);
    uint8_t checksum = sfn_checksum(short_entry);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t ord = count - i;
        uint8_t e[FS_DIRENT_SIZE];
        lfn_build_entry(e, name, len, ord, count, checksum);
        int r = dirent_store_at(dir, start + i, e);
        if (r) return r;
    }
    return dirent_store_at(dir, start + count, short_entry);
}

static bool fat_extend(uint32_t cluster, uint32_t *out) {
    uint16_t next;
    if (!fat_entry(cluster, &next)) return false;
    if (next < FS_EOC_MIN) { *out = next; return true; }

    uint32_t nc;
    if (!fat_alloc(&nc)) return false;
    if (!fat_set(nc, FS_EOC_MIN)) return false;
    if (!fat_set(cluster, (uint16_t)nc)) return false;
    *out = nc;
    return true;
}

static bool dirent_find_free_run(const fs_node_t *dir, uint32_t count,
                                  uint32_t *lba_out, uint32_t *off_out,
                                  uint32_t *index_out) {
    if (!dir || !count) return false;
    uint32_t per_sector = fs.bytes_per_sector / FS_DIRENT_SIZE;

    if (dir->is_root) {
        uint32_t run = 0, start_index = 0;
        for (uint32_t i = 0; i < fs.root_entry_count; i++) {
            uint8_t e[FS_DIRENT_SIZE];
            if (!dirent_raw(dir, i, e)) return false;
            if (e[0] == 0x00 || e[0] == 0xE5) {
                if (!run) start_index = i;
                run++;
                if (run == count) {
                    if (!dirent_locate(dir, start_index, lba_out, off_out)) return false;
                    *index_out = start_index;
                    return true;
                }
            } else {
                run = 0;
            }
        }
        return false;
    }

    uint8_t zero[FS_SECTOR_SIZE];
    memset(zero, 0, sizeof zero);

    uint32_t cluster = dir->cluster;
    uint32_t index = 0;
    uint32_t run = 0, start_index = 0;

    for (;;) {
        uint32_t base = cluster_lba(cluster);
        for (uint32_t sector = 0; sector < fs.sectors_per_cluster; sector++) {
            uint32_t lba = base + sector;
            uint8_t buf[FS_SECTOR_SIZE];
            if (!block_read(fs.dev, lba, 1, buf)) return false;
            for (uint32_t i = 0; i < per_sector; i++) {
                uint32_t off = i * FS_DIRENT_SIZE;
                if (buf[off] == 0x00 || buf[off] == 0xE5) {
                    if (!run) start_index = index;
                    run++;
                    if (run == count) {
                        if (!dirent_locate(dir, start_index, lba_out, off_out)) return false;
                        *index_out = start_index;
                        return true;
                    }
                } else {
                    run = 0;
                }
                index++;
            }
        }

        uint32_t nc;
        if (!fat_extend(cluster, &nc)) return false;
        for (uint32_t s = 0; s < fs.sectors_per_cluster; s++) {
            if (!sector_write(cluster_lba(nc) + s, zero)) return false;
        }
        cluster = nc;
    }
}

static int fs_node_sync_entry(fs_node_t *node) {
    if (!node || node->is_root) return ABI_EIO;

    fs_node_t parent;
    if (node->dir_cluster == 0) {
        if (!root_node(&parent)) return ABI_EIO;
    } else {
        dir_node(&parent, node->dir_cluster);
    }

    uint32_t lba, off;
    if (!dirent_locate(&parent, node->dir_index, &lba, &off)) return ABI_EIO;

    uint8_t buf[FS_SECTOR_SIZE];
    if (!block_read(fs.dev, lba, 1, buf)) return ABI_EIO;

    buf[off + 26] = (uint8_t)(node->cluster & 0xFF);
    buf[off + 27] = (uint8_t)((node->cluster >> 8) & 0xFF);
    buf[off + 28] = (uint8_t)(node->size & 0xFF);
    buf[off + 29] = (uint8_t)((node->size >> 8) & 0xFF);
    buf[off + 30] = (uint8_t)((node->size >> 16) & 0xFF);
    buf[off + 31] = (uint8_t)((node->size >> 24) & 0xFF);

    return sector_write(lba, buf) ? 0 : ABI_EIO;
}

int fs_node_chattr(fs_node_t *node, uint8_t attr) {
    if (!node || node->is_root) return ABI_EIO;

    node->attr = attr;

    fs_node_t parent;
    if (node->dir_cluster == 0) {
        if (!root_node(&parent)) return ABI_EIO;
    } else {
        dir_node(&parent, node->dir_cluster);
    }

    uint32_t lba, off;
    if (!dirent_locate(&parent, node->dir_index, &lba, &off)) return ABI_EIO;

    uint8_t buf[FS_SECTOR_SIZE];
    if (!block_read(fs.dev, lba, 1, buf)) return ABI_EIO;

    buf[off + 11] = attr;
    return sector_write(lba, buf) ? 0 : ABI_EIO;
}

static void fat_stamp(int64_t unix_sec, uint16_t *out_time, uint16_t *out_date) {
    int64_t days = unix_sec / 86400;
    int64_t rem = unix_sec % 86400;
    if (rem < 0) { rem += 86400; days--; }

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t year = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    uint64_t day = doy - (153 * mp + 2) / 5 + 1;
    uint64_t month = mp + (mp < 10 ? 3 : -9);
    year += (month <= 2);

    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;

    uint32_t hh = (uint32_t)(rem / 3600);
    uint32_t mm = (uint32_t)((rem % 3600) / 60);
    uint32_t ss = (uint32_t)(rem % 60);

    *out_time = (uint16_t)((hh << 11) | (mm << 5) | (ss / 2));
    *out_date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
}

static int fs_node_utimens_inner(fs_node_t *node, int64_t mtime) {
    if (!node || node->is_root) return 0;

    fs_node_t parent;
    if (node->dir_cluster == 0) {
        if (!root_node(&parent)) return ABI_EIO;
    } else {
        dir_node(&parent, node->dir_cluster);
    }

    uint32_t lba, off;
    if (!dirent_locate(&parent, node->dir_index, &lba, &off)) return ABI_EIO;

    uint8_t buf[512];
    if (!block_read(fs.dev, lba, 1, buf)) return ABI_EIO;
    uint16_t ft, fd2;
    fat_stamp(mtime, &ft, &fd2);
    buf[off + 18] = fd2 & 0xFF; buf[off + 19] = fd2 >> 8;
    buf[off + 22] = ft & 0xFF;  buf[off + 23] = ft >> 8;
    buf[off + 24] = fd2 & 0xFF; buf[off + 25] = fd2 >> 8;
    return sector_write(lba, buf) ? 0 : ABI_EIO;
}

static int fat_node_utimens(fs_node_t *node, int64_t mtime) {
    lock_acquire(LOCK_FS, &fs_lock);
    int r = fs_node_utimens_inner(node, mtime);
    lock_release(LOCK_FS, &fs_lock);
    return r;
}

static int fat_utimens(const char *path, int64_t mtime) {
    fs_node_t node;
    if (!fs.mounted) return ABI_EIO;
    bool ok = fat_lookup(path, &node);
    if (!ok) return ABI_ENOENT;
    return fat_node_utimens(&node, mtime);
}

static int fs_node_resize_inner(fs_node_t *node, uint32_t size) {
    if (!fs.mounted || !node) return ABI_EINVAL;
    if (node->is_dir) return ABI_EISDIR;
    if (size == node->size) return 0;

    if (size < node->size) {
        uint32_t cluster_size = (uint32_t)fs.sectors_per_cluster * fs.bytes_per_sector;
        uint32_t keep = (size + cluster_size - 1) / cluster_size;
        uint32_t cluster = node->cluster;
        uint32_t prev = 0;

        for (uint32_t i = 0; i < keep && cluster; i++) {
            uint16_t next;
            if (!fat_entry(cluster, &next)) return ABI_EIO;
            prev = cluster;
            if (next >= FS_EOC_MIN) { cluster = 0; break; }
            cluster = next;
        }

        if (cluster) {
            if (!fat_free_chain(cluster)) return ABI_EIO;
            if (prev) {
                if (!fat_set(prev, FS_EOC_MIN)) return ABI_EIO;
            } else {
                node->cluster = 0;
            }
        }

        node->size = size;
        return fs_node_sync_entry(node);
    }

    uint8_t zeros[512];
    memset(zeros, 0, sizeof zeros);
    uint32_t pos = node->size;
    while (pos < size) {
        uint32_t chunk = size - pos;
        if (chunk > sizeof zeros) chunk = (uint32_t)sizeof zeros;
        int r = fs_node_write_inner(node, pos, zeros, chunk);
        if (r < 0) return r;
        pos += chunk;
    }
    return 0;
}

static int fs_node_write_inner(fs_node_t *node, uint32_t offset, const void *buf,
                               uint32_t len) {
    if (!fs.mounted || !node || !buf) return ABI_EINVAL;
    if (node->is_dir) return ABI_EISDIR;
    if (len == 0) return 0;

    if ((uint32_t)offset > node->size) {
        uint32_t gap = (uint32_t)offset - node->size;
        uint8_t zeros[512];
        memset(zeros, 0, sizeof zeros);
        uint32_t pos = node->size;
        while (gap) {
            uint32_t chunk = gap > (uint32_t)sizeof zeros ? (uint32_t)sizeof zeros : gap;
            int r = fs_node_write_inner(node, pos, zeros, chunk);
            if (r < 0) return r;
            pos += chunk;
            gap -= chunk;
        }
    }

    uint32_t cluster_size = (uint32_t)fs.sectors_per_cluster * fs.bytes_per_sector;

    if (node->cluster == 0) {
        uint32_t c;
        if (!fat_alloc(&c)) return ABI_ENOSPC;
        if (!fat_set(c, FS_EOC_MIN)) return ABI_EIO;
        node->cluster = c;
    }

    uint32_t cl_index = offset / cluster_size;
    uint32_t within = offset % cluster_size;
    uint32_t cluster = node->cluster;

    for (uint32_t i = 0; i < cl_index; i++) {
        uint32_t next;
        if (!fat_extend(cluster, &next)) return ABI_ENOSPC;
        cluster = next;
    }

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0;

    while (len) {
        uint32_t lba = cluster_lba(cluster) + within / fs.bytes_per_sector;
        uint32_t off = within % fs.bytes_per_sector;
        uint32_t chunk = fs.bytes_per_sector - off;
        if (chunk > len) chunk = len;

        uint8_t sbuf[FS_SECTOR_SIZE];
        if (off == 0 && chunk == fs.bytes_per_sector) {
            memcpy(sbuf, src, chunk);
        } else {
            if (!block_read(fs.dev, lba, 1, sbuf)) return ABI_EIO;
            memcpy(sbuf + off, src, chunk);
        }
        if (!sector_write(lba, sbuf)) return ABI_EIO;

        src += chunk;
        done += chunk;
        len -= chunk;
        within += chunk;

        if (len && within == cluster_size) {
            within = 0;
            uint32_t next;
            if (!fat_extend(cluster, &next)) return ABI_ENOSPC;
            cluster = next;
        }
    }

    uint32_t end = offset + done;
    if (end > node->size) {
        node->size = end;
        int r = fs_node_sync_entry(node);
        if (r) return r;
    }
    return (int)done;
}

static int fs_node_truncate_inner(fs_node_t *node) {
    if (!fs.mounted || !node) return ABI_EINVAL;
    if (node->is_dir) return ABI_EISDIR;

    if (node->cluster) {
        if (!fat_free_chain(node->cluster)) return ABI_EIO;
    }
    node->cluster = 0;
    node->size = 0;
    return fs_node_sync_entry(node);
}

static int fs_create_inner(const char *path, fs_node_t *out) {
    fs_node_t parent;
    char name[FS_MAX_NAME + 2];
    int r = fs_resolve_parent(path, &parent, name, sizeof name);
    if (r) return r;

    if (!lfn_name_valid(name)) return ABI_EINVAL;

    char n83[11];
    r = make_short_name(&parent, name, n83);
    if (r) return r;

    uint32_t index;
    fs_node_t found;
    if (dir_find(&parent, name, &index, &found)) return ABI_EEXIST;

    uint32_t lba, off;
    uint32_t lfn_count = lfn_entry_count(name);
    if (!dirent_find_free_run(&parent, lfn_count + 1, &lba, &off, &index)) return ABI_ENOSPC;
    (void)lba;
    (void)off;

    uint8_t e[FS_DIRENT_SIZE];
    dirent_build(e, n83, FS_ATTR_ARCHIVE, 0, 0);
    r = write_named_entry(&parent, index, name, e);
    if (r) return r;

    if (out) {
        memset(out->name, 0, FS_MAX_NAME);
        memcpy(out->name, name, strlen(name));
        out->attr = FS_ATTR_ARCHIVE;
        out->cluster = 0;
        out->size = 0;
        out->is_dir = false;
        out->is_root = false;
        out->dir_cluster = parent.cluster;
        out->dir_index = index + lfn_count;
        out->ops = &fat_ops;
        out->dev_id = 0;
    }
    return 0;
}

static int fs_mkdir_inner(const char *path, fs_node_t *out) {
    fs_node_t parent;
    char name[FS_MAX_NAME + 2];
    int r = fs_resolve_parent(path, &parent, name, sizeof name);
    if (r) return r;

    if (!lfn_name_valid(name)) return ABI_EINVAL;

    char n83[11];
    r = make_short_name(&parent, name, n83);
    if (r) return r;

    uint32_t index;
    fs_node_t found;
    if (dir_find(&parent, name, &index, &found)) return ABI_EEXIST;

    uint32_t cluster;
    if (!fat_alloc(&cluster)) return ABI_ENOSPC;
    if (!fat_set(cluster, FS_EOC_MIN)) return ABI_EIO;

    uint8_t zero[FS_SECTOR_SIZE];
    memset(zero, 0, sizeof zero);

    uint32_t lba = cluster_lba(cluster);
    for (uint32_t s = 0; s < fs.sectors_per_cluster; s++) {
        if (!sector_write(lba + s, zero)) return ABI_EIO;
    }

    uint8_t e[FS_DIRENT_SIZE];
    char dot[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    char dotdot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    dirent_build(e, dot, FS_ATTR_DIRECTORY, cluster, 0);
    if (!dirent_store(lba, 0, e)) return ABI_EIO;
    dirent_build(e, dotdot, FS_ATTR_DIRECTORY, parent.cluster, 0);
    if (!dirent_store(lba, FS_DIRENT_SIZE, e)) return ABI_EIO;

    uint32_t elba, eoff;
    uint32_t lfn_count = lfn_entry_count(name);
    if (!dirent_find_free_run(&parent, lfn_count + 1, &elba, &eoff, &index)) return ABI_ENOSPC;
    (void)elba;
    (void)eoff;

    dirent_build(e, n83, FS_ATTR_DIRECTORY, cluster, 0);
    r = write_named_entry(&parent, index, name, e);
    if (r) return r;

    if (out) {
        memset(out->name, 0, FS_MAX_NAME);
        memcpy(out->name, name, strlen(name));
        out->attr = FS_ATTR_DIRECTORY;
        out->cluster = cluster;
        out->size = 0;
        out->is_dir = true;
        out->is_root = false;
        out->dir_cluster = parent.cluster;
        out->dir_index = index + lfn_count;
        out->ops = &fat_ops;
        out->dev_id = 0;
    }
    return 0;
}

static int delete_lfn_group(const fs_node_t *parent, uint32_t short_index) {
    uint8_t short_entry[FS_DIRENT_SIZE];
    if (!dirent_raw(parent, short_index, short_entry)) return ABI_EIO;
    uint8_t checksum = sfn_checksum(short_entry);

    uint32_t j = short_index;
    for (uint32_t depth = 0; depth < 20 && j > 0; depth++) {
        j--;
        uint8_t e[FS_DIRENT_SIZE];
        if (!dirent_raw(parent, j, e)) break;
        if (e[11] != FS_ATTR_LFN || e[13] != checksum) break;
        uint32_t lba, off;
        if (!dirent_locate(parent, j, &lba, &off)) return ABI_EIO;
        e[0] = 0xE5;
        if (!dirent_store(lba, off, e)) return ABI_EIO;
    }
    return 0;
}

static int fs_unlink_inner(const char *path, bool want_dir) {
    fs_node_t parent;
    char name[FS_MAX_NAME + 2];
    int r = fs_resolve_parent(path, &parent, name, sizeof name);
    if (r) return r;

    uint32_t index;
    fs_node_t found;
    if (!dir_find(&parent, name, &index, &found)) return ABI_ENOENT;

    if (found.is_dir != want_dir) return want_dir ? ABI_ENOTDIR : ABI_EISDIR;

    if (found.is_dir) {
        for (uint32_t i = 0; i < FS_SCAN_MAX; i++) {
            uint8_t de[FS_DIRENT_SIZE];
            fs_node_t tmp;
            if (!dirent_raw(&found, i, de)) break;
            if (parse_dirent(de, &tmp)) return ABI_ENOTEMPTY;
        }
    }

    if (found.cluster) {
        if (!fat_free_chain(found.cluster)) return ABI_EIO;
    }

    int dr = delete_lfn_group(&parent, index);
    if (dr) return dr;

    uint32_t lba, off;
    if (!dirent_locate(&parent, index, &lba, &off)) return ABI_EIO;

    uint8_t buf[FS_SECTOR_SIZE];
    if (!block_read(fs.dev, lba, 1, buf)) return ABI_EIO;
    buf[off] = 0xE5;
    if (!sector_write(lba, buf)) return ABI_EIO;
    return 0;
}

static bool fat_lookup(const char *path, fs_node_t *out) {
    if (!fs.mounted || !path || !out) return false;

    char buf[FS_PATH_MAX];
    uint32_t n = (uint32_t)strnlen(path, FS_PATH_MAX - 1);
    memcpy(buf, path, n);
    buf[n] = 0;
    if (n == 0) return root_node(out);

    fs_node_t cur;
    if (!root_node(&cur)) return false;

    uint32_t start = (buf[0] == '/') ? 1 : 0;
    while (start <= n) {
        const char *slash = strchr(buf + start, '/');
        uint32_t end = slash ? (uint32_t)(slash - buf) : n;

        uint32_t len = end - start;
        if (len == 0) {
            start = end + 1;
            continue;
        }
        if (len > FS_MAX_NAME + 1) return false;

        char comp[FS_MAX_NAME + 2];
        memcpy(comp, buf + start, len);
        comp[len] = 0;

        if (comp[0] == '.' && comp[1] == 0) {
            start = end + 1;
            continue;
        }
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) return false;

        fs_node_t found;
        uint32_t raw = 0;
        if (!dir_find(&cur, comp, &raw, &found)) return false;
        found.dir_cluster = cur.cluster;
        found.dir_index = raw;

        cur = found;
        if (end < n && !cur.is_dir) return false;
        start = end + 1;
    }

    *out = cur;
    return true;
}

static bool fat_read(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len) {
    if (!fs.mounted || !node || !buf) return false;
    if (node->is_dir) return false;
    if (len == 0) return true;
    if (offset >= node->size) return false;
    if (!cluster_ok(node->cluster)) return false;

    if (len > node->size - offset) len = node->size - offset;
    if (len == 0) return false;

    uint32_t cluster_size = (uint32_t)fs.sectors_per_cluster * fs.bytes_per_sector;
    uint32_t cl_index = offset / cluster_size;
    uint32_t within = offset % cluster_size;

    uint32_t cluster;
    if (!fat_read_cluster(node, cl_index, &cluster)) return false;

    uint8_t *out = (uint8_t *)buf;
    while (len) {
        uint32_t lba = cluster_lba(cluster) + within / fs.bytes_per_sector;
        uint32_t off = within % fs.bytes_per_sector;

        if (off == 0 && len >= fs.bytes_per_sector && ((uint64_t)out & 1) == 0) {
            uint32_t sectors = len / fs.bytes_per_sector;
            uint32_t avail = (cluster_size - within) / fs.bytes_per_sector;
            if (sectors > avail) sectors = avail;
            if (sectors > 256) sectors = 256;

            uint32_t bytes = sectors * fs.bytes_per_sector;
            if (!block_read(fs.dev, lba, sectors, out)) return false;

            out += bytes;
            len -= bytes;
            within += bytes;

            if (len && within == cluster_size) {
                within = 0;
                uint32_t nx;
                if (!fat_next(cluster, &nx)) return false;
                cluster = nx;
            }
            continue;
        }

        uint32_t chunk = fs.bytes_per_sector - off;
        if (chunk > len) chunk = len;

        if (!sector_read(lba)) return false;
        memcpy(out, sec + off, chunk);

        out += chunk;
        len -= chunk;
        within += chunk;

        if (len && within == cluster_size) {
            within = 0;
            uint32_t nx;
            if (!fat_next(cluster, &nx)) return false;
            cluster = nx;
        }
    }
    return true;
}

static int fat_unlink(const char *path) {
    return fs_unlink_inner(path, false);
}

static int fat_rmdir(const char *path) {
    return fs_unlink_inner(path, true);
}

static bool fat_sync(void) {
    if (!fs.mounted) return false;
    return block_flush(fs.dev);
}

bool fs_name_eq(const char *a, const char *b) {
    return name_eq(a, b);
}

const struct fs_ops fat_ops = {
    .iterate = fat_iterate,
    .lookup = fat_lookup,
    .read = fat_read,
    .write = fs_node_write_inner,
    .truncate = fs_node_truncate_inner,
    .resize = fs_node_resize_inner,
    .create = fs_create_inner,
    .mkdir = fs_mkdir_inner,
    .unlink = fat_unlink,
    .rmdir = fat_rmdir,
    .utimens = fat_utimens,
    .node_utimens = fat_node_utimens,
    .sync = fat_sync,
};
