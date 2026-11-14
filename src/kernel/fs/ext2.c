#include "fs.h"
#include "ext2.h"
#include "kernel/block/block.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/sync/preempt.h"
#include "kernel/sync/sync.h"
#include "kernel/vfs/vfs.h"
#include "arch/x86_64/cpu/cpu.h"
#include "arch/x86_64/rtc/rtc.h"
#include "abi/abi.h"

#define EXT2_SUPER_OFFSET 1024
#define EXT2_MAGIC        0xEF53
#define EXT2_ROOT_INODE   2
#define EXT2_BLOCK_MAX    4096

#define EXT2_COMPAT_FILETYPE  0x0002
#define EXT2_INCOMPAT_FILETYPE 0x0002
#define EXT2_INCOMPAT_EXTENTS  0x0040
#define EXT2_INCOMPAT_64BIT    0x0080

#define EXT2_RO_SPARSE_SUPER   0x0001
#define EXT2_RO_LARGE_FILE     0x0002
#define EXT2_RO_DIR_INDEX      0x0004

#define EXT2_S_IFMT  0xF000
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFREG 0x8000
#define EXT2_S_IFLNK 0xA000

#define EXT2_FT_REG  1
#define EXT2_FT_DIR  2
#define EXT2_FT_LNK  7

typedef struct {
    block_device_t *dev;
    bool mounted;
    uint32_t block_size;
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t first_data_block;
    uint32_t group_count;
    uint32_t inode_size;
    uint32_t desc_size;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    char label[17];
} ext2_fs_t;

static ext2_fs_t e2;
static spinlock_t e2_lock = SPINLOCK_INIT;

static bool e2_readlink_locked(const fs_node_t *node, char *buf, uint32_t size);
static uint8_t e2_bounce[MAX_CPUS][EXT2_BLOCK_MAX];
static uint8_t e2_bounce2[MAX_CPUS][EXT2_BLOCK_MAX];

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint8_t *bounce(void) {
    uint32_t id = cpu_id();
    if (id >= MAX_CPUS) id = 0;
    return e2_bounce[id];
}

static uint8_t *bounce2(void) {
    uint32_t id = cpu_id();
    if (id >= MAX_CPUS) id = 0;
    return e2_bounce2[id];
}

static bool e2_read_block(uint32_t block, void *buf) {
    if (!e2.mounted || !buf) return false;
    if (block >= e2.blocks_count) return false;
    uint32_t sectors = e2.block_size / 512;
    uint64_t lba = (uint64_t)block * sectors;
    return block_read(e2.dev, lba, sectors, buf);
}

static uint32_t e2_icache_blk[MAX_CPUS];
static uint8_t e2_icache_data[MAX_CPUS][EXT2_BLOCK_MAX];

static bool e2_indirect_block(uint32_t block, uint8_t *dst) {
    uint32_t id = cpu_id();
    if (id >= MAX_CPUS) id = 0;
    if (block && e2_icache_blk[id] == block) {
        memcpy(dst, e2_icache_data[id], e2.block_size);
        return true;
    }
    if (!e2_read_block(block, dst)) return false;
    memcpy(e2_icache_data[id], dst, e2.block_size);
    e2_icache_blk[id] = block;
    return true;
}

static void e2_indirect_invalidate(uint32_t block) {
    for (uint32_t id = 0; id < MAX_CPUS; id++)
        if (e2_icache_blk[id] == block) e2_icache_blk[id] = 0;
}

static bool e2_read_at(uint64_t off, void *buf, uint32_t len) {
    uint8_t *out = buf;
    bool ok = true;

    preempt_disable();
    while (len) {
        uint32_t blk = (uint32_t)(off / e2.block_size);
        uint32_t boff = (uint32_t)(off % e2.block_size);
        uint32_t chunk = e2.block_size - boff;
        if (chunk > len) chunk = len;
        uint8_t *b = bounce2();
        if (!e2_read_block(blk, b)) { ok = false; break; }
        memcpy(out, b + boff, chunk);
        out += chunk;
        len -= chunk;
        off += chunk;
    }
    preempt_enable();
    return ok;
}

static bool e2_write_block(uint32_t block, const void *buf) {
    if (!e2.mounted || !buf) return false;
    if (block >= e2.blocks_count) return false;
    e2_indirect_invalidate(block);
    uint32_t sectors = e2.block_size / 512;
    uint64_t lba = (uint64_t)block * sectors;
    return block_write(e2.dev, lba, sectors, buf);
}

static bool e2_write_at(uint64_t off, const void *buf, uint32_t len) {
    const uint8_t *in = buf;
    bool ok = true;

    preempt_disable();
    while (len) {
        uint32_t blk = (uint32_t)(off / e2.block_size);
        uint32_t boff = (uint32_t)(off % e2.block_size);
        uint32_t chunk = e2.block_size - boff;
        if (chunk > len) chunk = len;
        uint8_t *b = bounce2();
        if (!e2_read_block(blk, b)) { ok = false; break; }
        memcpy(b + boff, in, chunk);
        if (!e2_write_block(blk, b)) { ok = false; break; }
        in += chunk;
        len -= chunk;
        off += chunk;
    }
    preempt_enable();
    return ok;
}

static bool e2_group_desc(uint32_t g, uint8_t *out) {
    uint32_t gdt_block = (e2.block_size == 1024) ? 2 : 1;
    uint64_t off = (uint64_t)gdt_block * e2.block_size + (uint64_t)g * e2.desc_size;
    return e2_read_at(off, out, 32);
}

static bool e2_read_inode(uint32_t ino, uint8_t *buf) {
    if (ino < 1 || ino > e2.inodes_count) return false;
    uint32_t g = (ino - 1) / e2.inodes_per_group;
    uint32_t idx = (ino - 1) % e2.inodes_per_group;

    uint8_t gd[32];
    if (!e2_group_desc(g, gd)) return false;
    uint32_t itable = rd32(gd + 8);

    uint64_t off = (uint64_t)itable * e2.block_size + (uint64_t)idx * e2.inode_size;
    return e2_read_at(off, buf, e2.inode_size);
}

static bool e2_inode_slot(uint32_t ino, uint32_t *g_out, uint32_t *idx_out) {
    if (ino < 1 || ino > e2.inodes_count) return false;
    *g_out = (ino - 1) / e2.inodes_per_group;
    *idx_out = (ino - 1) % e2.inodes_per_group;
    return true;
}

static bool e2_write_inode(uint32_t ino, const uint8_t *buf) {
    uint32_t g = 0, idx = 0;
    if (!e2_inode_slot(ino, &g, &idx)) return false;

    uint8_t gd[32];
    if (!e2_group_desc(g, gd)) return false;
    uint32_t itable = rd32(gd + 8);

    uint64_t off = (uint64_t)itable * e2.block_size + (uint64_t)idx * e2.inode_size;
    return e2_write_at(off, buf, e2.inode_size);
}

static bool e2_filetype(void) {
    return (e2.feature_compat & EXT2_COMPAT_FILETYPE) != 0;
}

static uint32_t e2_group_first_block(uint32_t g) {
    return e2.first_data_block + g * e2.blocks_per_group;
}

static uint8_t e2_bm_buf[8192];

static bool e2_bitmap_load(uint32_t bitmap_block, uint32_t bits) {
    uint32_t bytes = (bits + 7) / 8;
    uint32_t nblocks = (bytes + e2.block_size - 1) / e2.block_size;

    memset(e2_bm_buf, 0, bytes);
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t chunk = bytes - i * e2.block_size;
        if (chunk > e2.block_size) chunk = e2.block_size;
        uint8_t *dst = e2_bm_buf + i * e2.block_size;
        if (!e2_read_block(bitmap_block + i, dst)) return false;
    }
    return true;
}

