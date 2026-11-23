#include <boot/limine_requests.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <fs/ext2.hpp>
#include <lib/crc32c.hpp>
#include <lib/format.hpp>
#include <lib/random.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/vmm.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <sched/sync.hpp>
#include <time/rtc.hpp>

namespace ext2 {

u64 g_cached_pages = 0; // page cache pages held by all ext2/ext4 volumes (/proc/meminfo Cached)

namespace {

constexpr u16 MAGIC = 0xEF53;
constexpr u32 ROOT_INO = 2;

// Feature bits.
constexpr u32 COMPAT_HAS_JOURNAL = 0x4;
constexpr u32 INCOMPAT_FILETYPE = 0x2, INCOMPAT_RECOVER = 0x4, INCOMPAT_META_BG = 0x10, INCOMPAT_EXTENTS = 0x40,
              INCOMPAT_64BIT = 0x80, INCOMPAT_FLEX_BG = 0x200,
              INCOMPAT_CSUM_SEED = 0x2000, INCOMPAT_LARGEDIR = 0x4000;
constexpr u32 INCOMPAT_READ_OK = INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_EXTENTS | INCOMPAT_64BIT |
                                 INCOMPAT_FLEX_BG | INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR | INCOMPAT_META_BG;
// Writable: everything Debian's mkfs.ext4 turns on. Not (yet): a journal that
// needs replaying (RECOVER), meta_bg, inline data, encryption, casefolding.
constexpr u32 INCOMPAT_WRITE_OK = INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG |
                                  INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR;
constexpr u32 RO_SPARSE_SUPER = 0x1, RO_LARGE_FILE = 0x2, RO_HUGE_FILE = 0x8, RO_GDT_CSUM = 0x10,
              RO_DIR_NLINK = 0x20, RO_EXTRA_ISIZE = 0x40, RO_METADATA_CSUM = 0x400;
constexpr u32 RO_WRITE_OK = RO_SPARSE_SUPER | RO_LARGE_FILE | RO_HUGE_FILE | RO_GDT_CSUM | RO_DIR_NLINK |
                            RO_EXTRA_ISIZE | RO_METADATA_CSUM;

constexpr u16 BG_INODE_UNINIT = 0x1, BG_BLOCK_UNINIT = 0x2;

constexpr u32 FLAG_INDEX = 0x1000, FLAG_EXTENTS = 0x80000, FLAG_INLINE = 0x10000000;
constexpr u8 FT_UNKNOWN = 0, FT_REG = 1, FT_DIR = 2, FT_CHR = 3, FT_BLK = 4, FT_FIFO = 5, FT_SOCK = 6, FT_SYMLINK = 7;

constexpr u16 EXT_MAGIC = 0xF30A;
constexpr u32 EXT_INIT_MAX_LEN = 32768;

struct PACKED RawInode {
    u16 mode, uid;
    u32 size_lo, atime, ctime, mtime, dtime;
    u16 gid, links;
    u32 blocks_lo, flags, osd1;
    u32 block[15];
    u32 generation, file_acl_lo, size_hi, faddr;
    u8 osd2[12];
};
static_assert(sizeof(RawInode) == 128);

u8 type_for_mode(u32 mode) {
    switch (mode & vfs::S_IFMT) {
    case vfs::S_IFREG: return FT_REG;
    case vfs::S_IFDIR: return FT_DIR;
    case vfs::S_IFLNK: return FT_SYMLINK;
    case vfs::S_IFCHR: return FT_CHR;
    case vfs::S_IFBLK: return FT_BLK;
    case vfs::S_IFIFO: return FT_FIFO;
    case vfs::S_IFSOCK: return FT_SOCK;
    default: return FT_UNKNOWN;
    }
}

u8 dtype_for(u8 ft) {
    switch (ft) {
    case FT_REG: return vfs::DT_REG;
    case FT_DIR: return vfs::DT_DIR;
    case FT_SYMLINK: return vfs::DT_LNK;
    case FT_CHR: return vfs::DT_CHR;
    case FT_BLK: return vfs::DT_BLK;
    case FT_FIFO: return vfs::DT_FIFO;
    default: return vfs::DT_UNKNOWN;
    }
}

u32 now_seconds() { return static_cast<u32>(rtc::realtime_ns() / 1'000'000'000); }

// Little-endian field access in on-disk structures.
u16 get16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }
u32 get32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
void put16(u8* p, u16 v) { memcpy(p, &v, 2); }
void put32(u8* p, u32 v) { memcpy(p, &v, 4); }

// jbd2 (the journal) stores everything big-endian.
u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
void put_be32(u8* p, u32 v) { p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v); }
void put_be16(u8* p, u16 v) { p[0] = u8(v >> 8); p[1] = u8(v); }

constexpr u32 JBD2_MAGIC = 0xC03B3998;
constexpr u32 JBD2_DESCRIPTOR = 1, JBD2_COMMIT = 2, JBD2_SB_V1 = 3, JBD2_SB_V2 = 4, JBD2_REVOKE = 5;
constexpr u32 JBD2_INCOMPAT_REVOKE = 0x1, JBD2_INCOMPAT_64BIT = 0x2, JBD2_INCOMPAT_ASYNC_COMMIT = 0x4,
              JBD2_INCOMPAT_CSUM_V3 = 0x10;
constexpr u32 JBD2_TAG_ESCAPE = 1, JBD2_TAG_SAME_UUID = 2, JBD2_TAG_LAST = 8;

// CRC-16 (ANSI, reflected 0xA001): the older "uninit_bg" group descriptor checksum.
u16 crc16(u16 crc, const void* data, usize len) {
    auto* p = static_cast<const u8*>(data);
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? static_cast<u16>((crc >> 1) ^ 0xA001) : static_cast<u16>(crc >> 1);
    }
    return crc;
}

// A small growable array (the kernel has no std::vector).
template <typename T>
struct Array {
    T* v = nullptr;
    usize n = 0, cap = 0;
    Array() = default;
    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;
    ~Array() { kfree(v); }
    bool push(const T& item) {
        if (n == cap) {
            usize ncap = cap ? cap * 2 : 16;
            T* nv = static_cast<T*>(kmalloc(ncap * sizeof(T)));
            if (!nv)
                return false;
            if (v)
                memcpy(nv, v, n * sizeof(T));
            kfree(v);
            v = nv;
            cap = ncap;
        }
        v[n++] = item;
        return true;
    }
    bool insert(usize at, const T& item) {
        if (!push(item))
            return false;
        for (usize i = n - 1; i > at; i--)
            v[i] = v[i - 1];
        v[at] = item;
        return true;
    }
};

struct Extent {
    u32 lblk; // first file block
    u32 len;
    u64 pblk; // first disk block
    bool uninit;
};

class Node;

class Volume {
public:
    block::Device* dev;
    u32 block_size;
    u32 inodes_per_group, blocks_per_group, inode_size, group_count, desc_size;
    u64 blocks_count, first_data_block, gdt_block;
    u32 compat, incompat, ro_compat;
    bool read_only;
    bool has_filetype;
    bool csum = false;     // metadata_csum: crc32c on everything
    bool gdt_csum = false; // uninit_bg: crc16 on group descriptors only
    bool sparse = false;
    bool extents = false;  // new files use extent trees
    u32 csum_seed = 0;
    u8 uuid[16] = {};
    u32 gdt_blocks = 0, reserved_gdt = 0, first_ino = 11;
    u16 extra_isize = 0;
    u64 fs_dev = vfs::next_fs_dev();
    sched::Mutex lock;
    Node* cache[256] = {};

    // ---- journal (jbd2): metadata changes are committed to the journal
    // before they reach their home blocks, so a crash can't leave half an
    // update behind. One running transaction, committed every few seconds
    // (and on sync), then checkpointed right away: the journal is empty
    // between commits.
    struct Journal {
        bool active = false;
        Node* inode = nullptr;
        u32 maxlen = 0, first = 0, sequence = 0, incompat = 0;
        bool csum_v3 = false, wide = false;
        u32 seed = 0;
        Array<u64> txn; // file system blocks in the running transaction
        u64* set = nullptr;  // open-addressing index of txn (block + 1; 0 = empty)
        usize set_cap = 0;
    } journal;

    bool txn_insert(u64 blk) { // false if already there
        if (journal.txn.n * 2 + 2 > journal.set_cap) {
            usize ncap = journal.set_cap ? journal.set_cap * 2 : 1024;
            u64* nset = static_cast<u64*>(kzalloc(ncap * sizeof(u64)));
            if (!nset)
                return false;
            for (usize i = 0; i < journal.txn.n; i++) {
                usize h = (journal.txn.v[i] * 0x9E3779B97F4A7C15ull) & (ncap - 1);
                while (nset[h]) h = (h + 1) & (ncap - 1);
                nset[h] = journal.txn.v[i] + 1;
            }
            kfree(journal.set);
            journal.set = nset;
            journal.set_cap = ncap;
        }
        usize h = (blk * 0x9E3779B97F4A7C15ull) & (journal.set_cap - 1);
        while (journal.set[h]) {
            if (journal.set[h] == blk + 1)
                return false;
            h = (h + 1) & (journal.set_cap - 1);
        }
        journal.set[h] = blk + 1;
        journal.txn.push(blk);
        return true;
    }

    // Every metadata write goes through here.
    void meta(u64 offset, const void* buf, usize len) {
        block::write(dev, offset, buf, len);
        if (!journal.active)
            return;
        for (u64 b = offset / block_size; b <= (offset + len - 1) / block_size; b++)
            if (txn_insert(b))
                block::hold(dev, b * block_size, block_size, +1);
    }

    u64 journal_phys(u32 lblk);
    long journal_load();
    void journal_write_sb(u32 start);
    void commit_locked();

    u64 block_offset(u64 b) const { return b * block_size; }
    u32 ptrs_per_block() const { return block_size / 4; }
    bool uses_gd_flags() const { return csum || gdt_csum; }

    // ---- superblock ----
    u32 sb32(u32 off) { u32 v = 0; block::read(dev, 1024 + off, &v, 4); return v; }
    void sb_csum() {
        if (!csum)
            return;
        u8 sb[1024];
        block::read(dev, 1024, sb, sizeof(sb));
        u32 c = crc32c(~0u, sb, 0x3FC);
        meta(1024 + 0x3FC, &c, 4);
    }
    void sb_add_free_blocks(i64 delta) {
        u64 v = sb32(12);
        if (incompat & INCOMPAT_64BIT)
            v |= static_cast<u64>(sb32(0x158)) << 32;
        v = static_cast<u64>(static_cast<i64>(v) + delta);
        u32 lo = static_cast<u32>(v), hi = static_cast<u32>(v >> 32);
        meta(1024 + 12, &lo, 4);
        if (incompat & INCOMPAT_64BIT)
            meta(1024 + 0x158, &hi, 4);
        sb_csum();
    }
    void sb_add_free_inodes(i64 delta) {
        u32 v = static_cast<u32>(static_cast<i64>(sb32(16)) + delta);
        meta(1024 + 16, &v, 4);
        sb_csum();
    }

