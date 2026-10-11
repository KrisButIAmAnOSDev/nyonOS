#include "fs.h"
#include "kernel/block/block.h"
#include "kernel/sync/preempt.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/cpu/cpu.h"

#define FS_PATH_MAX 128
#define FS_SCAN_MAX 4096

static fs_volume_t fs;

static uint8_t fs_sectors[MAX_CPUS][FS_SECTOR_SIZE] __attribute__((aligned(4)));

#define sec (fs_sectors[cpu_id()])

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
    preempt_disable();
    bool r = fs_mount_inner(dev);
    preempt_enable();
    return r;
}

static uint32_t cluster_lba(uint32_t cluster) {
    return fs.data_start + (cluster - FS_FIRST_CLUSTER) * fs.sectors_per_cluster;
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
    if (!sector_read(lba)) return false;
    *out = rd16(sec + off);
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

static bool dirent_raw(const fs_node_t *dir, uint32_t index, uint8_t *out) {
    if (!dir || !dir->is_dir) return false;
    if (index >= FS_SCAN_MAX) return false;

    uint32_t per_sector = fs.bytes_per_sector / FS_DIRENT_SIZE;
    uint32_t lba;

    if (dir->cluster == 0) {
        if (index >= fs.root_entry_count) return false;
        lba = fs.root_start + index / per_sector;
        uint32_t off = (index % per_sector) * FS_DIRENT_SIZE;
        if (!sector_read(lba)) return false;
        for (int i = 0; i < FS_DIRENT_SIZE; i++) out[i] = sec[off + i];
        return true;
    }

    uint32_t per_cluster = per_sector * fs.sectors_per_cluster;
    uint32_t cl_index = index / per_cluster;
    uint32_t within = index % per_cluster;

    uint32_t cluster;
    if (!cluster_nth(dir->cluster, cl_index, &cluster)) return false;

    lba = cluster_lba(cluster) + within / per_sector;
    uint32_t off = (within % per_sector) * FS_DIRENT_SIZE;
    if (!sector_read(lba)) return false;
    for (int i = 0; i < FS_DIRENT_SIZE; i++) out[i] = sec[off + i];
    return true;
}

static void trim83(const uint8_t *raw, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) out[n++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) out[n++] = (char)raw[i];
    }
    out[n] = 0;
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
    out->cluster = (uint32_t)rd16(e + 26) | ((uint32_t)rd16(e + 20) << 16);
    out->size = out->is_dir ? 0 : rd32(e + 28);

    if (!out->is_dir && !cluster_ok(out->cluster)) out->cluster = 0;
    return true;
}

bool fs_iterate_inner(const fs_node_t *dir, uint32_t index, fs_node_t *out) {
    if (!fs.mounted || !dir || !out) return false;
    if (!dir->is_dir) return false;

    uint32_t live = 0;
    for (uint32_t i = 0; i < FS_SCAN_MAX; i++) {
        uint8_t e[FS_DIRENT_SIZE];
        fs_node_t tmp;
        if (!dirent_raw(dir, i, e)) return false;
        if (!parse_dirent(e, &tmp)) continue;
        if (live == index) {
            *out = tmp;
            return true;
        }
        live++;
    }
    return false;
}

bool fs_iterate(const fs_node_t *dir, uint32_t index, fs_node_t *out) {
    preempt_disable();
    bool r = fs_iterate_inner(dir, index, out);
    preempt_enable();
    return r;
}

static bool root_node(fs_node_t *out) {
    if (!fs.mounted) return false;
    out->name[0] = '/';
    out->name[1] = 0;
    out->attr = FS_ATTR_DIRECTORY;
    out->cluster = 0;
    out->size = 0;
    out->is_dir = true;
    return true;
}

static bool name_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

bool fs_lookup_inner(const char *path, fs_node_t *out) {
    if (!fs.mounted || !path || !out) return false;

    char buf[FS_PATH_MAX];
    uint32_t n = 0;
    for (uint32_t i = 0; path[i] && n < FS_PATH_MAX - 1; i++) buf[n++] = path[i];
    buf[n] = 0;
    if (n == 0) return root_node(out);

    fs_node_t cur;
    if (!root_node(&cur)) return false;

    uint32_t start = (buf[0] == '/') ? 1 : 0;
    while (start <= n) {
        uint32_t end = start;
        while (end < n && buf[end] != '/') end++;

        uint32_t len = end - start;
        if (len == 0) {
            start = end + 1;
            continue;
        }
        if (len > FS_MAX_NAME + 1) return false;

        char comp[FS_MAX_NAME + 2];
        for (uint32_t i = 0; i < len; i++) comp[i] = buf[start + i];
        comp[len] = 0;

        if (comp[0] == '.' && comp[1] == 0) {
            start = end + 1;
            continue;
        }
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) return false;

        fs_node_t found;
        bool ok = false;
        for (uint32_t i = 0; i < FS_SCAN_MAX; i++) {
            if (!fs_iterate(&cur, i, &found)) break;
            if (name_eq(found.name, comp)) {
                ok = true;
                break;
            }
        }
        if (!ok) return false;

        cur = found;
        if (end < n && !cur.is_dir) return false;
        start = end + 1;
    }

    *out = cur;
    return true;
}

bool fs_lookup(const char *path, fs_node_t *out) {
    preempt_disable();
    bool r = fs_lookup_inner(path, out);
    preempt_enable();
    return r;
}

bool fs_node_read_inner(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len) {
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
    if (!cluster_nth(node->cluster, cl_index, &cluster)) return false;

    uint8_t *out = (uint8_t *)buf;
    while (len) {
        uint32_t lba = cluster_lba(cluster) + within / fs.bytes_per_sector;
        uint32_t off = within % fs.bytes_per_sector;
        uint32_t chunk = fs.bytes_per_sector - off;
        if (chunk > len) chunk = len;

        if (!sector_read(lba)) return false;
        for (uint32_t i = 0; i < chunk; i++) out[i] = sec[off + i];

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

bool fs_node_read(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len) {
    preempt_disable();
    bool r = fs_node_read_inner(node, offset, buf, len);
    preempt_enable();
    return r;
}

uint32_t fs_node_read_all_inner(const fs_node_t *node, void *buf, uint32_t max) {
    if (!node) return 0;
    uint32_t want = node->size;
    if (want > max) want = max;
    if (want == 0) return 0;
    if (!fs_node_read(node, 0, buf, want)) return 0;
    return want;
}

uint32_t fs_node_read_all(const fs_node_t *node, void *buf, uint32_t max) {
    preempt_disable();
    uint32_t r = fs_node_read_all_inner(node, buf, max);
    preempt_enable();
    return r;
}