static bool e2_bitmap_store(uint32_t bitmap_block, uint32_t bits) {
    uint32_t bytes = (bits + 7) / 8;
    uint32_t nblocks = (bytes + e2.block_size - 1) / e2.block_size;

    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t chunk = bytes - i * e2.block_size;
        if (chunk > e2.block_size) chunk = e2.block_size;
        if (!e2_write_block(bitmap_block + i, e2_bm_buf + i * e2.block_size)) return false;
    }
    return true;
}

static bool e2_bitmap_find_free(uint32_t bitmap_block, uint32_t bits, uint32_t *out) {
    if (!e2_bitmap_load(bitmap_block, bits)) return false;

    uint32_t bytes = (bits + 7) / 8;
    for (uint32_t i = 0; i < bytes; i++) {
        if (e2_bm_buf[i] == 0xff) continue;
        for (uint32_t bit = 0; bit < 8; bit++) {
            if (e2_bm_buf[i] & (1u << bit)) continue;
            uint32_t index = i * 8 + bit;
            if (index >= bits) return false;
            e2_bm_buf[i] |= (uint8_t)(1u << bit);
            if (!e2_bitmap_store(bitmap_block, bits)) return false;
            *out = index;
            return true;
        }
    }
    return false;
}

static bool e2_bitmap_clear(uint32_t bitmap_block, uint32_t bits, uint32_t index) {
    if (!e2_bitmap_load(bitmap_block, bits)) return false;
    uint32_t byte = index / 8;
    uint32_t bit = index % 8;
    if (byte >= e2.block_size) return false;
    e2_bm_buf[byte] &= (uint8_t)~(1u << bit);
    return e2_bitmap_store(bitmap_block, bits);
}

static void e2_super_counts(int32_t dblocks, int32_t dinodes) {
    uint8_t sb[1024];
    if (!e2_read_at(EXT2_SUPER_OFFSET, sb, sizeof sb)) return;
    uint32_t fb = rd32(sb + 12);
    uint32_t fi = rd32(sb + 16);
    if (dblocks > 0 && fb < (uint32_t)dblocks) fb = 0;
    else fb = (uint32_t)((int32_t)fb + dblocks);
    if (dinodes > 0 && fi < (uint32_t)dinodes) fi = 0;
    else fi = (uint32_t)((int32_t)fi + dinodes);
    uint8_t *p = sb + 12;
    p[0] = (uint8_t)fb; p[1] = (uint8_t)(fb >> 8); p[2] = (uint8_t)(fb >> 16); p[3] = (uint8_t)(fb >> 24);
    p = sb + 16;
    p[0] = (uint8_t)fi; p[1] = (uint8_t)(fi >> 8); p[2] = (uint8_t)(fi >> 16); p[3] = (uint8_t)(fi >> 24);
    e2_write_at(EXT2_SUPER_OFFSET, sb, sizeof sb);
}

static void e2_group_counts(uint32_t g, int32_t dblocks, int32_t dinodes) {
    uint8_t gd[32];
    if (!e2_group_desc(g, gd)) return;
    uint16_t fb = rd16(gd + 12);
    uint16_t fi = rd16(gd + 14);
    fb = (uint16_t)((int32_t)fb + dblocks);
    fi = (uint16_t)((int32_t)fi + dinodes);
    gd[12] = (uint8_t)fb; gd[13] = (uint8_t)(fb >> 8);
    gd[14] = (uint8_t)fi; gd[15] = (uint8_t)(fi >> 8);
    uint32_t gdt_block = (e2.block_size == 1024) ? 2 : 1;
    uint64_t off = (uint64_t)gdt_block * e2.block_size + (uint64_t)g * e2.desc_size;
    e2_write_at(off, gd, 32);
}

static bool e2_alloc_block(uint32_t *out) {
    for (uint32_t g = 0; g < e2.group_count; g++) {
        uint8_t gd[32];
        if (!e2_group_desc(g, gd)) return false;
        uint32_t bmb = rd32(gd + 0);
        uint32_t idx = 0;
        if (!e2_bitmap_find_free(bmb, e2.blocks_per_group, &idx)) continue;
        uint32_t blk = e2_group_first_block(g) + idx;
        if (blk >= e2.blocks_count) return false;
        e2_super_counts(-1, 0);
        e2_group_counts(g, -1, 0);
        *out = blk;
        return true;
    }
    return false;
}

static bool e2_free_block(uint32_t blk) {
    if (!blk || blk >= e2.blocks_count) return false;
    e2_indirect_invalidate(blk);
    uint32_t g = (blk - e2.first_data_block) / e2.blocks_per_group;
    if (g >= e2.group_count) return false;
    uint32_t idx = blk - e2_group_first_block(g);

    uint8_t gd[32];
    if (!e2_group_desc(g, gd)) return false;
    if (!e2_bitmap_clear(rd32(gd + 0), e2.blocks_per_group, idx)) return false;
    e2_super_counts(1, 0);
    e2_group_counts(g, 1, 0);
    return true;
}

static bool e2_alloc_inode(uint32_t *out) {
    for (uint32_t g = 0; g < e2.group_count; g++) {
        uint8_t gd[32];
        if (!e2_group_desc(g, gd)) return false;
        uint32_t ibm = rd32(gd + 4);
        uint32_t idx = 0;
        if (!e2_bitmap_find_free(ibm, e2.inodes_per_group, &idx)) continue;
        uint32_t ino = g * e2.inodes_per_group + idx + 1;
        if (ino > e2.inodes_count) return false;
        e2_super_counts(0, -1);
        e2_group_counts(g, 0, -1);
        *out = ino;
        return true;
    }
    return false;
}

static bool e2_free_inode(uint32_t ino) {
    uint32_t g = 0, idx = 0;
    if (!e2_inode_slot(ino, &g, &idx)) return false;

    uint8_t gd[32];
    if (!e2_group_desc(g, gd)) return false;
    if (!e2_bitmap_clear(rd32(gd + 4), e2.inodes_per_group, idx)) return false;
    e2_super_counts(0, 1);
    e2_group_counts(g, 0, 1);
    return true;
}

static void e2_add_blocks(uint8_t *inode, uint32_t nblocks) {
    uint32_t *p = (uint32_t *)(inode + 28);
    *p += nblocks * (e2.block_size / 512);
}

static bool e2_new_block(uint32_t *out) {
    uint32_t blk;
    if (!e2_alloc_block(&blk)) return false;
    uint8_t *b = bounce2();
    memset(b, 0, e2.block_size);
    if (!e2_write_block(blk, b)) return false;
    *out = blk;
    return true;
}