    // ---- group descriptors ----
    u64 gd_offset(u32 g) const { return block_offset(gdt_block) + static_cast<u64>(g) * desc_size; }
    void gd_read(u32 g, u8* d) {
        memset(d, 0, 64);
        block::read(dev, gd_offset(g), d, desc_size);
    }
    void gd_write(u32 g, u8* d) {
        if (csum) {
            u32 c = crc32c(csum_seed, &g, 4);
            c = crc32c(c, d, 0x1E);
            u16 zero = 0;
            c = crc32c(c, &zero, 2);
            if (desc_size > 0x20)
                c = crc32c(c, d + 0x20, desc_size - 0x20);
            put16(d + 0x1E, static_cast<u16>(c));
        } else if (gdt_csum) {
            u16 c = crc16(0xFFFF, uuid, 16);
            c = crc16(c, &g, 4);
            c = crc16(c, d, 0x1E);
            if (desc_size > 0x20)
                c = crc16(c, d + 0x20, desc_size - 0x20);
            put16(d + 0x1E, c);
        }
        meta(gd_offset(g), d, desc_size);
    }
    // Fields split into a low half and (64-bit descriptors) a high half.
    u64 gd_get32x2(const u8* d, u32 lo, u32 hi) const {
        return get32(d + lo) | (desc_size >= 64 ? static_cast<u64>(get32(d + hi)) << 32 : 0);
    }
    u32 gd_get16x2(const u8* d, u32 lo, u32 hi) const {
        return get16(d + lo) | (desc_size >= 64 ? static_cast<u32>(get16(d + hi)) << 16 : 0);
    }
    void gd_set16x2(u8* d, u32 lo, u32 hi, u32 v) const {
        put16(d + lo, static_cast<u16>(v));
        if (desc_size >= 64)
            put16(d + hi, static_cast<u16>(v >> 16));
    }
    u64 gd_block_bitmap(u32 g) { u8 d[64]; gd_read(g, d); return gd_get32x2(d, 0, 0x20); }
    u64 gd_inode_bitmap(u32 g) { u8 d[64]; gd_read(g, d); return gd_get32x2(d, 4, 0x24); }
    u64 gd_inode_table(u32 g) { u8 d[64]; gd_read(g, d); return gd_get32x2(d, 8, 0x28); }

    void set_bitmap_csum(u8* d, bool inode_bitmap, const u8* bitmap) {
        if (!csum)
            return;
        u32 c = crc32c(csum_seed, bitmap, (inode_bitmap ? inodes_per_group : blocks_per_group) / 8);
        put16(d + (inode_bitmap ? 0x1A : 0x18), static_cast<u16>(c));
        if (desc_size >= 0x3C)
            put16(d + (inode_bitmap ? 0x3A : 0x38), static_cast<u16>(c >> 16));
    }

    // ---- group layout ----
    bool group_has_super(u32 g) const {
        if (g <= 1 || !sparse)
            return true;
        static const u32 bases[] = {3, 5, 7};
        for (u32 base : bases) {
            u64 p = base;
            while (p < g)
                p *= base;
            if (p == g)
                return true;
        }
        return false;
    }
    u32 blocks_in_group(u32 g) const {
        return static_cast<u32>(min<u64>(blocks_per_group, blocks_count - first_data_block - static_cast<u64>(g) * blocks_per_group));
    }
    // A BLOCK_UNINIT group's bitmap isn't on disk: it's implied by the layout.
    void init_block_bitmap(u32 g, const u8* d, u8* bm) {
        memset(bm, 0, block_size);
        auto mark = [&](u32 i) { bm[i / 8] |= static_cast<u8>(1 << (i % 8)); };
        if (group_has_super(g))
            for (u32 i = 0; i < 1 + gdt_blocks + reserved_gdt; i++)
                mark(i);
        u64 start = first_data_block + static_cast<u64>(g) * blocks_per_group;
        u32 count = blocks_in_group(g);
        auto mark_abs = [&](u64 b) { if (b >= start && b < start + count) mark(static_cast<u32>(b - start)); };
        mark_abs(gd_get32x2(d, 0, 0x20));
        mark_abs(gd_get32x2(d, 4, 0x24));
        u64 table = gd_get32x2(d, 8, 0x28);
        u64 table_blocks = (static_cast<u64>(inodes_per_group) * inode_size + block_size - 1) / block_size;
        for (u64 i = 0; i < table_blocks; i++)
            mark_abs(table + i);
        for (u32 i = count; i < block_size * 8; i++)
            mark(i);
    }

    // ---- inodes ----
    u64 inode_offset(u32 ino) {
        u32 g = (ino - 1) / inodes_per_group;
        u32 idx = (ino - 1) % inodes_per_group;
        return block_offset(gd_inode_table(g)) + static_cast<u64>(idx) * inode_size;
    }
    void read_inode(u32 ino, RawInode* out) { block::read(dev, inode_offset(ino), out, sizeof(RawInode)); }

    u32 inode_seed(u32 ino, u32 generation) const {
        u32 c = crc32c(csum_seed, &ino, 4);
        return crc32c(c, &generation, 4);
    }

    // Write the first 128 bytes; the rest of a large inode (extra fields) is
    // kept, or zeroed for a fresh inode. Then the inode checksum.
    void write_inode(u32 ino, const RawInode& in, bool fresh = false) {
        u8 buf[1024];
        u64 off = inode_offset(ino);
        if (fresh) {
            memset(buf, 0, inode_size);
            if (inode_size > 128)
                put16(buf + 0x80, extra_isize);
        } else {
            block::read(dev, off, buf, inode_size);
        }
        memcpy(buf, &in, sizeof(RawInode));
        if (csum) {
            u16 zero = 0;
            u32 c = inode_seed(ino, in.generation);
            c = crc32c(c, buf, 0x7C);
            c = crc32c(c, &zero, 2);
            c = crc32c(c, buf + 0x7E, 128 - 0x7E);
            bool has_hi = false;
            if (inode_size > 128) {
                u16 extra = get16(buf + 0x80);
                c = crc32c(c, buf + 128, 2);
                u32 rest = 0x82;
                if (extra >= 4) {
                    c = crc32c(c, &zero, 2);
                    rest = 0x84;
                    has_hi = true;
                }
                c = crc32c(c, buf + rest, inode_size - rest);
            }
            put16(buf + 0x7C, static_cast<u16>(c));
            if (has_hi)
                put16(buf + 0x82, static_cast<u16>(c >> 16));
        }
        meta(off, buf, inode_size);
    }

    // ---- allocation ----
    // A free block, searching from `goal` onwards (keeps files contiguous).
    u64 alloc_block(u64 goal) {
        if (goal < first_data_block || goal >= blocks_count)
            goal = first_data_block;
        u8* bitmap = static_cast<u8*>(kmalloc(block_size));
        if (!bitmap)
            return 0;
        u32 goal_group = static_cast<u32>((goal - first_data_block) / blocks_per_group);
        u32 goal_index = static_cast<u32>((goal - first_data_block) % blocks_per_group);
        u64 result = 0;
        for (u32 n = 0; n <= group_count && !result; n++) {
            u32 g = (goal_group + n) % group_count;
            u8 d[64];
            gd_read(g, d);
            u32 free = gd_get16x2(d, 0x0C, 0x2C);
            if (free == 0)
                continue;
            u16 flags = get16(d + 0x12);
            u64 bm = gd_get32x2(d, 0, 0x20);
            if (uses_gd_flags() && (flags & BG_BLOCK_UNINIT))
                init_block_bitmap(g, d, bitmap);
            else
                block::read(dev, block_offset(bm), bitmap, block_size);
            u32 count = blocks_in_group(g);
            u32 from = (n == 0) ? goal_index : 0;
            u32 found = ~0u;
            for (u32 k = 0; k < count && found == ~0u; k++) {
                u32 i = (from + k) % count;
                if (!(bitmap[i / 8] & (1 << (i % 8))))
                    found = i;
            }
            if (found == ~0u)
                continue;
            bitmap[found / 8] |= static_cast<u8>(1 << (found % 8));
            meta(block_offset(bm), bitmap, block_size);
            gd_set16x2(d, 0x0C, 0x2C, free - 1);
            put16(d + 0x12, static_cast<u16>(flags & ~BG_BLOCK_UNINIT));
            set_bitmap_csum(d, false, bitmap);
            gd_write(g, d);
            sb_add_free_blocks(-1);
            result = first_data_block + static_cast<u64>(g) * blocks_per_group + found;
        }
        if (result) { // fresh blocks read back as zeros
            memset(bitmap, 0, block_size);
            block::write(dev, block_offset(result), bitmap, block_size);
        }
        kfree(bitmap);
        return result;
    }

    // Free `count` blocks starting at `start` (one bitmap update per group).
    void free_range(u64 start, u64 count) {
        u8* bitmap = static_cast<u8*>(kmalloc(block_size));
        if (!bitmap)
            return;
        while (count) {
            if (start < first_data_block || start >= blocks_count)
                break;
            u64 rel = start - first_data_block;
            u32 g = static_cast<u32>(rel / blocks_per_group);
            u32 i = static_cast<u32>(rel % blocks_per_group);
            u32 chunk = static_cast<u32>(min<u64>(count, blocks_per_group - i));
            u8 d[64];
            gd_read(g, d);
            u64 bm = gd_get32x2(d, 0, 0x20);
            block::read(dev, block_offset(bm), bitmap, block_size);
            u32 cleared = 0;
            for (u32 k = i; k < i + chunk; k++) {
                if (bitmap[k / 8] & (1 << (k % 8))) {
                    bitmap[k / 8] &= static_cast<u8>(~(1 << (k % 8)));
                    cleared++;
                }
            }
            if (cleared) {
                meta(block_offset(bm), bitmap, block_size);
                gd_set16x2(d, 0x0C, 0x2C, gd_get16x2(d, 0x0C, 0x2C) + cleared);
                set_bitmap_csum(d, false, bitmap);
                gd_write(g, d);
                sb_add_free_blocks(cleared);
            }
            start += chunk;
            count -= chunk;
        }
        kfree(bitmap);
    }

    u32 alloc_inode(u32 goal_group, bool dir) {
        u8* bitmap = static_cast<u8*>(kmalloc(block_size));
        if (!bitmap)
            return 0;
        u32 result = 0;
        for (u32 n = 0; n < group_count && !result; n++) {
            u32 g = (goal_group + n) % group_count;
            u8 d[64];
            gd_read(g, d);
            u32 free = gd_get16x2(d, 0x0E, 0x2E);
            if (free == 0)
                continue;
            u16 flags = get16(d + 0x12);
            u64 bm = gd_get32x2(d, 4, 0x24);
            if (uses_gd_flags() && (flags & BG_INODE_UNINIT)) {
                memset(bitmap, 0, block_size);
                for (u32 i = inodes_per_group; i < block_size * 8; i++)
                    bitmap[i / 8] |= static_cast<u8>(1 << (i % 8));
            } else {
                block::read(dev, block_offset(bm), bitmap, block_size);
            }
            u32 found = ~0u;
            for (u32 i = 0; i < inodes_per_group; i++) {
                u32 ino = g * inodes_per_group + i + 1;
                if (ino >= first_ino && !(bitmap[i / 8] & (1 << (i % 8)))) {
                    found = i;
                    break;
                }
            }
            if (found == ~0u)
                continue;
            bitmap[found / 8] |= static_cast<u8>(1 << (found % 8));
            meta(block_offset(bm), bitmap, block_size);
            gd_set16x2(d, 0x0E, 0x2E, free - 1);
            if (dir)
                gd_set16x2(d, 0x10, 0x30, gd_get16x2(d, 0x10, 0x30) + 1);
            if (uses_gd_flags()) {
                put16(d + 0x12, static_cast<u16>(flags & ~BG_INODE_UNINIT));
                u32 unused = gd_get16x2(d, 0x1C, 0x32);
                if (found >= inodes_per_group - unused) // the high-water mark of used inodes moves up
                    gd_set16x2(d, 0x1C, 0x32, inodes_per_group - found - 1);
            }
            set_bitmap_csum(d, true, bitmap);
            gd_write(g, d);
            sb_add_free_inodes(-1);
            result = g * inodes_per_group + found + 1;
        }
        kfree(bitmap);
        return result;
    }

    void free_inode(u32 ino, bool dir) {
        u8* bitmap = static_cast<u8*>(kmalloc(block_size));
        if (!bitmap)
            return;
        u32 g = (ino - 1) / inodes_per_group;
        u32 i = (ino - 1) % inodes_per_group;
        u8 d[64];
        gd_read(g, d);
        u64 bm = gd_get32x2(d, 4, 0x24);
        block::read(dev, block_offset(bm), bitmap, block_size);
        if (bitmap[i / 8] & (1 << (i % 8))) {
            bitmap[i / 8] &= static_cast<u8>(~(1 << (i % 8)));
            meta(block_offset(bm), bitmap, block_size);
            gd_set16x2(d, 0x0E, 0x2E, gd_get16x2(d, 0x0E, 0x2E) + 1);
            if (dir)
                gd_set16x2(d, 0x10, 0x30, gd_get16x2(d, 0x10, 0x30) - 1);
            set_bitmap_csum(d, true, bitmap);
            gd_write(g, d);
            sb_add_free_inodes(1);
        }
        kfree(bitmap);
    }

    // ---- checksummed directory and extent blocks ----
    u32 dir_limit() const { return block_size - (csum ? 12 : 0); } // usable bytes per directory block
    void finish_dir_block(u32 seed, u8* buf) {
        if (!csum)
            return;
        u8* t = buf + block_size - 12;
        put32(t, 0);
        put16(t + 4, 12);
        t[6] = 0;
        t[7] = 0xDE; // the checksum "entry"
        put32(t + 8, crc32c(seed, buf, block_size - 12));
    }
    void finish_extent_block(u32 seed, u8* buf) {
        if (!csum)
            return;
        u32 tail = 12 + get16(buf + 4) * 12u;
        if (tail + 4 <= block_size)
            put32(buf + tail, crc32c(seed, buf, tail));
    }

    Node* node(u32 ino, Node* parent);
    void forget(u32 ino);

    // ---- orphans: files unlinked while still open ----
    // They keep their blocks until the last close; meanwhile they sit on the
    // superblock's orphan list (s_last_orphan, chained through each inode's
    // dtime), so a crash in between leaves nothing for fsck to find: the
    // next mount frees them.
    static constexpr u32 SB_LAST_ORPHAN = 0xE8;
    void orphan_add(Node* n);
    void orphan_remove(Node* n);
    void free_orphans(); // at mount
};

class Node : public vfs::Inode {
public:
    Volume* vol;
    u32 number;
    RawInode raw;
    Node* next_cached = nullptr;

    // Page cache for mmap (see vfs::Inode::cache_page): file page -> frame.
    u64* m_cache = nullptr;
    u32 m_cache_pages = 0;

    void drop_cache() {
        if (!m_cache)
            return;
        for (u32 i = 0; i < m_cache_pages; i++)
            if (m_cache[i]) {
                vmm::release_user_page(m_cache[i]); // mappings keep their own references
                __atomic_sub_fetch(&g_cached_pages, 1, __ATOMIC_RELAXED);
            }
        kfree(m_cache);
        m_cache = nullptr;
        m_cache_pages = 0;
    }

    // Under memory pressure (volume lock held): give back cached pages no
    // mapping uses any more (only the cache's reference left). A later
    // fault simply reads them from disk again.
    u64 shrink_cache(u64 wanted) {
        u64 freed = 0;
        for (u32 i = 0; i < m_cache_pages && freed < wanted; i++) {
            u64 p = m_cache[i];
            if (p && __atomic_load_n(&pmm::frame(p)->refcount, __ATOMIC_ACQUIRE) == 1) {
                m_cache[i] = 0;
                vmm::release_user_page(p);
                __atomic_sub_fetch(&g_cached_pages, 1, __ATOMIC_RELAXED);
                freed++;
            }
        }
        return freed;
    }

    long cache_page(u64 offset, u64* phys) override {
        if (!vfs::is_reg(mode) || (offset & 4095))
            return -err::INVAL;
        LockGuard guard(vol->lock);
        if (offset >= size)
            return -err::FAULT;
        u32 index = static_cast<u32>(offset / 4096);
        if (index >= m_cache_pages) {
            u32 want = static_cast<u32>((size + 4095) / 4096);
            if (want <= index)
                return -err::FAULT;
            auto* grown = static_cast<u64*>(kzalloc(want * sizeof(u64)));
            if (!grown)
                return -err::NOMEM;
            if (m_cache)
                memcpy(grown, m_cache, m_cache_pages * sizeof(u64));
            kfree(m_cache);
            m_cache = grown;
            m_cache_pages = want;
        }
        if (!m_cache[index]) {
            u64 p = pmm::alloc_page(true);
            if (!p)
                return -err::NOMEM;
            usize n = min<u64>(4096, size - offset);
            if (read_locked(offset, mm::phys_to_virt<u8>(p), n) < 0) {
                pmm::free_page(p);
                return -err::IO;
            }
            m_cache[index] = p; // the cache's own reference (refcount 1)
            __atomic_add_fetch(&g_cached_pages, 1, __ATOMIC_RELAXED);
        }
        *phys = m_cache[index];
        __atomic_add_fetch(&pmm::frame(*phys)->refcount, 1, __ATOMIC_ACQ_REL); // the caller's (see vfs::Inode)
        return 0;
    }

    u32 opens = 0;         // Files open on it
    u32 maps = 0;          // memory mappings of it (they outlive the fd: open, mmap, close)
    bool orphaned = false; // unlinked while open or mapped: freed when the last of both goes

    bool in_use() const {
        return __atomic_load_n(&opens, __ATOMIC_ACQUIRE) || __atomic_load_n(&maps, __ATOMIC_ACQUIRE);
    }
    void last_user_gone() {
        LockGuard guard(vol->lock);
        if (orphaned && !in_use()) {
            vol->orphan_remove(this);
            orphaned = false;
            release_inode(this);
        }
    }
    void on_open(vfs::File*) override { __atomic_add_fetch(&opens, 1, __ATOMIC_ACQ_REL); }
    void on_release(vfs::File*) override {
        if (__atomic_sub_fetch(&opens, 1, __ATOMIC_ACQ_REL) == 0)
            last_user_gone();
    }
    // A running program whose file is deleted or replaced (apt upgrade,
    // rename over it) keeps its mapped pages: the file is freed only after
    // the last mapping goes. Freeing it at unlink left the program faulting
    // in zero pages from a released inode.
    void mmap_ref(int delta) override {
        if (__atomic_add_fetch(&maps, static_cast<u32>(delta), __ATOMIC_ACQ_REL) == 0)
            last_user_gone();
    }

    Node(Volume* v, u32 inode_number) : vol(v), number(inode_number) {
        v->read_inode(inode_number, &raw);
        ino = inode_number;
        dev = v->fs_dev;
        refresh();
    }

    void refresh() {
        mode = raw.mode;
        uid = raw.uid | (static_cast<u32>(get16(raw.osd2 + 4)) << 16);
        gid = raw.gid | (static_cast<u32>(get16(raw.osd2 + 6)) << 16);
        nlink = raw.links;
        size = raw.size_lo | (vfs::is_reg(raw.mode) ? static_cast<u64>(raw.size_hi) << 32 : 0);
        atime = vfs::Timespec{raw.atime, 0};
        mtime = vfs::Timespec{raw.mtime, 0};
        ctime = vfs::Timespec{raw.ctime, 0};
    }

    void save() {
        raw.mode = static_cast<u16>(mode);
        raw.uid = static_cast<u16>(uid);
        raw.gid = static_cast<u16>(gid);
        put16(raw.osd2 + 4, static_cast<u16>(uid >> 16)); // l_i_uid_high
        put16(raw.osd2 + 6, static_cast<u16>(gid >> 16)); // l_i_gid_high
        raw.links = static_cast<u16>(nlink);
        raw.size_lo = static_cast<u32>(size);
        if (vfs::is_reg(mode))
            raw.size_hi = static_cast<u32>(size >> 32);
        raw.mtime = static_cast<u32>(mtime.sec);
        raw.ctime = static_cast<u32>(ctime.sec);
        raw.atime = static_cast<u32>(atime.sec);
        vol->write_inode(number, raw);
    }

    u32 seed() const { return vol->inode_seed(number, raw.generation); }
    bool uses_extents() const { return raw.flags & FLAG_EXTENTS; }
    u32 goal_group() const { return (number - 1) / vol->inodes_per_group; }
    u64 goal_block() const { return vol->first_data_block + static_cast<u64>(goal_group()) * vol->blocks_per_group; }
    void add_blocks(i64 count) {
        raw.blocks_lo = static_cast<u32>(static_cast<i64>(raw.blocks_lo) + count * (vol->block_size / 512));
    }

    // ---- extent trees ----

    // Physical block for file block `n` via the extent tree (0 = hole).
    u64 extent_block(u64 n) {
        u8 node_buf[4096];
        const u8* hdr = reinterpret_cast<const u8*>(raw.block);
        for (int level = 0; level < 8; level++) {
            if (get16(hdr) != EXT_MAGIC)
                break;
            u16 entries = get16(hdr + 2), depth = get16(hdr + 6);
            const u8* e = hdr + 12;
            if (depth == 0) {
                for (u16 i = 0; i < entries; i++, e += 12) {
                    u32 first = get32(e);
                    u32 len = get16(e + 4);
                    bool uninit = len > EXT_INIT_MAX_LEN;
                    if (uninit) len -= EXT_INIT_MAX_LEN;
                    if (n >= first && n < first + len)
                        return uninit ? 0 : ((static_cast<u64>(get16(e + 6)) << 32) | get32(e + 8)) + (n - first);
                }
                break;
            }
            // Index node: follow the last entry whose first block <= n.
            u64 child = 0;
            for (u16 i = 0; i < entries; i++, e += 12) {
                if (get32(e) > n)
                    break;
                child = (static_cast<u64>(get16(e + 8)) << 32) | get32(e + 4);
            }
            if (!child || vol->block_size > sizeof(node_buf))
                break;
            block::read(vol->dev, vol->block_offset(child), node_buf, vol->block_size);
            hdr = node_buf;
        }
        return 0;
    }