static bool e2_map_block_alloc(uint8_t *inode, uint32_t lblk, uint32_t *out) {
    uint32_t *ib = (uint32_t *)(inode + 40);
    uint32_t ptrs = e2.block_size / 4;
    uint8_t *b = bounce();
    uint8_t *b2 = bounce2();

    if (lblk < 12) {
        if (!ib[lblk]) {
            uint32_t blk;
            if (!e2_new_block(&blk)) return false;
            ib[lblk] = blk;
            e2_add_blocks(inode, 1);
        }
        *out = ib[lblk];
        return true;
    }
    lblk -= 12;

    if (lblk < ptrs) {
        if (!ib[12]) {
            uint32_t blk;
            if (!e2_new_block(&blk)) return false;
            ib[12] = blk;
            e2_add_blocks(inode, 1);
        }
        if (!e2_read_block(ib[12], b)) return false;
        uint32_t *tab = (uint32_t *)b;
        if (!tab[lblk]) {
            uint32_t blk;
            if (!e2_new_block(&blk)) return false;
            tab[lblk] = blk;
            if (!e2_write_block(ib[12], b)) return false;
            e2_add_blocks(inode, 1);
        }
        *out = tab[lblk];
        return true;
    }
    lblk -= ptrs;

    if (lblk < ptrs * ptrs) {
        if (!ib[13]) {
            uint32_t blk;
            if (!e2_new_block(&blk)) return false;
            ib[13] = blk;
            e2_add_blocks(inode, 1);
        }
        if (!e2_read_block(ib[13], b)) return false;
        uint32_t *tab = (uint32_t *)b;
        uint32_t i1 = lblk / ptrs;
        uint32_t off = lblk % ptrs;

        if (!tab[i1]) {
            uint32_t blk;
            if (!e2_new_block(&blk)) return false;
            tab[i1] = blk;
            if (!e2_write_block(ib[13], b)) return false;
            e2_add_blocks(inode, 1);
        }
        uint32_t l2 = tab[i1];
        if (!e2_read_block(l2, b2)) return false;
        uint32_t *tab2 = (uint32_t *)b2;
        if (!tab2[off]) {
            uint32_t blk;
            if (!e2_new_block(&blk)) return false;
            tab2[off] = blk;
            if (!e2_write_block(l2, b2)) return false;
            e2_add_blocks(inode, 1);
        }
        *out = tab2[off];
        return true;
    }
    lblk -= ptrs * ptrs;

    if (lblk < ptrs * ptrs * ptrs) {
        if (!ib[14]) {
            uint32_t blk;
            if (!e2_new_block(&blk)) return false;
            ib[14] = blk;
            e2_add_blocks(inode, 1);
        }
        if (!e2_read_block(ib[14], b)) return false;
        uint32_t *tab = (uint32_t *)b;
        uint32_t i1 = lblk / (ptrs * ptrs);
        uint32_t rem = lblk % (ptrs * ptrs);
        uint32_t i2 = rem / ptrs;
        uint32_t i3 = rem % ptrs;

        if (!tab[i1]) {
            uint32_t blk;
            if (!e2_new_block(&blk)) return false;
            tab[i1] = blk;
            if (!e2_write_block(ib[14], b)) return false;
            e2_add_blocks(inode, 1);
        }
        uint32_t l2 = tab[i1];
        if (!e2_read_block(l2, b2)) return false;
        uint32_t *tab2 = (uint32_t *)b2;
        if (!tab2[i2]) {
            uint32_t blk;
            if (!e2_new_block(&blk)) return false;
            tab2[i2] = blk;
            if (!e2_write_block(l2, b2)) return false;
            e2_add_blocks(inode, 1);
        }
        uint32_t l3 = tab2[i2];
        if (!e2_read_block(l3, b)) return false;
        uint32_t *tab3 = (uint32_t *)b;
        if (!tab3[i3]) {
            uint32_t blk;
            if (!e2_new_block(&blk)) return false;
            tab3[i3] = blk;
            if (!e2_write_block(l3, b)) return false;
            e2_add_blocks(inode, 1);
        }
        *out = tab3[i3];
        return true;
    }

    return false;
}

static bool e2_map_block(const uint8_t *inode, uint32_t lblk, uint32_t *out) {
    const uint8_t *ib = inode + 40;
    uint32_t ptrs = e2.block_size / 4;
    preempt_disable();
    uint8_t *b = bounce();

    bool ok = false;

    if (lblk < 12) {
        uint32_t blk = rd32(ib + lblk * 4);
        if (blk) {
            *out = blk;
            ok = true;
        }
        goto done;
    }
    lblk -= 12;

    if (lblk < ptrs) {
        uint32_t blk = rd32(ib + 12 * 4);
        if (!blk) goto done;
        if (!e2_indirect_block(blk, b)) goto done;
        uint32_t v = rd32(b + lblk * 4);
        if (v) {
            *out = v;
            ok = true;
        }
        goto done;
    }
    lblk -= ptrs;

    if (lblk < ptrs * ptrs) {
        uint32_t blk = rd32(ib + 13 * 4);
        if (!blk) goto done;
        if (!e2_indirect_block(blk, b)) goto done;
        uint32_t idx = lblk / ptrs;
        uint32_t off = lblk % ptrs;
        uint32_t b2 = rd32(b + idx * 4);
        if (!b2) goto done;
        if (!e2_indirect_block(b2, b)) goto done;
        uint32_t v = rd32(b + off * 4);
        if (v) {
            *out = v;
            ok = true;
        }
        goto done;
    }
    lblk -= ptrs * ptrs;

    if (lblk < ptrs * ptrs * ptrs) {
        uint32_t blk = rd32(ib + 14 * 4);
        if (!blk) goto done;
        if (!e2_indirect_block(blk, b)) goto done;
        uint32_t i1 = lblk / (ptrs * ptrs);
        uint32_t rem = lblk % (ptrs * ptrs);
        uint32_t i2 = rem / ptrs;
        uint32_t i3 = rem % ptrs;
        uint32_t b2 = rd32(b + i1 * 4);
        if (!b2) goto done;
        if (!e2_indirect_block(b2, b)) goto done;
        uint32_t b3 = rd32(b + i2 * 4);
        if (!b3) goto done;
        if (!e2_indirect_block(b3, b)) goto done;
        uint32_t v = rd32(b + i3 * 4);
        if (v) {
            *out = v;
            ok = true;
        }
        goto done;
    }

done:
    preempt_enable();
    return ok;
}