    // Every extent, in order, plus the tree's own (non-inode) blocks.
    bool flatten(const u8* hdr, Array<Extent>& out, Array<u64>* tree_blocks, int level = 0) {
        if (level > 8 || get16(hdr) != EXT_MAGIC)
            return level == 0 && get16(hdr) == 0; // an empty i_block is an empty tree
        u16 entries = get16(hdr + 2), depth = get16(hdr + 6);
        const u8* e = hdr + 12;
        if (depth == 0) {
            for (u16 i = 0; i < entries; i++, e += 12) {
                u32 len = get16(e + 4);
                bool uninit = len > EXT_INIT_MAX_LEN;
                if (uninit) len -= EXT_INIT_MAX_LEN;
                if (!out.push(Extent{get32(e), len, (static_cast<u64>(get16(e + 6)) << 32) | get32(e + 8), uninit}))
                    return false;
            }
            return true;
        }
        u8* buf = static_cast<u8*>(kmalloc(vol->block_size));
        if (!buf)
            return false;
        bool ok = true;
        for (u16 i = 0; i < entries && ok; i++, e += 12) {
            u64 child = (static_cast<u64>(get16(e + 8)) << 32) | get32(e + 4);
            if (tree_blocks && !tree_blocks->push(child)) {
                ok = false;
                break;
            }
            block::read(vol->dev, vol->block_offset(child), buf, vol->block_size);
            ok = flatten(buf, out, tree_blocks, level + 1);
        }
        kfree(buf);
        return ok;
    }

    static void put_extent(u8* e, const Extent& x) {
        put32(e, x.lblk);
        put16(e + 4, static_cast<u16>(x.uninit ? x.len + EXT_INIT_MAX_LEN : x.len));
        put16(e + 6, static_cast<u16>(x.pblk >> 32));
        put32(e + 8, static_cast<u32>(x.pblk));
    }
    static void put_header(u8* h, u16 entries, u16 max, u16 depth) {
        put16(h, EXT_MAGIC);
        put16(h + 2, entries);
        put16(h + 4, max);
        put16(h + 6, depth);
        put32(h + 8, 0);
    }

    // Write a fresh tree holding `ext` (the old tree's blocks are already
    // freed): up to 4 extents live in the inode, more go into leaf blocks
    // under index levels.
    long write_tree(const Extent* ext, usize count) {
        u8* root = reinterpret_cast<u8*>(raw.block);
        memset(root, 0, 60);
        if (count <= 4) {
            put_header(root, static_cast<u16>(count), 4, 0);
            for (usize i = 0; i < count; i++)
                put_extent(root + 12 + i * 12, ext[i]);
            save();
            return 0;
        }
        u16 per_block = static_cast<u16>((vol->block_size - 12) / 12);
        struct Key { u32 lblk; u64 blk; };
        Array<Key> level;
        u8* buf = static_cast<u8*>(kmalloc(vol->block_size));
        if (!buf)
            return -err::NOMEM;
        u64 goal = ext[0].pblk;
        for (usize i = 0; i < count; i += per_block) { // leaves
            usize n = min<usize>(per_block, count - i);
            u64 b = vol->alloc_block(goal);
            if (!b) { kfree(buf); return -err::NOSPC; }
            add_blocks(1);
            memset(buf, 0, vol->block_size);
            put_header(buf, static_cast<u16>(n), per_block, 0);
            for (usize k = 0; k < n; k++)
                put_extent(buf + 12 + k * 12, ext[i + k]);
            vol->finish_extent_block(seed(), buf);
            vol->meta(vol->block_offset(b), buf, vol->block_size);
            level.push(Key{ext[i].lblk, b});
        }
        u16 depth = 1;
        while (level.n > 4) { // index levels
            Array<Key> up;
            for (usize i = 0; i < level.n; i += per_block) {
                usize n = min<usize>(per_block, level.n - i);
                u64 b = vol->alloc_block(goal);
                if (!b) { kfree(buf); return -err::NOSPC; }
                add_blocks(1);
                memset(buf, 0, vol->block_size);
                put_header(buf, static_cast<u16>(n), per_block, depth);
                for (usize k = 0; k < n; k++) {
                    u8* e = buf + 12 + k * 12;
                    put32(e, level.v[i + k].lblk);
                    put32(e + 4, static_cast<u32>(level.v[i + k].blk));
                    put16(e + 8, static_cast<u16>(level.v[i + k].blk >> 32));
                }
                vol->finish_extent_block(seed(), buf);
                vol->meta(vol->block_offset(b), buf, vol->block_size);
                up.push(Key{level.v[i].lblk, b});
            }
            kfree(level.v);
            level.v = up.v;
            level.n = up.n;
            level.cap = up.cap;
            up.v = nullptr;
            depth++;
        }
        put_header(root, static_cast<u16>(level.n), 4, depth);
        for (usize k = 0; k < level.n; k++) {
            u8* e = root + 12 + k * 12;
            put32(e, level.v[k].lblk);
            put32(e + 4, static_cast<u32>(level.v[k].blk));
            put16(e + 8, static_cast<u16>(level.v[k].blk >> 32));
        }
        kfree(buf);
        save();
        return 0;
    }

    void free_tree_blocks(Array<u64>& blocks) {
        for (usize i = 0; i < blocks.n; i++) {
            vol->free_range(blocks.v[i], 1);
            add_blocks(-1);
        }
    }

    // The leaf that holds (or would hold) file block `n`: copied into `buf`;
    // *where = its disk block, or 0 for the inode itself.
    bool leaf_for(u64 n, u8* buf, u64* where) {
        memcpy(buf, raw.block, 60);
        *where = 0;
        for (int level = 0; level < 8; level++) {
            if (get16(buf) != EXT_MAGIC)
                return false;
            u16 entries = get16(buf + 2), depth = get16(buf + 6);
            if (depth == 0)
                return true;
            if (!entries)
                return false;
            const u8* e = buf + 12;
            u64 child = (static_cast<u64>(get16(e + 8)) << 32) | get32(e + 4);
            for (u16 i = 1; i < entries; i++) {
                e += 12;
                if (get32(e) > n)
                    break;
                child = (static_cast<u64>(get16(e + 8)) << 32) | get32(e + 4);
            }
            block::read(vol->dev, vol->block_offset(child), buf, vol->block_size);
            *where = child;
        }
        return false;
    }

    void write_leaf(u64 where, u8* buf) {
        if (!where) {
            memcpy(raw.block, buf, 60);
            save();
            return;
        }
        vol->finish_extent_block(seed(), buf);
        vol->meta(vol->block_offset(where), buf, vol->block_size);
    }

    // Map file block `n`, allocating a block if it's a hole.
    u64 extent_alloc(u64 n) {
        if (u64 b = extent_block(n))
            return b;
        u64 goal = n ? extent_block(n - 1) : 0;
        goal = goal ? goal + 1 : goal_block();

        // Inside an unwritten (fallocated) extent? Zero it and mark it written.
        // Otherwise allocate a block and try the cheap updates first.
        u8* buf = static_cast<u8*>(kmalloc(vol->block_size));
        if (!buf)
            return 0;
        u64 where;
        u64 nb = 0;
        if (leaf_for(n, buf, &where)) {
            u16 entries = get16(buf + 2), max = get16(buf + 4);
            bool covered_uninit = false;
            for (u16 i = 0; i < entries; i++) {
                u8* e = buf + 12 + i * 12;
                u32 len = get16(e + 4);
                if (len > EXT_INIT_MAX_LEN && n >= get32(e) && n < get32(e) + len - EXT_INIT_MAX_LEN)
                    covered_uninit = true;
            }
            if (!covered_uninit) {
                nb = vol->alloc_block(goal);
                if (!nb) { kfree(buf); return 0; }
                add_blocks(1);
                // 1) Extend the extent that ends right before n, if the block is adjacent.
                for (u16 i = 0; i < entries; i++) {
                    u8* e = buf + 12 + i * 12;
                    u32 len = get16(e + 4);
                    u64 p = (static_cast<u64>(get16(e + 6)) << 32) | get32(e + 8);
                    if (len < EXT_INIT_MAX_LEN && get32(e) + len == n && p + len == nb) {
                        put16(e + 4, static_cast<u16>(len + 1));
                        write_leaf(where, buf);
                        kfree(buf);
                        return nb;
                    }
                }
                // 2) Append a new extent at the end of this leaf (still sorted:
                //    leaf_for() chose the last leaf whose first block is <= n).
                bool after_all = true;
                if (entries) {
                    u8* last = buf + 12 + (entries - 1) * 12;
                    u32 llen = get16(last + 4);
                    if (llen > EXT_INIT_MAX_LEN) llen -= EXT_INIT_MAX_LEN;
                    after_all = get32(last) + llen <= n;
                }
                if (entries < max && after_all) {
                    put_extent(buf + 12 + entries * 12, Extent{static_cast<u32>(n), 1, nb, false});
                    put16(buf + 2, static_cast<u16>(entries + 1));
                    write_leaf(where, buf);
                    kfree(buf);
                    return nb;
                }
            }
        }
        kfree(buf);

        // 3) General case: rebuild the tree with the new extent in place.
        Array<Extent> ext;
        Array<u64> tree;
        if (!flatten(reinterpret_cast<const u8*>(raw.block), ext, &tree))
            return 0;
        for (usize i = 0; i < ext.n; i++) {
            Extent& x = ext.v[i];
            if (x.uninit && n >= x.lblk && n < x.lblk + x.len) {
                u8* zeros = static_cast<u8*>(kzalloc(vol->block_size));
                if (!zeros)
                    return 0;
                for (u32 k = 0; k < x.len; k++)
                    block::write(vol->dev, vol->block_offset(x.pblk + k), zeros, vol->block_size);
                kfree(zeros);
                x.uninit = false;
                nb = x.pblk + (n - x.lblk);
                free_tree_blocks(tree);
                return write_tree(ext.v, ext.n) == 0 ? nb : 0;
            }
        }
        if (!nb) {
            nb = vol->alloc_block(goal);
            if (!nb)
                return 0;
            add_blocks(1);
        }
        usize at = 0;
        while (at < ext.n && ext.v[at].lblk < n)
            at++;
        if (!ext.insert(at, Extent{static_cast<u32>(n), 1, nb, false}))
            return 0;
        free_tree_blocks(tree);
        return write_tree(ext.v, ext.n) == 0 ? nb : 0;
    }

    // Free every block from file block `from` on (extent-mapped files).
    void free_extents_from(u64 from) {
        Array<Extent> ext, keep;
        Array<u64> tree;
        if (!flatten(reinterpret_cast<const u8*>(raw.block), ext, &tree))
            return;
        free_tree_blocks(tree);
        for (usize i = 0; i < ext.n; i++) {
            Extent x = ext.v[i];
            if (x.lblk >= from) {
                vol->free_range(x.pblk, x.len);
                add_blocks(-static_cast<i64>(x.len));
            } else if (x.lblk + x.len > from) {
                u32 cut = static_cast<u32>(x.lblk + x.len - from);
                vol->free_range(x.pblk + (x.len - cut), cut);
                add_blocks(-static_cast<i64>(cut));
                x.len -= cut;
                keep.push(x);
            } else {
                keep.push(x);
            }
        }
        write_tree(keep.v, keep.n);
    }

    // ---- block-mapped (ext2/ext3) files: direct, single, double, triple indirect ----
    u64 mapped_block(u64 n, bool create) {
        u32 ppb = vol->ptrs_per_block();
        u64 goal = goal_block();
        if (n < 12) {
            if (!raw.block[n] && create) {
                raw.block[n] = static_cast<u32>(vol->alloc_block(goal));
                if (raw.block[n]) add_blocks(1);
                save();
            }
            return raw.block[n];
        }
        n -= 12;
        int levels;
        u32* slot;
        if (n < ppb) { levels = 1; slot = &raw.block[12]; }
        else if ((n -= ppb) < static_cast<u64>(ppb) * ppb) { levels = 2; slot = &raw.block[13]; }
        else { n -= static_cast<u64>(ppb) * ppb; levels = 3; slot = &raw.block[14]; }

        if (!*slot) {
            if (!create) return 0;
            *slot = static_cast<u32>(vol->alloc_block(goal));
            if (!*slot) return 0;
            add_blocks(1);
            save();
        }
        u64 table = *slot;
        for (int level = levels; level >= 1; level--) {
            u64 span = 1;
            for (int k = 1; k < level; k++) span *= ppb;
            u32 index = static_cast<u32>(n / span);
            n %= span;
            u32 entry;
            u64 off = vol->block_offset(table) + static_cast<u64>(index) * 4;
            block::read(vol->dev, off, &entry, 4);
            if (!entry) {
                if (!create) return 0;
                entry = static_cast<u32>(vol->alloc_block(table + 1));
                if (!entry) return 0;
                vol->meta(off, &entry, 4);
                add_blocks(1);
                save();
            }
            table = entry;
        }
        return table;
    }

    u64 file_block(u64 n, bool create) {
        if (uses_extents())
            return create ? extent_alloc(n) : extent_block(n);
        return mapped_block(n, create);
    }

    void free_blocks_from(u64 from) {
        if (uses_extents()) {
            free_extents_from(from);
            return;
        }
        u32 ppb = vol->ptrs_per_block();
        for (u64 n = from; n < 12; n++) {
            if (raw.block[n]) { vol->free_range(raw.block[n], 1); raw.block[n] = 0; add_blocks(-1); }
        }
        // Indirect trees: free whole trees only when truncating to their start.
        auto free_tree = [&](auto& self, u64 blk, int level) -> void {
            if (!blk) return;
            if (level > 0) {
                u32* entries = static_cast<u32*>(kmalloc(vol->block_size));
                if (entries) {
                    block::read(vol->dev, vol->block_offset(blk), entries, vol->block_size);
                    for (u32 i = 0; i < ppb; i++) self(self, entries[i], level - 1);
                    kfree(entries);
                }
            }
            vol->free_range(blk, 1);
            add_blocks(-1);
        };
        u64 starts[3] = {12, 12 + static_cast<u64>(ppb), 12 + static_cast<u64>(ppb) + static_cast<u64>(ppb) * ppb};
        for (int i = 0; i < 3; i++) {
            if (from <= starts[i] && raw.block[12 + i]) {
                free_tree(free_tree, raw.block[12 + i], i + 1);
                raw.block[12 + i] = 0;
            }
        }
        save();
    }

    // ---- data ----

    isize read(u64 offset, void* buf, usize count) override {
        if (vfs::is_dir(mode))
            return -err::ISDIR;
        LockGuard guard(vol->lock);
        return read_locked(offset, buf, count);
    }

    isize read_locked(u64 offset, void* buf, usize count) {
        if (raw.flags & FLAG_INLINE)
            return -err::OPNOTSUPP; // inline data: not yet
        if (offset >= size)
            return 0;
        count = min<u64>(count, size - offset);
        auto* out = static_cast<u8*>(buf);
        usize done = 0;
        while (done < count) {
            u64 n = (offset + done) / vol->block_size;
            u32 in = static_cast<u32>((offset + done) % vol->block_size);
            usize chunk = min<usize>(count - done, vol->block_size - in);
            u64 b = file_block(n, false);
            if (b) {
                if (block::read(vol->dev, vol->block_offset(b) + in, out + done, chunk) < 0)
                    return done ? static_cast<isize>(done) : -err::IO;
            } else {
                memset(out + done, 0, chunk); // hole
            }
            done += chunk;
        }
        return static_cast<isize>(done);
    }

    bool writable() const { return !vol->read_only && !(raw.flags & FLAG_INLINE); }

    isize write(u64 offset, const void* buf, usize count) override {
        if (vfs::is_dir(mode))
            return -err::ISDIR;
        if (!writable())
            return -err::ROFS;
        LockGuard guard(vol->lock);
        return write_locked(offset, buf, count);
    }

    isize write_locked(u64 offset, const void* buf, usize count) {
        drop_cache(); // new mappings must see the new data
        auto* in = static_cast<const u8*>(buf);
        usize done = 0;
        while (done < count) {
            u64 n = (offset + done) / vol->block_size;
            u32 within = static_cast<u32>((offset + done) % vol->block_size);
            usize chunk = min<usize>(count - done, vol->block_size - within);
            u64 b = file_block(n, true);
            if (!b)
                break; // disk full
            block::write(vol->dev, vol->block_offset(b) + within, in + done, chunk);
            done += chunk;
        }
        if (offset + done > size)
            size = offset + done;
        mtime = ctime = vfs::now();
        save();
        if (done == 0 && count)
            return -err::NOSPC;
        return static_cast<isize>(done);
    }

    long truncate(u64 new_size) override {
        if (vfs::is_dir(mode))
            return -err::ISDIR;
        if (!writable())
            return -err::ROFS;
        LockGuard guard(vol->lock);
        drop_cache();
        if (new_size < size) {
            u64 keep_blocks = (new_size + vol->block_size - 1) / vol->block_size;
            free_blocks_from(keep_blocks);
            // Zero the tail of the last kept block so a later extension reads zeros.
            u32 within = static_cast<u32>(new_size % vol->block_size);
            if (within) {
                u64 b = file_block(new_size / vol->block_size, false);
                if (b) {
                    usize n = vol->block_size - within;
                    u8* zeros = static_cast<u8*>(kzalloc(n));
                    if (zeros) {
                        block::write(vol->dev, vol->block_offset(b) + within, zeros, n);
                        kfree(zeros);
                    }
                }
            }
        }
        size = new_size; // growing: the new range is a hole (reads as zeros)
        mtime = ctime = vfs::now();
        save();
        return 0;
    }

    long statfs(FsStats* out) override {
        LockGuard guard(vol->lock);
        u64 free_blocks = vol->sb32(12);
        if (vol->incompat & INCOMPAT_64BIT)
            free_blocks |= static_cast<u64>(vol->sb32(0x158)) << 32;
        *out = FsStats{0xEF53, vol->block_size, vol->blocks_count, free_blocks,
                       static_cast<u64>(vol->sb32(0)), static_cast<u64>(vol->sb32(16))};
        return 0;
    }

    long set_owner(u32 new_mode, u32 new_uid, u32 new_gid) override {
        if (vol->read_only)
            return -err::ROFS;
        LockGuard guard(vol->lock);
        mode = (mode & vfs::S_IFMT) | (new_mode & 07777);
        uid = new_uid;
        gid = new_gid;
        ctime = vfs::now();
        save();
        return 0;
    }

    long set_times(vfs::Timespec a, vfs::Timespec m) override {
        if (vol->read_only)
            return -err::ROFS;
        LockGuard guard(vol->lock);
        atime = a;
        mtime = m;
        ctime = vfs::now();
        save();
        return 0;
    }

    isize readlink(char* buf, usize bufsize) override {
        if (!vfs::is_lnk(mode))
            return -err::INVAL;
        LockGuard guard(vol->lock);
        usize n = min<u64>(size, bufsize);
        if (size < 60 && raw.blocks_lo == 0 && !uses_extents()) { // fast symlink: target lives in i_block
            memcpy(buf, raw.block, n);
            return static_cast<isize>(n);
        }
        return read_locked(0, buf, n);
    }

    // ---- directories ----

    // fn(name, len, ino, type, entry_offset, prev_entry_offset) -> stop?
    template <typename Fn>
    bool iterate(Fn fn) {
        u8* blockbuf = static_cast<u8*>(kmalloc(vol->block_size));
        if (!blockbuf)
            return false;
        bool stopped = false;
        u64 blocks = (size + vol->block_size - 1) / vol->block_size;
        for (u64 bi = 0; bi < blocks && !stopped; bi++) {
            u64 b = file_block(bi, false);
            if (!b)
                continue;
            block::read(vol->dev, vol->block_offset(b), blockbuf, vol->block_size);
            u32 off = 0;
            u64 prev = 0;
            while (off + 8 <= vol->block_size) {
                u32 ino = get32(blockbuf + off);
                u16 rec_len = get16(blockbuf + off + 4);
                u8 name_len = blockbuf[off + 6];
                u8 type = vol->has_filetype ? blockbuf[off + 7] : FT_UNKNOWN;
                if (rec_len < 8 || off + rec_len > vol->block_size)
                    break; // corrupt: stop reading this block
                u64 here = vol->block_offset(b) + off;
                if (ino && name_len && fn(reinterpret_cast<const char*>(blockbuf + off + 8), name_len, ino, type, here, prev)) {
                    stopped = true;
                    break;
                }
                prev = here;
                off += rec_len;
            }
        }
        kfree(blockbuf);
        return stopped;
    }

    vfs::Inode* lookup(const char* name, usize len) override {
        if (!vfs::is_dir(mode))
            return nullptr;
        LockGuard guard(vol->lock);
        return find_locked(name, len);
    }

    Node* find_locked(const char* name, usize len) {
        u32 found = 0;
        iterate([&](const char* n, u8 nl, u32 ino, u8, u64, u64) {
            if (nl == len && memcmp(n, name, len) == 0) {
                found = ino;
                return true;
            }
            return false;
        });
        return found ? vol->node(found, this) : nullptr;
    }

    long readdir(u64 index, vfs::DirEntry* out) override {
        if (!vfs::is_dir(mode))
            return -err::NOTDIR;
        LockGuard guard(vol->lock);
        u64 i = 0;
        bool found = iterate([&](const char* n, u8 nl, u32 ino, u8 type, u64, u64) {
            if (i++ != index)
                return false;
            out->ino = ino;
            out->type = dtype_for(type);
            memcpy(out->name, n, nl);
            out->name[nl] = 0;
            return true;
        });
        return found ? 1 : 0;
    }

    static u16 entry_size(u32 name_len) { return static_cast<u16>((8 + name_len + 3) & ~3u); }

    // Read-modify-write the directory block containing byte `disk_offset`.
    template <typename Fn>
    void edit_dir_block(u64 disk_offset, Fn fn) {
        u8* buf = static_cast<u8*>(kmalloc(vol->block_size));
        if (!buf)
            return;
        u64 start = disk_offset - disk_offset % vol->block_size;
        block::read(vol->dev, start, buf, vol->block_size);
        fn(buf, static_cast<u32>(disk_offset - start));
        vol->finish_dir_block(seed(), buf);
        vol->meta(start, buf, vol->block_size);
        kfree(buf);
    }