static void e2_fill_node(fs_node_t *out, uint32_t ino, const uint8_t *inode,
                          const char *name) {
    uint16_t mode = rd16(inode + 0);
    uint32_t size = rd32(inode + 4);
    uint32_t size_high = rd32(inode + 108);
    bool is_dir = (mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    bool ro = ((mode & 0777) & 0222) == 0;

    if (!is_dir && (e2.feature_ro_compat & EXT2_RO_LARGE_FILE))
        size |= size_high << 16;

    memset(out, 0, sizeof(*out));
    if (name) {
        uint32_t n = strnlen(name, FS_MAX_NAME - 1);
        memcpy(out->name, name, n);
        out->name[n] = 0;
    } else {
        out->name[0] = '/';
        out->name[1] = 0;
    }
    out->attr = (is_dir ? FS_ATTR_DIRECTORY : 0) | (ro ? FS_ATTR_READ_ONLY : 0);
    out->cluster = ino;
    out->dir_cluster = ino;
    out->dir_index = 0;
    out->size = size;
    out->is_dir = is_dir;
    out->is_root = false;
    out->ops = &ext2_ops;
    out->dev_id = 1;
}

static bool e2_node_from_inode(uint32_t ino, const char *name, fs_node_t *out) {
    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return false;
    if (!e2_read_inode(ino, inode)) return false;
    e2_fill_node(out, ino, inode, name);
    return true;
}

typedef bool (*e2_dir_cb)(void *ctx, uint32_t ino, uint8_t ftype, const char *name,
                           uint32_t name_len);

static bool e2_scan_dir(const fs_node_t *dir, e2_dir_cb cb, void *ctx) {
    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return false;
    if (!e2_read_inode(dir->cluster, inode)) return false;

    uint32_t size = rd32(inode + 4);
    uint32_t blocks = (size + e2.block_size - 1) / e2.block_size;

    for (uint32_t bi = 0; bi < blocks; bi++) {
        uint32_t blk;
        if (!e2_map_block(inode, bi, &blk)) break;

        preempt_disable();
        uint8_t *b = bounce();
        bool ok = e2_read_block(blk, b);
        if (!ok) { preempt_enable(); break; }

        uint32_t off = 0;
        while (off + 8 <= e2.block_size) {
            uint32_t child = rd32(b + off);
            uint16_t rec_len = rd16(b + off + 4);
            uint16_t name_len = e2_filetype() ? b[off + 6] : rd16(b + off + 6);
            uint8_t ftype = e2_filetype() ? b[off + 7] : 0;

            if (rec_len < 8 || off + rec_len > e2.block_size) break;
            if (child == 0) { off += rec_len; continue; }
            if (name_len == 0 || name_len > rec_len - 8) { off += rec_len; continue; }

            char name[FS_MAX_NAME + 1];
            uint32_t n = name_len;
            if (n > FS_MAX_NAME) n = FS_MAX_NAME;
            memcpy(name, b + off + 8, n);
            name[n] = 0;

            if (!cb(ctx, child, ftype, name, n)) { preempt_enable(); return false; }
            off += rec_len;
        }
        preempt_enable();
    }
    return true;
}

struct e2_find_ctx {
    const char *name;
    uint32_t ino;
    uint32_t count;
    uint32_t want;
    bool found;
    fs_node_t node;
};

static bool e2_find_cb(void *vctx, uint32_t ino, uint8_t ftype, const char *name,
                       uint32_t name_len) {
    struct e2_find_ctx *ctx = vctx;
    (void)ftype;

    if (name[0] == '.' && name[1] == 0) return true;
    if (name[0] == '.' && name[1] == '.' && name[2] == 0) return true;

    if (ctx->name) {
        if (strcasecmp(name, ctx->name) != 0) return true;
        ctx->found = true;
        ctx->ino = ino;
        return false;
    }

    if (ctx->count == ctx->want) {
        if (!e2_node_from_inode(ino, name, &ctx->node)) return false;
        ctx->found = true;
        return false;
    }
    ctx->count++;
    return true;
}

static bool e2_lookup_locked(const char *path, fs_node_t *out) {
    if (!e2.mounted || !path || !out) return false;

    fs_node_t cur;
    if (!e2_node_from_inode(EXT2_ROOT_INODE, "/", &cur)) return false;
    uint32_t parent = EXT2_ROOT_INODE;

    char curpath[FS_PATH_MAX];
    curpath[0] = '/';
    curpath[1] = 0;

    char work[FS_PATH_MAX];
    uint32_t wn = 0;
    while (path[wn] && wn < FS_PATH_MAX - 1) {
        work[wn] = path[wn];
        wn++;
    }
    work[wn] = 0;

    int depth = 0;

    for (;;) {
        const char *p = work;
        bool restart = false;

        while (*p == '/') p++;

        while (*p) {
            char comp[FS_MAX_NAME + 1];
            uint32_t n = 0;
            while (*p && *p != '/' && n < FS_MAX_NAME) comp[n++] = *p++;
            comp[n] = 0;
            while (*p == '/') p++;
            if (!n) continue;

            if (comp[0] == '.' && comp[1] == 0) continue;
            if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
                if (!e2_node_from_inode(parent, "/", &cur)) return false;
                continue;
            }

            if (!cur.is_dir) return false;

            struct e2_find_ctx ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.name = comp;
            e2_scan_dir(&cur, e2_find_cb, &ctx);
            if (!ctx.found) return false;

            parent = cur.cluster;
            if (!e2_node_from_inode(ctx.ino, comp, &cur)) return false;

            uint16_t mode = 0;
            {
                uint8_t inode[256];
                if (e2.inode_size <= sizeof inode && e2_read_inode(cur.cluster, inode))
                    mode = rd16(inode + 0);
            }

            if ((mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
                if (++depth > 8) return false;

                char target[256];
                if (!e2_readlink_locked(&cur, target, sizeof target)) return false;

                char next[FS_PATH_MAX];
                if (target[0] == '/') {
                    uint32_t tn = 0;
                    while (target[tn] && tn < FS_PATH_MAX - 1) {
                        next[tn] = target[tn];
                        tn++;
                    }
                    next[tn] = 0;
                } else {
                    uint32_t tn = 0;
                    uint32_t dn = 0;
                    while (curpath[dn] && dn < FS_PATH_MAX - 1) dn++;
                    for (uint32_t i = 0; i < dn && tn < FS_PATH_MAX - 1; i++)
                        next[tn++] = curpath[i];
                    if (tn && next[tn - 1] != '/') next[tn++] = '/';
                    for (uint32_t i = 0; target[i] && tn < FS_PATH_MAX - 1; i++)
                        next[tn++] = target[i];
                    next[tn] = 0;
                }

                memcpy(work, next, sizeof work);
                wn = 0;
                while (work[wn]) wn++;

                if (!e2_node_from_inode(EXT2_ROOT_INODE, "/", &cur)) return false;
                parent = EXT2_ROOT_INODE;
                curpath[0] = '/';
                curpath[1] = 0;
                restart = true;
                break;
            }

            uint32_t cn = 0;
            while (curpath[cn] && cn < FS_PATH_MAX - 1) cn++;
            if (cn && curpath[cn - 1] != '/') curpath[cn++] = '/';
            for (uint32_t i = 0; comp[i] && cn < FS_PATH_MAX - 1; i++)
                curpath[cn++] = comp[i];
            curpath[cn] = 0;
        }

        if (!restart) break;
    }

    *out = cur;
    return true;
}

static bool e2_lookup(const char *path, fs_node_t *out) {
    bool ok;
    spin_lock(&e2_lock);
    ok = e2_lookup_locked(path, out);
    spin_unlock(&e2_lock);
    return ok;
}

static bool e2_iterate_locked(const fs_node_t *dir, uint32_t index, fs_node_t *out) {
    if (!e2.mounted || !dir || !out || !dir->is_dir) return false;

    struct e2_find_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.want = index;
    e2_scan_dir(dir, e2_find_cb, &ctx);
    if (!ctx.found) return false;
    *out = ctx.node;
    return true;
}

static bool e2_read_block_run(uint32_t first, uint32_t count, void *dst) {
    if (!e2.mounted || !dst || !count) return false;
    uint32_t sectors = e2.block_size / 512;
    uint64_t lba = (uint64_t)first * sectors;
    return block_read(e2.dev, lba, sectors * count, dst);
}

static bool e2_read_locked(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len) {
    if (!e2.mounted || !node || !buf) return false;
    if (node->is_dir) return false;
    if (!len) return true;
    if (offset >= node->size) return false;
    if (len > node->size - offset) len = node->size - offset;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return false;
    if (!e2_read_inode(node->cluster, inode)) return false;

    uint8_t *out = buf;
    while (len) {
        uint32_t lblk = offset / e2.block_size;
        uint32_t boff = offset % e2.block_size;

        if (boff) {
            uint32_t chunk = e2.block_size - boff;
            if (chunk > len) chunk = len;

            preempt_disable();
            uint8_t *b = bounce();
            uint32_t blk;
            bool mapped = e2_map_block(inode, lblk, &blk);
            bool ok = mapped && e2_read_block(blk, b);
            if (ok) memcpy(out, b + boff, chunk);
            preempt_enable();
            if (!mapped) {
                memset(out, 0, chunk);
            } else if (!ok) {
                return false;
            }

            out += chunk;
            len -= chunk;
            offset += chunk;
            continue;
        }

        uint32_t maxrun = len / e2.block_size;
        if (maxrun > 64) maxrun = 64;

        if (!maxrun) {
            preempt_disable();
            uint8_t *b = bounce();
            uint32_t blk;
            bool mapped = e2_map_block(inode, lblk, &blk);
            bool ok = mapped && e2_read_block(blk, b);
            if (ok) memcpy(out, b, len);
            preempt_enable();
            if (!mapped) memset(out, 0, len);
            else if (!ok) return false;

            out += len;
            offset += len;
            len = 0;
            continue;
        }

        uint32_t run = 0;
        uint32_t first = 0;
        uint32_t prev = 0;

        preempt_disable();
        for (uint32_t i = 0; i < maxrun; i++) {
            uint32_t blk;
            if (!e2_map_block(inode, lblk + i, &blk)) break;
            if (i) {
                if (blk != prev + 1) break;
            } else {
                first = blk;
            }
            prev = blk;
            run = i + 1;
        }
        preempt_enable();

        if (!run) {
            uint32_t chunk = e2.block_size;
            if (chunk > len) chunk = len;

            memset(out, 0, chunk);

            out += chunk;
            len -= chunk;
            offset += chunk;
            continue;
        }

        uint32_t bytes = run * e2.block_size;

        preempt_disable();
        bool ok = e2_read_block_run(first, run, out);
        preempt_enable();
        if (!ok) return false;

        out += bytes;
        len -= bytes;
        offset += bytes;
    }
    return true;
}

static bool e2_readlink_locked(const fs_node_t *node, char *buf, uint32_t size) {
    if (!e2.mounted || !node || !buf || !size) return false;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return false;
    if (!e2_read_inode(node->cluster, inode)) return false;

    uint16_t mode = rd16(inode + 0);
    if ((mode & EXT2_S_IFMT) != EXT2_S_IFLNK) return false;

    uint32_t len = rd32(inode + 4);
    if (len == 0 || len >= size) return false;

    if (len < 60) {
        memcpy(buf, inode + 40, len);
    } else {
        uint32_t blk;
        if (!e2_map_block(inode, 0, &blk)) return false;
        uint8_t *b = bounce();
        preempt_disable();
        bool ok = e2_read_block(blk, b);
        if (ok) memcpy(buf, b, len);
        preempt_enable();
        if (!ok) return false;
    }
    buf[len] = 0;
    return true;
}

static int e2_write_locked(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len) {
    if (!e2.mounted || !node || !buf) return ABI_EINVAL;
    if (node->is_dir) return ABI_EISDIR;
    if (!len) return 0;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return ABI_EINVAL;
    if (!e2_read_inode(node->cluster, inode)) return ABI_EIO;

    const uint8_t *in = buf;
    uint32_t remaining = len;
    uint32_t off = offset;

    while (remaining) {
        uint32_t lblk = off / e2.block_size;
        uint32_t boff = off % e2.block_size;
        uint32_t chunk = e2.block_size - boff;
        if (chunk > remaining) chunk = remaining;

        uint32_t blk;
        if (!e2_map_block_alloc(inode, lblk, &blk)) {
            e2_write_inode(node->cluster, inode);
            return ABI_ENOSPC;
        }

        uint8_t *b = bounce();
        preempt_disable();
        bool ok = e2_read_block(blk, b);
        if (ok) {
            memcpy(b + boff, in, chunk);
            ok = e2_write_block(blk, b);
        }
        preempt_enable();
        if (!ok) {
            e2_write_inode(node->cluster, inode);
            return ABI_EIO;
        }

        in += chunk;
        remaining -= chunk;
        off += chunk;
    }

    uint32_t *size = (uint32_t *)(inode + 4);
    if (off > *size) *size = off;
    uint32_t *mtime = (uint32_t *)(inode + 16);
    *mtime = (uint32_t)rtc_unix_time();

    if (!e2_write_inode(node->cluster, inode)) return ABI_EIO;
    node->size = *size;
    return (int)len;
}

static void e2_free_tables(uint8_t *inode) {
    uint8_t *ib = inode + 40;
    uint32_t ptrs = e2.block_size / 4;
    uint8_t *b = bounce();
    uint8_t *c = bounce2();

    uint32_t t1 = rd32(ib + 48);
    if (t1) e2_free_block(t1);

    t1 = rd32(ib + 52);
    if (t1) {
        if (e2_read_block(t1, b)) {
            for (uint32_t i = 0; i < ptrs; i++) {
                uint32_t t2 = rd32(b + i * 4);
                if (t2) e2_free_block(t2);
            }
        }
        e2_free_block(t1);
    }

    t1 = rd32(ib + 56);
    if (t1) {
        if (e2_read_block(t1, b)) {
            for (uint32_t i = 0; i < ptrs; i++) {
                uint32_t t2 = rd32(b + i * 4);
                if (!t2) continue;
                if (e2_read_block(t2, c)) {
                    for (uint32_t j = 0; j < ptrs; j++) {
                        uint32_t t3 = rd32(c + j * 4);
                        if (t3) e2_free_block(t3);
                    }
                }
                e2_free_block(t2);
            }
        }
        e2_free_block(t1);
    }

    uint32_t *ibp = (uint32_t *)(inode + 40);
    for (int i = 12; i < 15; i++) ibp[i] = 0;
}

static bool e2_free_tail(uint8_t *inode, uint32_t new_blocks) {
    uint32_t old_blocks = (rd32(inode + 4) + e2.block_size - 1) / e2.block_size;
    if (new_blocks >= old_blocks) return true;

    for (uint32_t l = new_blocks; l < old_blocks; l++) {
        uint32_t blk;
        if (!e2_map_block(inode, l, &blk)) continue;
        e2_free_block(blk);
    }

    if (new_blocks == 0) e2_free_tables(inode);
    return true;
}

static int e2_truncate_locked(fs_node_t *node) {
    if (!e2.mounted || !node) return ABI_EINVAL;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return ABI_EINVAL;
    if (!e2_read_inode(node->cluster, inode)) return ABI_EIO;

    e2_free_tail(inode, 0);

    uint32_t *size = (uint32_t *)(inode + 4);
    *size = 0;
    uint32_t *blocks = (uint32_t *)(inode + 28);
    *blocks = 0;
    uint32_t *ib = (uint32_t *)(inode + 40);
    for (int i = 0; i < 15; i++) ib[i] = 0;
    uint32_t *mtime = (uint32_t *)(inode + 16);
    *mtime = (uint32_t)rtc_unix_time();

    if (!e2_write_inode(node->cluster, inode)) return ABI_EIO;
    node->size = 0;
    return 0;
}

static int e2_resize_locked(fs_node_t *node, uint32_t size) {
    if (!e2.mounted || !node) return ABI_EINVAL;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return ABI_EINVAL;
    if (!e2_read_inode(node->cluster, inode)) return ABI_EIO;

    uint32_t new_blocks = (size + e2.block_size - 1) / e2.block_size;
    e2_free_tail(inode, new_blocks);

    uint32_t *sz = (uint32_t *)(inode + 4);
    *sz = size;
    uint32_t *mtime = (uint32_t *)(inode + 16);
    *mtime = (uint32_t)rtc_unix_time();

    if (!e2_write_inode(node->cluster, inode)) return ABI_EIO;
    node->size = size;
    return 0;
}

static bool e2_split_path(const char *path, char *dir, size_t dir_max, char *name,
                          size_t name_max) {
    if (!path) return false;
    size_t n = strlen(path);
    while (n && path[n - 1] == '/') n--;
    if (!n) return false;

    size_t slash = n;
    while (slash && path[slash - 1] != '/') slash--;

    size_t name_len = n - slash;
    if (!name_len || name_len >= name_max) return false;
    memcpy(name, path + slash, name_len);
    name[name_len] = 0;

    size_t dir_len = slash;
    while (dir_len && path[dir_len - 1] == '/') dir_len--;
    if (dir_len >= dir_max) return false;
    memcpy(dir, path, dir_len);
    dir[dir_len] = 0;
    if (!dir_len) {
        dir[0] = '/';
        dir[1] = 0;
    }
    return true;
}

static bool e2_dir_find(const fs_node_t *dir, const char *name, uint32_t *out_ino,
                         uint32_t *out_block, uint32_t *out_off) {
    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return false;
    if (!e2_read_inode(dir->cluster, inode)) return false;

    uint32_t size = rd32(inode + 4);
    uint32_t blocks = (size + e2.block_size - 1) / e2.block_size;

    for (uint32_t bi = 0; bi < blocks; bi++) {
        uint32_t blk;
        if (!e2_map_block(inode, bi, &blk)) break;

        preempt_disable();
        uint8_t *b = bounce();
        bool ok = e2_read_block(blk, b);
        if (!ok) { preempt_enable(); break; }

        uint32_t off = 0;
        while (off + 8 <= e2.block_size) {
            uint32_t child = rd32(b + off);
            uint16_t rec_len = rd16(b + off + 4);
            uint16_t name_len = e2_filetype() ? b[off + 6] : rd16(b + off + 6);
            if (rec_len < 8 || off + rec_len > e2.block_size) break;
            if (child && name_len && name_len <= rec_len - 8) {
                if (name_len == strlen(name) &&
                    memcmp(b + off + 8, name, name_len) == 0) {
                    if (out_ino) *out_ino = child;
                    if (out_block) *out_block = blk;
                    if (out_off) *out_off = off;
                    preempt_enable();
                    return true;
                }
            }
            off += rec_len;
        }
        preempt_enable();
    }
    return false;
}

static bool e2_touch_dir(fs_node_t *dir) {
    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return false;
    if (!e2_read_inode(dir->cluster, inode)) return false;
    uint32_t *mtime = (uint32_t *)(inode + 16);
    *mtime = (uint32_t)rtc_unix_time();
    return e2_write_inode(dir->cluster, inode);
}

static bool e2_dir_add(fs_node_t *dir, uint32_t ino, uint8_t ftype, const char *name) {
    uint32_t name_len = (uint32_t)strlen(name);
    if (!name_len || name_len > 255) return false;
    uint32_t need = 8 + name_len;
    need = (need + 3) & ~3u;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return false;
    if (!e2_read_inode(dir->cluster, inode)) return false;

    uint32_t size = rd32(inode + 4);
    uint32_t blocks = (size + e2.block_size - 1) / e2.block_size;

    for (uint32_t bi = 0; bi < blocks; bi++) {
        uint32_t blk;
        if (!e2_map_block(inode, bi, &blk)) break;

        preempt_disable();
        uint8_t *b = bounce();
        bool ok = e2_read_block(blk, b);
        if (!ok) { preempt_enable(); break; }

        uint32_t off = 0;
        while (off + 8 <= e2.block_size) {
            uint32_t child = rd32(b + off);
            uint16_t rec_len = rd16(b + off + 4);
            if (rec_len < 8 || off + rec_len > e2.block_size) break;

            if (!child) {
                if (rec_len >= need) {
                    uint32_t *di = (uint32_t *)(b + off);
                    uint16_t *dr = (uint16_t *)(b + off + 4);
                    *di = ino;
                    *dr = (uint16_t)rec_len;
                    b[off + 6] = (uint8_t)name_len;
                    if (e2_filetype()) b[off + 7] = ftype;
                    else b[off + 7] = 0;
                    memcpy(b + off + 8, name, name_len);
                    ok = e2_write_block(blk, b);
                    preempt_enable();
                    e2_touch_dir(dir);
                    return ok;
                }
                off += rec_len;
                continue;
            }

            off += rec_len;
        }
        preempt_enable();
    }

    uint32_t nblk = 0;
    if (!e2_map_block_alloc(inode, blocks, &nblk)) return false;

    uint8_t *b = bounce();
    memset(b, 0, e2.block_size);
    uint32_t *di = (uint32_t *)b;
    uint16_t *dr = (uint16_t *)(b + 4);
    *di = ino;
    *dr = (uint16_t)e2.block_size;
    b[6] = (uint8_t)name_len;
    b[7] = e2_filetype() ? ftype : 0;
    memcpy(b + 8, name, name_len);
    if (!e2_write_block(nblk, b)) return false;

    uint32_t *sz = (uint32_t *)(inode + 4);
    *sz = (blocks + 1) * e2.block_size;
    uint32_t *mtime = (uint32_t *)(inode + 16);
    *mtime = (uint32_t)rtc_unix_time();
    if (!e2_write_inode(dir->cluster, inode)) return false;
    return true;
}

static bool e2_dir_remove(fs_node_t *dir, uint32_t blk, uint32_t off) {
    uint8_t *b = bounce();
    preempt_disable();
    bool ok = e2_read_block(blk, b);
    if (ok) {
        uint32_t *di = (uint32_t *)(b + off);
        *di = 0;
        ok = e2_write_block(blk, b);
    }
    preempt_enable();
    return ok;
}

static int e2_create_locked(const char *path, fs_node_t *out) {
    fs_node_t tmp;
    if (!out) out = &tmp;
    char dirpath[FS_PATH_MAX];
    char name[FS_MAX_NAME + 1];
    if (!e2_split_path(path, dirpath, sizeof dirpath, name, sizeof name)) return ABI_EINVAL;

    fs_node_t dir;
    if (!e2_lookup_locked(dirpath, &dir)) return ABI_ENOENT;
    if (!dir.is_dir) return ABI_ENOTDIR;

    uint32_t have = 0, hblk = 0, hoff = 0;
    if (e2_dir_find(&dir, name, &have, &hblk, &hoff)) return ABI_EEXIST;

    uint32_t ino = 0;
    if (!e2_alloc_inode(&ino)) return ABI_ENOSPC;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return ABI_EINVAL;
    memset(inode, 0, sizeof inode);
    uint16_t *mode = (uint16_t *)(inode + 0);
    *mode = 0x8000 | 0644;
    uint16_t *links = (uint16_t *)(inode + 26);
    *links = 1;
    uint32_t *mtime = (uint32_t *)(inode + 16);
    *mtime = (uint32_t)rtc_unix_time();
    uint32_t *ctime = (uint32_t *)(inode + 12);
    *ctime = *mtime;
    uint32_t *atime = (uint32_t *)(inode + 8);
    *atime = *mtime;

    if (!e2_write_inode(ino, inode)) {
        e2_free_inode(ino);
        return ABI_EIO;
    }

    if (!e2_dir_add(&dir, ino, EXT2_FT_REG, name)) {
        e2_free_inode(ino);
        return ABI_ENOSPC;
    }

    if (!e2_node_from_inode(ino, name, out)) return ABI_EIO;
    return 0;
}

static int e2_mkdir_locked(const char *path, fs_node_t *out) {
    fs_node_t tmp;
    if (!out) out = &tmp;
    char dirpath[FS_PATH_MAX];
    char name[FS_MAX_NAME + 1];
    if (!e2_split_path(path, dirpath, sizeof dirpath, name, sizeof name)) return ABI_EINVAL;

    fs_node_t dir;
    if (!e2_lookup_locked(dirpath, &dir)) return ABI_ENOENT;
    if (!dir.is_dir) return ABI_ENOTDIR;

    uint32_t have = 0, hblk = 0, hoff = 0;
    if (e2_dir_find(&dir, name, &have, &hblk, &hoff)) return ABI_EEXIST;

    uint32_t ino = 0;
    if (!e2_alloc_inode(&ino)) return ABI_ENOSPC;

    uint32_t blk = 0;
    if (!e2_new_block(&blk)) {
        e2_free_inode(ino);
        return ABI_ENOSPC;
    }

    uint8_t dbuf[EXT2_BLOCK_MAX];
    memset(dbuf, 0, sizeof dbuf);

    uint32_t *di = (uint32_t *)dbuf;
    uint16_t *dr = (uint16_t *)(dbuf + 4);
    uint8_t *dn = dbuf + 6;
    uint8_t *dt = dbuf + 7;
    di[0] = ino; dr[0] = 12; *dn = 1; *dt = e2_filetype() ? EXT2_FT_DIR : 0; dbuf[8] = '.';
    di[3] = dir.cluster; dr[3] = 12; dn[3] = 2; dt[3] = e2_filetype() ? EXT2_FT_DIR : 0;
    dbuf[11] = '.'; dbuf[12] = '.';

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return ABI_EINVAL;
    memset(inode, 0, sizeof inode);
    uint16_t *mode = (uint16_t *)(inode + 0);
    *mode = 0x4000 | 0755;
    uint16_t *links = (uint16_t *)(inode + 26);
    *links = 2;
    uint32_t *size = (uint32_t *)(inode + 4);
    *size = e2.block_size;
    uint32_t *blocks = (uint32_t *)(inode + 28);
    *blocks = e2.block_size / 512;
    uint32_t *ib = (uint32_t *)(inode + 40);
    ib[0] = blk;
    uint32_t *mtime = (uint32_t *)(inode + 16);
    *mtime = (uint32_t)rtc_unix_time();

    if (!e2_write_block(blk, dbuf) || !e2_write_inode(ino, inode)) {
        e2_free_block(blk);
        e2_free_inode(ino);
        return ABI_EIO;
    }

    if (!e2_dir_add(&dir, ino, EXT2_FT_DIR, name)) {
        e2_free_block(blk);
        e2_free_inode(ino);
        return ABI_ENOSPC;
    }

    if (!e2_node_from_inode(ino, name, out)) return ABI_EIO;
    return 0;
}

static int e2_unlink_locked(const char *path) {
    char dirpath[FS_PATH_MAX];
    char name[FS_MAX_NAME + 1];
    if (!e2_split_path(path, dirpath, sizeof dirpath, name, sizeof name)) return ABI_EINVAL;

    fs_node_t dir;
    if (!e2_lookup_locked(dirpath, &dir)) return ABI_ENOENT;
    if (!dir.is_dir) return ABI_ENOTDIR;

    uint32_t ino = 0, dblk = 0, doff = 0;
    if (!e2_dir_find(&dir, name, &ino, &dblk, &doff)) return ABI_ENOENT;

    fs_node_t node;
    if (!e2_node_from_inode(ino, name, &node)) return ABI_EIO;
    if (node.is_dir) return ABI_EISDIR;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return ABI_EINVAL;
    if (!e2_read_inode(ino, inode)) return ABI_EIO;

    e2_free_tail(inode, 0);

    memset(inode, 0, sizeof inode);
    e2_write_inode(ino, inode);
    e2_free_inode(ino);

    if (!e2_dir_remove(&dir, dblk, doff)) return ABI_EIO;
    e2_touch_dir(&dir);
    return 0;
}

static int e2_rmdir_locked(const char *path) {
    char dirpath[FS_PATH_MAX];
    char name[FS_MAX_NAME + 1];
    if (!e2_split_path(path, dirpath, sizeof dirpath, name, sizeof name)) return ABI_EINVAL;

    fs_node_t dir;
    if (!e2_lookup_locked(dirpath, &dir)) return ABI_ENOENT;
    if (!dir.is_dir) return ABI_ENOTDIR;

    uint32_t ino = 0, dblk = 0, doff = 0;
    if (!e2_dir_find(&dir, name, &ino, &dblk, &doff)) return ABI_ENOENT;

    fs_node_t node;
    if (!e2_node_from_inode(ino, name, &node)) return ABI_EIO;
    if (!node.is_dir) return ABI_ENOTDIR;

    fs_node_t sub;
    if (!e2_node_from_inode(ino, name, &sub)) return ABI_EIO;
    uint32_t count = 0;
    fs_node_t ent;
    while (e2_iterate_locked(&sub, count, &ent)) count++;
    if (count) return ABI_ENOTEMPTY;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return ABI_EINVAL;
    if (!e2_read_inode(ino, inode)) return ABI_EIO;

    e2_free_tail(inode, 0);

    memset(inode, 0, sizeof inode);
    e2_write_inode(ino, inode);
    e2_free_inode(ino);

    if (!e2_dir_remove(&dir, dblk, doff)) return ABI_EIO;
    e2_touch_dir(&dir);
    return 0;
}

static int e2_utimens(const char *path, int64_t mtime) {
    (void)path; (void)mtime;
    return ABI_EROFS;
}

static int e2_node_utimens(fs_node_t *node, int64_t mtime) {
    (void)node; (void)mtime;
    return ABI_EROFS;
}

static bool e2_sync(void) {
    return true;
}


static bool e2_iterate(const fs_node_t *dir, uint32_t index, fs_node_t *out) {
    bool ok;
    spin_lock(&e2_lock);
    ok = e2_iterate_locked(dir, index, out);
    spin_unlock(&e2_lock);
    return ok;
}

static bool e2_read(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len) {
    bool ok;
    spin_lock(&e2_lock);
    ok = e2_read_locked(node, offset, buf, len);
    spin_unlock(&e2_lock);
    return ok;
}

static bool e2_readlink_path_locked(const char *path, char *buf, uint32_t size) {
    if (!e2.mounted || !path || !buf || !size) return false;

    char dirpath[FS_PATH_MAX];
    char name[FS_MAX_NAME + 1];
    if (!e2_split_path(path, dirpath, sizeof dirpath, name, sizeof name)) return false;

    fs_node_t dir;
    if (!e2_lookup_locked(dirpath, &dir) || !dir.is_dir) return false;

    uint32_t ino = 0, blk = 0, off = 0;
    if (!e2_dir_find(&dir, name, &ino, &blk, &off)) return false;

    fs_node_t node;
    if (!e2_node_from_inode(ino, name, &node)) return false;

    return e2_readlink_locked(&node, buf, size);
}

static bool e2_readlink(const char *path, char *buf, uint32_t size) {
    bool ok;
    spin_lock(&e2_lock);
    ok = e2_readlink_path_locked(path, buf, size);
    spin_unlock(&e2_lock);
    return ok;
}

static int e2_symlink_locked(const char *target, const char *path, fs_node_t *out) {
    if (!e2.mounted || !target || !path) return ABI_EINVAL;
    fs_node_t tmp;
    if (!out) out = &tmp;

    uint32_t tlen = (uint32_t)strlen(target);
    if (!tlen || tlen >= 4096) return ABI_EINVAL;

    char dirpath[FS_PATH_MAX];
    char name[FS_MAX_NAME + 1];
    if (!e2_split_path(path, dirpath, sizeof dirpath, name, sizeof name)) return ABI_EINVAL;

    fs_node_t dir;
    if (!e2_lookup_locked(dirpath, &dir)) return ABI_ENOENT;
    if (!dir.is_dir) return ABI_ENOTDIR;

    uint32_t ino = 0;
    if (!e2_alloc_inode(&ino)) return ABI_ENOSPC;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return ABI_EINVAL;
    memset(inode, 0, sizeof inode);

    uint16_t *mode = (uint16_t *)(inode + 0);
    *mode = EXT2_S_IFLNK | 0777;
    uint16_t *links = (uint16_t *)(inode + 26);
    *links = 1;
    uint32_t *size = (uint32_t *)(inode + 4);
    *size = tlen;
    uint32_t *mtime = (uint32_t *)(inode + 16);
    *mtime = (uint32_t)rtc_unix_time();

    if (tlen < 60) {
        memcpy(inode + 40, target, tlen);
    } else {
        uint32_t blk = 0;
        if (!e2_new_block(&blk)) {
            e2_free_inode(ino);
            return ABI_ENOSPC;
        }
        uint8_t *b = bounce();
        memset(b, 0, e2.block_size);
        memcpy(b, target, tlen);
        if (!e2_write_block(blk, b)) {
            e2_free_block(blk);
            e2_free_inode(ino);
            return ABI_EIO;
        }
        uint32_t *ib = (uint32_t *)(inode + 40);
        ib[0] = blk;
        uint32_t *blocks = (uint32_t *)(inode + 28);
        *blocks = e2.block_size / 512;
    }

    if (!e2_write_inode(ino, inode)) {
        e2_free_inode(ino);
        return ABI_EIO;
    }

    if (!e2_dir_add(&dir, ino, EXT2_FT_LNK, name)) {
        e2_free_inode(ino);
        return ABI_ENOSPC;
    }

    if (!e2_node_from_inode(ino, name, out)) return ABI_EIO;
    return 0;
}

static int e2_symlink(const char *target, const char *path, fs_node_t *out) {
    int r;
    spin_lock(&e2_lock);
    r = e2_symlink_locked(target, path, out);
    spin_unlock(&e2_lock);
    return r;
}

static int e2_chmod_locked(fs_node_t *node, uint32_t mode) {
    if (!e2.mounted || !node) return ABI_EINVAL;

    uint8_t inode[256];
    if (e2.inode_size > sizeof inode) return ABI_EINVAL;
    if (!e2_read_inode(node->cluster, inode)) return ABI_EIO;

    uint16_t *imode = (uint16_t *)(inode + 0);
    uint16_t type = (uint16_t)(*imode & 0xF000);
    *imode = (uint16_t)(type | (mode & 07777));

    uint32_t *ctime = (uint32_t *)(inode + 12);
    *ctime = (uint32_t)rtc_unix_time();

    if (!e2_write_inode(node->cluster, inode)) return ABI_EIO;
    return 0;
}

static int e2_write(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len) {
    int r;
    spin_lock(&e2_lock);
    r = e2_write_locked(node, offset, buf, len);
    spin_unlock(&e2_lock);
    return r;
}

static int e2_chmod(fs_node_t *node, uint32_t mode) {
    int r;
    spin_lock(&e2_lock);
    r = e2_chmod_locked(node, mode);
    spin_unlock(&e2_lock);
    return r;
}

static int e2_truncate(fs_node_t *node) {
    int r;
    spin_lock(&e2_lock);
    r = e2_truncate_locked(node);
    spin_unlock(&e2_lock);
    return r;
}

static int e2_resize(fs_node_t *node, uint32_t size) {
    int r;
    spin_lock(&e2_lock);
    r = e2_resize_locked(node, size);
    spin_unlock(&e2_lock);
    return r;
}

static int e2_create(const char *path, fs_node_t *out) {
    int r;
    spin_lock(&e2_lock);
    r = e2_create_locked(path, out);
    spin_unlock(&e2_lock);
    return r;
}

static int e2_mkdir(const char *path, fs_node_t *out) {
    int r;
    spin_lock(&e2_lock);
    r = e2_mkdir_locked(path, out);
    spin_unlock(&e2_lock);
    return r;
}

static int e2_unlink(const char *path) {
    int r;
    spin_lock(&e2_lock);
    r = e2_unlink_locked(path);
    spin_unlock(&e2_lock);
    return r;
}

static int e2_rmdir(const char *path) {
    int r;
    spin_lock(&e2_lock);
    r = e2_rmdir_locked(path);
    spin_unlock(&e2_lock);
    return r;
}

const struct fs_ops ext2_ops = {
    .iterate = e2_iterate,
    .lookup = e2_lookup,
    .read = e2_read,
    .write = e2_write,
    .truncate = e2_truncate,
    .resize = e2_resize,
    .create = e2_create,
    .mkdir = e2_mkdir,
    .unlink = e2_unlink,
    .rmdir = e2_rmdir,
    .utimens = e2_utimens,
    .node_utimens = e2_node_utimens,
    .sync = e2_sync,
    .chmod = e2_chmod,
    .symlink = e2_symlink,
    .readlink = e2_readlink,
};

bool ext2_mount(block_device_t *dev) {
    if (!dev || !dev->read) return false;

    memset(&e2, 0, sizeof e2);

    uint8_t sb[1024];
    if (!block_read(dev, EXT2_SUPER_OFFSET / 512, 2, sb)) return false;

    if (rd16(sb + 56) != EXT2_MAGIC) return false;

    uint32_t log_block_size = rd32(sb + 24);
    if (log_block_size > 2) return false;
    uint32_t block_size = 1024u << log_block_size;
    if (block_size > EXT2_BLOCK_MAX) return false;

    e2.dev = dev;
    e2.block_size = block_size;
    e2.inodes_count = rd32(sb + 0);
    e2.blocks_count = rd32(sb + 4);
    e2.first_data_block = rd32(sb + 20);
    e2.blocks_per_group = rd32(sb + 32);
    e2.inodes_per_group = rd32(sb + 40);
    e2.feature_compat = rd32(sb + 92);
    e2.feature_incompat = rd32(sb + 96);
    e2.feature_ro_compat = rd32(sb + 100);
    e2.inode_size = rd16(sb + 88);
    if (!e2.inode_size) e2.inode_size = 128;
    e2.desc_size = 32;
    if (e2.feature_incompat & EXT2_INCOMPAT_64BIT) {
        uint32_t ds = rd16(sb + 254);
        if (ds >= 32) e2.desc_size = ds;
    }

    if (e2.feature_incompat & EXT2_INCOMPAT_EXTENTS) return false;
    if (e2.feature_ro_compat & EXT2_RO_DIR_INDEX) return false;

    if (!e2.blocks_per_group || !e2.inodes_per_group) return false;
    if (e2.blocks_count < e2.first_data_block) return false;

    e2.group_count = (e2.blocks_count - e2.first_data_block + e2.blocks_per_group - 1) /
                      e2.blocks_per_group;
    if (!e2.group_count) return false;

    memcpy(e2.label, sb + 120, 16);
    e2.label[16] = 0;

    e2.mounted = true;

    for (uint32_t id = 0; id < MAX_CPUS; id++) e2_icache_blk[id] = 0;

    kprintf(PRINT_SERIAL, "EXT2: mounted %s, block size %u, %u blocks, %u inodes, "
            "%u groups, inode size %u\n",
            e2.label[0] ? e2.label : "(no label)", e2.block_size, e2.blocks_count,
            e2.inodes_count, e2.group_count, e2.inode_size);
    return true;
}

typedef struct {
    fs_node_t node;
    bool valid;
} ext2_file_dev_t;

static ext2_file_dev_t e2_file;
static block_device_t e2_file_blk;

static bool e2_file_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    ext2_file_dev_t *f = ctx;
    if (!f->valid) return false;
    uint64_t off = lba * 512;
    if (off + (uint64_t)count * 512 > f->node.size) return false;
    return fs_node_read(&f->node, (uint32_t)off, buf, count * 512);
}

static bool e2_file_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    ext2_file_dev_t *f = ctx;
    if (!f->valid) return false;
    uint64_t off = lba * 512;
    if (off + (uint64_t)count * 512 > f->node.size) return false;
    int r = fs_node_write(&f->node, (uint32_t)off, (void *)buf, count * 512);
    return r == (int)(count * 512);
}

bool ext2_mount_file(const char *path) {
    fs_node_t node;
    if (!fs_lookup(path, &node)) return false;
    if (!node.is_dir && node.size < 4096) return false;

    e2_file.node = node;
    e2_file.valid = true;

    memset(&e2_file_blk, 0, sizeof e2_file_blk);
    e2_file_blk.sector_size = 512;
    e2_file_blk.sector_shift = 9;
    e2_file_blk.sector_count = node.size / 512;
    e2_file_blk.read_only = false;
    e2_file_blk.ctx = &e2_file;
    e2_file_blk.read = e2_file_read;
    e2_file_blk.write = e2_file_write;

    return ext2_mount(&e2_file_blk);
}