    // We don't maintain htree indexes: before changing an indexed directory,
    // turn it back into a plain linear one (which ext4 reads just as well).
    // With checksums, the index blocks are rewritten as checksummed leaves.
    void ensure_linear() {
        if (!(raw.flags & FLAG_INDEX))
            return;
        if (vol->csum) {
            u8* buf = static_cast<u8*>(kmalloc(vol->block_size));
            if (buf) {
                u64 blocks = (size + vol->block_size - 1) / vol->block_size;
                for (u64 bi = 0; bi < blocks; bi++) {
                    u64 b = file_block(bi, false);
                    if (!b)
                        continue;
                    block::read(vol->dev, vol->block_offset(b), buf, vol->block_size);
                    bool rewrite = false;
                    if (bi == 0) { // the index root: "." then ".." spanning the rest
                        u16 dot_len = get16(buf + 4);
                        put16(buf + dot_len + 4, static_cast<u16>(vol->dir_limit() - dot_len));
                        rewrite = true;
                    } else if (get32(buf) == 0 && get16(buf + 4) == vol->block_size) { // an index node
                        memset(buf, 0, vol->block_size);
                        put16(buf + 4, static_cast<u16>(vol->dir_limit()));
                        rewrite = true;
                    }
                    if (rewrite) {
                        vol->finish_dir_block(seed(), buf);
                        vol->meta(vol->block_offset(b), buf, vol->block_size);
                    }
                }
                kfree(buf);
            }
        }
        raw.flags &= ~FLAG_INDEX;
        save();
    }

    // Add a directory entry.
    long add_entry(const char* name, usize len, u32 ino, u8 type) {
        ensure_linear();
        u16 needed = entry_size(static_cast<u32>(len));
        u32 limit = vol->dir_limit();
        u8* blockbuf = static_cast<u8*>(kmalloc(vol->block_size));
        if (!blockbuf)
            return -err::NOMEM;
        u64 blocks = (size + vol->block_size - 1) / vol->block_size;
        for (u64 bi = 0; bi < blocks; bi++) {
            u64 b = file_block(bi, false);
            if (!b)
                continue;
            block::read(vol->dev, vol->block_offset(b), blockbuf, vol->block_size);
            u32 off = 0;
            while (off + 8 <= limit) {
                u32 e_ino = get32(blockbuf + off);
                u16 rec_len = get16(blockbuf + off + 4);
                if (rec_len < 8 || off + rec_len > limit)
                    break;
                u16 used = e_ino ? entry_size(blockbuf[off + 6]) : 0;
                if (rec_len - used >= needed) {
                    u32 at = off + used;
                    if (e_ino) // split: shrink the existing entry, ours takes the rest
                        put16(blockbuf + off + 4, used);
                    put32(blockbuf + at, ino);
                    put16(blockbuf + at + 4, static_cast<u16>(rec_len - used));
                    blockbuf[at + 6] = static_cast<u8>(len);
                    blockbuf[at + 7] = vol->has_filetype ? type : 0;
                    memcpy(blockbuf + at + 8, name, len);
                    vol->finish_dir_block(seed(), blockbuf);
                    vol->meta(vol->block_offset(b), blockbuf, vol->block_size);
                    kfree(blockbuf);
                    touch_dir();
                    return 0;
                }
                off += rec_len;
            }
        }
        // No room: append a block holding just this entry.
        u64 b = file_block(blocks, true);
        if (!b) {
            kfree(blockbuf);
            return -err::NOSPC;
        }
        memset(blockbuf, 0, vol->block_size);
        put32(blockbuf, ino);
        put16(blockbuf + 4, static_cast<u16>(limit));
        blockbuf[6] = static_cast<u8>(len);
        blockbuf[7] = vol->has_filetype ? type : 0;
        memcpy(blockbuf + 8, name, len);
        vol->finish_dir_block(seed(), blockbuf);
        vol->meta(vol->block_offset(b), blockbuf, vol->block_size);
        kfree(blockbuf);
        size += vol->block_size;
        touch_dir();
        return 0;
    }

    void touch_dir() {
        mtime = ctime = vfs::now();
        save();
    }

    // Remove the entry for `name`; returns its inode number (0 if absent).
    u32 remove_entry(const char* name, usize len) {
        ensure_linear();
        u32 victim = 0;
        u64 at = 0, prev = 0;
        iterate([&](const char* n, u8 nl, u32 ino, u8, u64 here, u64 before) {
            if (nl == len && memcmp(n, name, len) == 0) {
                victim = ino;
                at = here;
                prev = before;
                return true;
            }
            return false;
        });
        if (!victim)
            return 0;
        u64 block_start = at - at % vol->block_size;
        bool merge = prev >= block_start && prev && prev != at;
        edit_dir_block(at, [&](u8* buf, u32 off) {
            if (merge) { // merge into the previous entry of the same block
                u32 poff = static_cast<u32>(prev - block_start);
                put16(buf + poff + 4, static_cast<u16>(get16(buf + poff + 4) + get16(buf + off + 4)));
            } else {
                put32(buf + off, 0); // first entry in its block: just mark it unused
            }
        });
        touch_dir();
        return victim;
    }

    bool dir_empty() {
        return !iterate([](const char* n, u8 nl, u32, u8, u64, u64) {
            return !(nl == 1 && n[0] == '.') && !(nl == 2 && n[0] == '.' && n[1] == '.');
        });
    }

    long create(const char* name, usize len, u32 new_mode, const char* target, vfs::Inode** out) override {
        if (!vfs::is_dir(mode))
            return -err::NOTDIR;
        if (!writable())
            return -err::ROFS;
        if (len > 255)
            return -err::NAMETOOLONG;
        LockGuard guard(vol->lock);
        if (find_locked(name, len))
            return -err::EXIST;
        u32 type = new_mode & vfs::S_IFMT;
        if (type == 0)
            type = vfs::S_IFREG;
        bool dir = type == vfs::S_IFDIR;
        u32 ino = vol->alloc_inode(goal_group(), dir);
        if (!ino)
            return -err::NOSPC;

        RawInode r{};
        u32 t = now_seconds();
        r.mode = static_cast<u16>(type | (new_mode & 07777));
        r.links = dir ? 2 : 1;
        r.atime = r.ctime = r.mtime = t;
        r.generation = static_cast<u32>(random::next_u64());
        bool fast_symlink = type == vfs::S_IFLNK && target && strlen(target) < 60;
        if (vol->extents && (type == vfs::S_IFREG || dir || (type == vfs::S_IFLNK && !fast_symlink))) {
            r.flags |= FLAG_EXTENTS;
            put_header(reinterpret_cast<u8*>(r.block), 0, 4, 0);
        }
        vol->write_inode(ino, r, true);
        vol->forget(ino);
        Node* n = vol->node(ino, this);

        if (dir) {
            // "." and ".." in a fresh block.
            u64 b = n->file_block(0, true);
            if (!b)
                return -err::NOSPC;
            u8* blockbuf = static_cast<u8*>(kzalloc(vol->block_size));
            if (!blockbuf)
                return -err::NOMEM;
            put32(blockbuf, ino);
            put16(blockbuf + 4, 12);
            blockbuf[6] = 1;
            blockbuf[7] = vol->has_filetype ? FT_DIR : 0;
            blockbuf[8] = '.';
            put32(blockbuf + 12, number);
            put16(blockbuf + 16, static_cast<u16>(vol->dir_limit() - 12));
            blockbuf[18] = 2;
            blockbuf[19] = vol->has_filetype ? FT_DIR : 0;
            blockbuf[20] = '.';
            blockbuf[21] = '.';
            vol->finish_dir_block(n->seed(), blockbuf);
            vol->meta(vol->block_offset(b), blockbuf, vol->block_size);
            kfree(blockbuf);
            n->size = vol->block_size;
            n->save();
            nlink++; // the new ".." refers to us
            save();
        } else if (type == vfs::S_IFLNK && target) {
            usize tl = strlen(target);
            if (fast_symlink) {
                memcpy(n->raw.block, target, tl);
                n->size = tl;
                n->save();
            } else {
                n->write_locked(0, target, tl);
            }
        }
        if (long r2 = add_entry(name, len, ino, type_for_mode(n->mode)); r2 < 0)
            return r2;
        if (out)
            *out = n;
        return 0;
    }

    void release_inode(Node* victim) {
        bool dir = vfs::is_dir(victim->mode);
        bool fast_symlink = vfs::is_lnk(victim->mode) && !victim->uses_extents() && victim->raw.blocks_lo == 0;
        if (!fast_symlink)
            victim->free_blocks_from(0);
        victim->drop_cache();
        victim->raw.dtime = now_seconds();
        victim->nlink = 0;
        victim->size = 0;
        victim->save();
        vfs::forget_xattrs(victim->dev, victim->number); // the number may come back for another file
        vol->free_inode(victim->number, dir);
        vol->forget(victim->number);
    }

    long unlink(const char* name, usize len, bool remove_dir) override {
        if (!vfs::is_dir(mode))
            return -err::NOTDIR;
        if (!writable())
            return -err::ROFS;
        LockGuard guard(vol->lock);
        Node* victim = find_locked(name, len);
        if (!victim)
            return -err::NOENT;
        bool dir = vfs::is_dir(victim->mode);
        if (remove_dir && !dir) return -err::NOTDIR;
        if (!remove_dir && dir) return -err::ISDIR;
        if (dir && !victim->dir_empty()) return -err::NOTEMPTY;
        if (victim->raw.flags & FLAG_INLINE) return -err::ROFS;
        remove_entry(name, len);
        if (dir) {
            nlink--;
            save();
            release_inode(victim);
        } else if (--victim->nlink == 0) {
            drop_last_link(victim);
        } else {
            victim->save();
        }
        return 0;
    }

    // A regular file lost its last name: free it now, or (still open) at
    // its last close.
    void drop_last_link(Node* victim) {
        if (victim->in_use() && !victim->orphaned) {
            victim->orphaned = true;
            victim->save(); // links = 0
            vol->orphan_add(victim);
            vol->forget(victim->number);
            return;
        }
        release_inode(victim);
    }

    long link(const char* name, usize len, vfs::Inode* target_inode) override {
        auto* target = static_cast<Node*>(target_inode);
        if (target->dev != dev || target->vol != vol)
            return -err::XDEV;
        if (!writable())
            return -err::ROFS;
        if (len > 255)
            return -err::NAMETOOLONG;
        if (vfs::is_dir(target->mode))
            return -err::PERM;
        LockGuard guard(vol->lock);
        if (find_locked(name, len))
            return -err::EXIST;
        if (target->nlink >= 65000)
            return -err::MLINK;
        if (long r = add_entry(name, len, target->number, type_for_mode(target->mode)); r < 0)
            return r;
        target->nlink++;
        target->ctime = vfs::now();
        target->save();
        return 0;
    }

    long rename(const char* old_name, usize old_len, vfs::Inode* new_dir_inode, const char* new_name,
                usize new_len) override {
        auto* new_dir = static_cast<Node*>(new_dir_inode);
        if (new_dir->vol != vol)
            return -err::XDEV;
        if (!writable() || !new_dir->writable())
            return -err::ROFS;
        LockGuard guard(vol->lock);
        Node* moving = find_locked(old_name, old_len);
        if (!moving)
            return -err::NOENT;
        if (Node* existing = new_dir->find_locked(new_name, new_len)) {
            if (existing == moving)
                return 0;
            if (vfs::is_dir(existing->mode) && (!vfs::is_dir(moving->mode) || !existing->dir_empty()))
                return vfs::is_dir(moving->mode) ? -err::NOTEMPTY : -err::ISDIR;
            // Replace atomically enough: drop the old target, then link ours.
            new_dir->remove_entry(new_name, new_len);
            if (vfs::is_dir(existing->mode)) {
                new_dir->nlink--;
                new_dir->save();
                release_inode(existing);
            } else if (--existing->nlink == 0) {
                drop_last_link(existing);
            } else {
                existing->save();
            }
        }
        if (long r = new_dir->add_entry(new_name, new_len, moving->number, type_for_mode(moving->mode)); r < 0)
            return r;
        remove_entry(old_name, old_len);
        if (vfs::is_dir(moving->mode) && new_dir != this) {
            // Point the moved directory's ".." at its new parent.
            moving->ensure_linear();
            u64 dotdot = 0;
            moving->iterate([&](const char* n, u8 nl, u32, u8, u64 here, u64) {
                if (nl == 2 && n[0] == '.' && n[1] == '.') {
                    dotdot = here;
                    return true;
                }
                return false;
            });
            if (dotdot)
                moving->edit_dir_block(dotdot, [&](u8* buf, u32 off) { put32(buf + off, new_dir->number); });
            nlink--;
            save();
            new_dir->nlink++;
            new_dir->save();
            moving->parent = new_dir;
        }
        return 0;
    }
};

Node* Volume::node(u32 ino, Node* parent) {
    u32 h = ino % 256;
    for (Node* n = cache[h]; n; n = n->next_cached)
        if (n->number == ino)
            return n;
    auto* n = new Node(this, ino);
    if (vfs::is_dir(n->mode))
        n->parent = parent;
    n->next_cached = cache[h];
    cache[h] = n;
    return n;
}

void Volume::forget(u32 ino) {
    // Drop a cached node whose on-disk inode was freed or reinitialized. The
    // object itself stays allocated: open files may still point at it.
    Node** link = &cache[ino % 256];
    while (*link) {
        if ((*link)->number == ino) {
            *link = (*link)->next_cached;
            return;
        }
        link = &(*link)->next_cached;
    }
}


void Volume::orphan_add(Node* n) {
    n->raw.dtime = sb32(SB_LAST_ORPHAN); // next on the list
    n->save();
    u32 ino = n->number;
    meta(1024 + SB_LAST_ORPHAN, &ino, 4);
    sb_csum();
}

void Volume::orphan_remove(Node* n) {
    u32 next = n->raw.dtime;
    u32 cur = sb32(SB_LAST_ORPHAN);
    if (cur == n->number) {
        meta(1024 + SB_LAST_ORPHAN, &next, 4);
        sb_csum();
        return;
    }
    for (int guard = 0; cur && guard < 100000; guard++) {
        RawInode r;
        read_inode(cur, &r);
        if (r.dtime == n->number) {
            Node* prev = node(cur, nullptr);
            prev->raw.dtime = next;
            prev->save();
            return;
        }
        cur = r.dtime;
    }
}

void Volume::free_orphans() {
    u32 ino = sb32(SB_LAST_ORPHAN);
    int freed = 0;
    while (ino && ino <= inodes_per_group * group_count && freed < 100000) {
        Node* n = node(ino, nullptr);
        u32 next = n->raw.dtime;
        if (n->raw.links == 0 && n->raw.mode)
            n->release_inode(n);
        ino = next;
        freed++;
    }
    u32 zero = 0;
    meta(1024 + SB_LAST_ORPHAN, &zero, 4);
    sb_csum();
    if (freed)
        klog::info("ext4: freed %d file(s) deleted while open before the last shutdown", freed);
}

// ---- journal ----

u64 Volume::journal_phys(u32 lblk) { return journal.inode->file_block(lblk, false); }

void Volume::journal_write_sb(u32 start) {
    u8 sb[1024];
    u64 at = block_offset(journal_phys(0));
    block::read(dev, at, sb, sizeof(sb));
    put_be32(sb + 24, journal.sequence);
    put_be32(sb + 28, start);
    if (journal.csum_v3) {
        put_be32(sb + 0xFC, 0);
        put_be32(sb + 0xFC, crc32c(~0u, sb, sizeof(sb)));
    }
    block::write_through(dev, at, sb, sizeof(sb));
    dev->flush();
}

// Read the journal superblock; replay committed transactions if the journal
// isn't empty. Returns 0 if the journal can be used for writing.
long Volume::journal_load() {
    u32 jino = sb32(0xE0);
    if (!jino)
        return -err::OPNOTSUPP; // external journal device: not supported
    journal.inode = node(jino, nullptr);
    u8* buf = static_cast<u8*>(kmalloc(block_size));
    u8* data = static_cast<u8*>(kmalloc(block_size));
    if (!buf || !data) {
        kfree(buf);
        kfree(data);
        return -err::NOMEM;
    }
    long result = 0;
    block::read(dev, block_offset(journal_phys(0)), buf, block_size);
    u32 type = be32(buf + 4);
    journal.maxlen = be32(buf + 16);
    journal.first = be32(buf + 20);
    journal.sequence = be32(buf + 24);
    u32 start = be32(buf + 28);
    journal.incompat = type == JBD2_SB_V2 ? be32(buf + 40) : 0;
    u32 jcompat = type == JBD2_SB_V2 ? be32(buf + 36) : 0;
    journal.csum_v3 = journal.incompat & JBD2_INCOMPAT_CSUM_V3;
    journal.wide = journal.incompat & JBD2_INCOMPAT_64BIT;
    journal.seed = crc32c(~0u, buf + 48, 16);
    if (be32(buf) != JBD2_MAGIC || (type != JBD2_SB_V1 && type != JBD2_SB_V2) || be32(buf + 12) != block_size ||
        journal.first == 0 || journal.first >= journal.maxlen || jcompat ||
        (journal.incompat & ~(JBD2_INCOMPAT_REVOKE | JBD2_INCOMPAT_64BIT | JBD2_INCOMPAT_ASYNC_COMMIT | JBD2_INCOMPAT_CSUM_V3)) ||
        (!journal.wide && blocks_count > 0xFFFFFFFFull)) {
        klog::warn("ext4 %s: journal format not supported (type %u, features %#x/%#x)", dev->name, type, jcompat,
                   journal.incompat);
        result = -err::OPNOTSUPP;
    }
    u32 tag_size = journal.csum_v3 ? 16 : (journal.wide ? 12 : 8);
    auto next = [&](u32 pos) { return pos + 1 >= journal.maxlen ? journal.first : pos + 1; };

    if (result == 0 && start != 0) {
        // Pass 1: find the committed transactions and the revoked blocks.
        struct Tag { u64 blk; u32 jpos; bool escape; u32 tid; };
        struct Revoke { u64 blk; u32 tid; };
        Array<Tag> committed, pending;
        Array<Revoke> revokes, pending_revokes;
        u32 pos = start, expect = journal.sequence, transactions = 0;
        for (u32 guard = 0; guard < journal.maxlen; guard++) {
            block::read(dev, block_offset(journal_phys(pos)), buf, block_size);
            if (be32(buf) != JBD2_MAGIC || be32(buf + 8) != expect)
                break;
            u32 btype = be32(buf + 4);
            if (btype == JBD2_DESCRIPTOR) {
                u32 off = 12, end = block_size - (journal.csum_v3 ? 4 : 0);
                u32 dpos = next(pos);
                while (off + tag_size <= end) {
                    u64 blk = be32(buf + off);
                    u32 flags = journal.csum_v3 ? be32(buf + off + 4) : be16(buf + off + 6);
                    if (journal.wide)
                        blk |= static_cast<u64>(be32(buf + off + 8)) << 32;
                    pending.push(Tag{blk, dpos, (flags & JBD2_TAG_ESCAPE) != 0, expect});
                    dpos = next(dpos);
                    off += tag_size;
                    if (!(flags & JBD2_TAG_SAME_UUID))
                        off += 16;
                    if (flags & JBD2_TAG_LAST)
                        break;
                }
                pos = dpos;
            } else if (btype == JBD2_COMMIT) {
                if (journal.csum_v3) {
                    u32 want = be32(buf + 16);
                    put_be32(buf + 16, 0);
                    if (crc32c(journal.seed, buf, block_size) != want)
                        break; // torn commit: this transaction never happened
                }
                for (usize i = 0; i < pending.n; i++)
                    committed.push(pending.v[i]);
                for (usize i = 0; i < pending_revokes.n; i++)
                    revokes.push(pending_revokes.v[i]);
                pending.n = pending_revokes.n = 0;
                transactions++;
                expect++;
                pos = next(pos);
            } else if (btype == JBD2_REVOKE) {
                u32 used = min<u32>(be32(buf + 12), block_size - (journal.csum_v3 ? 4 : 0));
                u32 rsize = journal.wide ? 8 : 4;
                for (u32 off = 16; off + rsize <= used; off += rsize) {
                    u64 blk = journal.wide ? (static_cast<u64>(be32(buf + off)) << 32) | be32(buf + off + 4) : be32(buf + off);
                    pending_revokes.push(Revoke{blk, expect});
                }
                pos = next(pos);
            } else {
                break;
            }
        }
        // Pass 2: write the journaled copies home, oldest first.
        u32 replayed = 0;
        for (usize i = 0; i < committed.n; i++) {
            const Tag& t = committed.v[i];
            bool revoked = false;
            for (usize k = 0; k < revokes.n && !revoked; k++)
                revoked = revokes.v[k].blk == t.blk && revokes.v[k].tid >= t.tid;
            if (revoked || t.blk >= blocks_count)
                continue;
            block::read(dev, block_offset(journal_phys(t.jpos)), data, block_size);
            if (t.escape)
                put_be32(data, JBD2_MAGIC);
            block::write(dev, block_offset(t.blk), data, block_size);
            replayed++;
        }
        block::sync_device(dev);
        journal.sequence = expect;
        journal_write_sb(0);
        klog::ok("ext4 %s: journal replayed (%u transactions, %u blocks)", dev->name, transactions, replayed);
    }
    kfree(buf);
    kfree(data);
    if (result == 0)
        journal.active = true;
    return result;
}

// Test hook: "jbd2_crash=before|after" on the kernel command line stops the
// machine at the second commit, just before / right after the commit block
// reaches the disk (crash-recovery tests).
int crash_test_mode() {
    static int mode = -1;
    if (mode < 0) {
        mode = 0;
        const char* c = boot::cmdline();
        for (const char* p = c; *p; p++) {
            if (memcmp(p, "jbd2_crash=before", 17) == 0) mode = 1;
            if (memcmp(p, "jbd2_crash=after", 16) == 0) mode = 2;
        }
    }
    return mode;
}

// Commit the running transaction, then checkpoint it.
void Volume::commit_locked() {
    if (!journal.active || journal.txn.n == 0)
        return;
    u32 n = static_cast<u32>(journal.txn.n);
    u32 tag_size = journal.csum_v3 ? 16 : (journal.wide ? 12 : 8);
    u32 per_desc = (block_size - 12 - (journal.csum_v3 ? 4 : 0) - 16) / tag_size;
    u32 needed = (n + per_desc - 1) / per_desc + n + 1;
    auto release_all = [&] {
        for (usize i = 0; i < journal.txn.n; i++)
            block::hold(dev, block_offset(journal.txn.v[i]), block_size, -1);
        journal.txn.n = 0;
        memset(journal.set, 0, journal.set_cap * sizeof(u64));
    };
    if (needed > journal.maxlen - journal.first) {
        klog::warn("ext4 %s: transaction of %u blocks doesn't fit the journal; writing it directly", dev->name, n);
        release_all();
        block::sync_device(dev);
        return;
    }
    u8* desc = static_cast<u8*>(kmalloc(block_size));
    u8* data = static_cast<u8*>(kmalloc(block_size));
    if (!desc || !data) {
        kfree(desc);
        kfree(data);
        return;
    }
    u32 tid = journal.sequence;
    block::sync_device(dev);           // 1. file data first ("ordered" mode)
    journal_write_sb(journal.first);   // 2. the journal now starts here
    u32 pos = journal.first;           // 3. descriptor blocks + copies
    u8 juuid[16];
    block::read(dev, block_offset(journal_phys(0)) + 48, juuid, 16);
    for (u32 i = 0; i < n;) {
        u32 count = min<u32>(per_desc, n - i);
        memset(desc, 0, block_size);
        put_be32(desc, JBD2_MAGIC);
        put_be32(desc + 4, JBD2_DESCRIPTOR);
        put_be32(desc + 8, tid);
        u32 off = 12;
        for (u32 k = 0; k < count; k++) {
            u64 blk = journal.txn.v[i + k];
            block::read(dev, block_offset(blk), data, block_size);
            u32 flags = 0;
            if (be32(data) == JBD2_MAGIC) { // a block that looks like a journal block: escape it
                put_be32(data, 0);
                flags |= JBD2_TAG_ESCAPE;
            }
            if (k > 0)
                flags |= JBD2_TAG_SAME_UUID;
            if (k == count - 1)
                flags |= JBD2_TAG_LAST;
            u8* tag = desc + off;
            put_be32(tag, static_cast<u32>(blk));
            if (journal.csum_v3) {
                put_be32(tag + 4, flags);
                put_be32(tag + 8, static_cast<u32>(blk >> 32));
                u8 seq[4];
                put_be32(seq, tid);
                put_be32(tag + 12, crc32c(crc32c(journal.seed, seq, 4), data, block_size));
            } else {
                put_be16(tag + 6, static_cast<u16>(flags));
                if (journal.wide)
                    put_be32(tag + 8, static_cast<u32>(blk >> 32));
            }
            off += tag_size;
            if (k == 0) {
                memcpy(desc + off, juuid, 16);
                off += 16;
            }
            block::write_through(dev, block_offset(journal_phys(pos + 1 + k)), data, block_size);
        }
        if (journal.csum_v3)
            put_be32(desc + block_size - 4, crc32c(journal.seed, desc, block_size));
        block::write_through(dev, block_offset(journal_phys(pos)), desc, block_size);
        pos += 1 + count;
        i += count;
    }
    dev->flush();
    static int commits = 0;
    bool crash_now = crash_test_mode() && ++commits == 2;
    if (crash_now && crash_test_mode() == 1)
        panic("jbd2 crash test: stopped before the commit block of transaction %u (%u blocks)", tid, n);
    memset(desc, 0, block_size);       // 4. the commit block: from here on the transaction counts
    put_be32(desc, JBD2_MAGIC);
    put_be32(desc + 4, JBD2_COMMIT);
    put_be32(desc + 8, tid);
    u64 now_ns = rtc::realtime_ns();
    put_be32(desc + 48, static_cast<u32>((now_ns / 1'000'000'000) >> 32));
    put_be32(desc + 52, static_cast<u32>(now_ns / 1'000'000'000));
    put_be32(desc + 56, static_cast<u32>(now_ns % 1'000'000'000));
    if (journal.csum_v3)
        put_be32(desc + 16, crc32c(journal.seed, desc, block_size));
    block::write_through(dev, block_offset(journal_phys(pos)), desc, block_size);
    dev->flush();
    if (crash_now)
        panic("jbd2 crash test: stopped after committing transaction %u (%u blocks)", tid, n);
    release_all();                     // 5. checkpoint: the blocks go home
    block::sync_device(dev);
    journal.sequence = tid + 1;        // 6. the journal is empty again
    journal_write_sb(0);
    kfree(desc);
    kfree(data);
}

Volume* g_volumes[16];
usize g_volume_count = 0;

} // namespace

u64 cached_pages() { return __atomic_load_n(&g_cached_pages, __ATOMIC_RELAXED); }

// pmm reclaimer: walk every volume's nodes (round-robin, so no file is
// always the first to lose its cache). Volumes whose lock is busy are skipped:
// the allocation that needed memory may be holding one.
u64 reclaim_cache(u64 wanted) {
    static u32 cursor = 0;
    u64 freed = 0;
    for (usize vi = 0; vi < g_volume_count && freed < wanted; vi++) {
        Volume* v = g_volumes[vi];
        if (!v->lock.try_lock())
            continue;
        for (u32 n = 0; n < 256 && freed < wanted; n++) {
            u32 bucket = (cursor + n) % 256;
            for (Node* node = v->cache[bucket]; node && freed < wanted; node = node->next_cached)
                freed += node->shrink_cache(wanted - freed);
        }
        v->lock.unlock();
    }
    cursor += 17;
    return freed;
}

void shutdown_all() {
    for (usize i = 0; i < g_volume_count; i++) {
        Volume* v = g_volumes[i];
        LockGuard guard(v->lock);
        if (v->read_only)
            continue;
        v->commit_locked(); // the journal is empty afterwards
        if (v->journal.active) {
            // Cleanly unmounted: nothing to replay at the next mount.
            u32 inc = v->sb32(96) & ~INCOMPAT_RECOVER;
            block::write(v->dev, 1024 + 96, &inc, 4);
            v->journal.active = false; // no more commits / holds
            v->sb_csum();
        }
        block::sync_device(v->dev);
    }
}

bool probe(block::Device* dev) {
    u16 magic = 0;
    if (dev->size_bytes() < 4096 || block::read(dev, 1024 + 56, &magic, 2) != 0)
        return false;
    return magic == MAGIC;
}

long mount(block::Device* dev, vfs::Inode** root, bool* read_only, const char** kind_out) {
    if (!probe(dev))
        return -err::INVAL;
    u8 sb[1024];
    block::read(dev, 1024, sb, sizeof(sb));
    auto u32_at = [&](u32 off) { return get32(sb + off); };
    auto u16_at = [&](u32 off) { return get16(sb + off); };

    auto* v = new Volume;
    v->dev = dev;
    v->block_size = 1024u << u32_at(24);
    v->blocks_per_group = u32_at(32);
    v->inodes_per_group = u32_at(40);
    v->first_data_block = u32_at(20);
    u32 rev = u32_at(76);
    v->inode_size = rev >= 1 ? u16_at(88) : 128;
    v->first_ino = rev >= 1 ? u32_at(0x54) : 11;
    v->compat = rev >= 1 ? u32_at(92) : 0;
    v->incompat = rev >= 1 ? u32_at(96) : 0;
    v->ro_compat = rev >= 1 ? u32_at(100) : 0;
    v->has_filetype = v->incompat & INCOMPAT_FILETYPE;
    v->blocks_count = u32_at(4);
    v->desc_size = 32;
    if (v->incompat & INCOMPAT_64BIT) {
        v->blocks_count |= static_cast<u64>(u32_at(0x150)) << 32;
        v->desc_size = max<u32>(u16_at(0xFE), 32);
    }
    if (v->block_size > 4096 || !v->blocks_per_group || !v->inodes_per_group || v->inode_size < 128 ||
        v->inode_size > 1024 || v->desc_size > 64) {
        delete v;
        return -err::INVAL;
    }
    if (v->incompat & ~INCOMPAT_READ_OK) {
        klog::warn("ext: %s uses unsupported features (incompat %#x)", dev->name, v->incompat & ~INCOMPAT_READ_OK);
        delete v;
        return -err::INVAL;
    }
    memcpy(v->uuid, sb + 0x68, 16);
    v->csum = v->ro_compat & RO_METADATA_CSUM;
    v->gdt_csum = !v->csum && (v->ro_compat & RO_GDT_CSUM);
    v->sparse = v->ro_compat & RO_SPARSE_SUPER;
    v->extents = v->incompat & INCOMPAT_EXTENTS;
    v->csum_seed = (v->incompat & INCOMPAT_CSUM_SEED) ? u32_at(0x270) : crc32c(~0u, v->uuid, 16);
    v->reserved_gdt = u16_at(0xCE);
    if (v->inode_size > 128) {
        u16 want = u16_at(0x15E);
        v->extra_isize = static_cast<u16>(min<u32>(want ? want : 32, v->inode_size - 128));
    }
    v->group_count = static_cast<u32>((v->blocks_count - v->first_data_block + v->blocks_per_group - 1) / v->blocks_per_group);
    v->gdt_blocks = (v->group_count * v->desc_size + v->block_size - 1) / v->block_size;
    v->gdt_block = v->first_data_block + 1;
    // We can write only what we fully understand; anything else stays safe.
    v->read_only = (v->incompat & ~(INCOMPAT_WRITE_OK | INCOMPAT_RECOVER)) || (v->ro_compat & ~RO_WRITE_OK);
    bool needed_recovery = v->incompat & INCOMPAT_RECOVER;
    if (!v->read_only && (v->compat & COMPAT_HAS_JOURNAL)) {
        if (v->journal_load() == 0) {
            // While we have it mounted, the journal is in use (as on Linux):
            // after a crash, whoever mounts it next replays it first.
            u32 inc = v->sb32(96) | INCOMPAT_RECOVER;
            block::write(dev, 1024 + 96, &inc, 4);
            v->incompat = inc;
            v->sb_csum();
            block::sync_device(dev);
            block::add_sync_hook([](void* ctx) {
                auto* vol = static_cast<Volume*>(ctx);
                LockGuard guard(vol->lock);
                vol->commit_locked();
            }, v);
        } else {
            v->read_only = true;
        }
    } else if (needed_recovery) {
        v->read_only = true; // a journal we can't replay
    }
    *read_only = v->read_only;
    if (!v->read_only && v->sb32(Volume::SB_LAST_ORPHAN)) {
        LockGuard guard(v->lock);
        v->free_orphans();
        v->commit_locked();
    }

    const char* kind = (v->incompat & (INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG)) ? "ext4"
                       : (v->compat & COMPAT_HAS_JOURNAL)                                    ? "ext3"
                                                                                             : "ext2";
    if (kind_out)
        *kind_out = kind;
    char label[17] = {};
    memcpy(label, sb + 120, 16);
    const char* why = "";
    if (v->read_only)
        why = needed_recovery ? " (journal needs replay)" : " (unsupported features)";
    else if (v->journal.active)
        why = ", journaled";
    klog::ok("%s on %s%s%s%s: %lu MiB, %u-byte blocks, %s%s%s", kind, dev->name, label[0] ? " (\"" : "", label,
             label[0] ? "\")" : "", v->blocks_count * v->block_size / MiB, v->block_size,
             v->read_only ? "read-only" : "read-write", v->csum ? ", checksums" : "", why);
    *root = v->node(ROOT_INO, nullptr);
    if (g_volume_count < 16)
        g_volumes[g_volume_count++] = v;
        static bool registered = false;
        if (!registered) {
            registered = true;
            pmm::register_reclaimer(reclaim_cache);
        }
    return 0;
}

} // namespace ext2
