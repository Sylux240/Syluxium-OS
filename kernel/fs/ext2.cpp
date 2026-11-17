#include <core/log.hpp>
#include <fs/ext2.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <sched/sync.hpp>
#include <time/rtc.hpp>

namespace ext2 {

namespace {

constexpr u16 MAGIC = 0xEF53;
constexpr u32 ROOT_INO = 2;

// Feature bits.
constexpr u32 INCOMPAT_FILETYPE = 0x2, INCOMPAT_RECOVER = 0x4, INCOMPAT_META_BG = 0x10, INCOMPAT_EXTENTS = 0x40,
              INCOMPAT_64BIT = 0x80, INCOMPAT_FLEX_BG = 0x200,
              INCOMPAT_CSUM_SEED = 0x2000, INCOMPAT_LARGEDIR = 0x4000;
constexpr u32 INCOMPAT_READ_OK = INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_EXTENTS | INCOMPAT_64BIT |
                                 INCOMPAT_FLEX_BG | INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR | INCOMPAT_META_BG;
constexpr u32 INCOMPAT_WRITE_OK = INCOMPAT_FILETYPE | INCOMPAT_FLEX_BG;
constexpr u32 RO_SPARSE_SUPER = 0x1, RO_LARGE_FILE = 0x2, RO_HUGE_FILE = 0x8, RO_GDT_CSUM = 0x10,
              RO_DIR_NLINK = 0x20, RO_EXTRA_ISIZE = 0x40, RO_METADATA_CSUM = 0x400;
constexpr u32 RO_WRITE_OK = RO_SPARSE_SUPER | RO_LARGE_FILE | RO_HUGE_FILE | RO_DIR_NLINK | RO_EXTRA_ISIZE;

constexpr u32 FLAG_INDEX = 0x1000, FLAG_EXTENTS = 0x80000, FLAG_INLINE = 0x10000000;
constexpr u8 FT_UNKNOWN = 0, FT_REG = 1, FT_DIR = 2, FT_CHR = 3, FT_BLK = 4, FT_FIFO = 5, FT_SOCK = 6, FT_SYMLINK = 7;

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

class Node;

class Volume {
public:
    block::Device* dev;
    u32 block_size;
    u32 inodes_per_group, blocks_per_group, inode_size, group_count, desc_size;
    u64 blocks_count, first_data_block, gdt_block;
    u32 incompat, ro_compat;
    bool read_only;
    bool has_filetype;
    u64 fs_dev = vfs::next_fs_dev();
    sched::Mutex lock;
    Node* cache[256] = {};

    u64 block_offset(u64 b) const { return b * block_size; }
    u32 ptrs_per_block() const { return block_size / 4; }

    // ---- superblock / group descriptors ----
    u32 sb32(u32 off) { u32 v = 0; block::read(dev, 1024 + off, &v, 4); return v; }
    void sb_add32(u32 off, i64 delta) {
        u32 v = sb32(off);
        v = static_cast<u32>(static_cast<i64>(v) + delta);
        block::write(dev, 1024 + off, &v, 4);
    }

    u64 gd_offset(u32 g) const { return block_offset(gdt_block) + static_cast<u64>(g) * desc_size; }
    u64 gd_block_bitmap(u32 g) {
        u8 d[64] = {};
        block::read(dev, gd_offset(g), d, desc_size);
        u32 lo, hi = 0;
        memcpy(&lo, d + 0, 4);
        if (desc_size >= 64) memcpy(&hi, d + 0x20, 4);
        return lo | (static_cast<u64>(hi) << 32);
    }
    u64 gd_inode_bitmap(u32 g) {
        u8 d[64] = {};
        block::read(dev, gd_offset(g), d, desc_size);
        u32 lo, hi = 0;
        memcpy(&lo, d + 4, 4);
        if (desc_size >= 64) memcpy(&hi, d + 0x24, 4);
        return lo | (static_cast<u64>(hi) << 32);
    }
    u64 gd_inode_table(u32 g) {
        u8 d[64] = {};
        block::read(dev, gd_offset(g), d, desc_size);
        u32 lo, hi = 0;
        memcpy(&lo, d + 8, 4);
        if (desc_size >= 64) memcpy(&hi, d + 0x28, 4);
        return lo | (static_cast<u64>(hi) << 32);
    }
    u16 gd16(u32 g, u32 off) { u16 v = 0; block::read(dev, gd_offset(g) + off, &v, 2); return v; }
    void gd_add16(u32 g, u32 off, int delta) {
        u16 v = static_cast<u16>(gd16(g, off) + delta);
        block::write(dev, gd_offset(g) + off, &v, 2);
    }

    // ---- inodes ----
    u64 inode_offset(u32 ino) {
        u32 g = (ino - 1) / inodes_per_group;
        u32 idx = (ino - 1) % inodes_per_group;
        return block_offset(gd_inode_table(g)) + static_cast<u64>(idx) * inode_size;
    }
    void read_inode(u32 ino, RawInode* out) { block::read(dev, inode_offset(ino), out, sizeof(RawInode)); }
    void write_inode(u32 ino, const RawInode& in) { block::write(dev, inode_offset(ino), &in, sizeof(RawInode)); }

    // ---- allocation (ext2-style bitmaps) ----
    u64 alloc_block(u32 goal_group) {
        u8* bitmap = static_cast<u8*>(kmalloc(block_size));
        if (!bitmap)
            return 0;
        u64 result = 0;
        for (u32 n = 0; n < group_count && !result; n++) {
            u32 g = (goal_group + n) % group_count;
            if (gd16(g, 12) == 0)
                continue; // no free blocks in this group
            u64 bm = gd_block_bitmap(g);
            block::read(dev, block_offset(bm), bitmap, block_size);
            u32 in_group = min<u64>(blocks_per_group, blocks_count - first_data_block - static_cast<u64>(g) * blocks_per_group);
            for (u32 i = 0; i < in_group; i++) {
                if (bitmap[i / 8] & (1 << (i % 8)))
                    continue;
                bitmap[i / 8] |= static_cast<u8>(1 << (i % 8));
                block::write(dev, block_offset(bm) + i / 8, &bitmap[i / 8], 1);
                gd_add16(g, 12, -1);
                sb_add32(12, -1);
                result = first_data_block + static_cast<u64>(g) * blocks_per_group + i;
                break;
            }
        }
        if (result) { // fresh blocks read back as zeros
            memset(bitmap, 0, block_size);
            block::write(dev, block_offset(result), bitmap, block_size);
        }
        kfree(bitmap);
        return result;
    }

    void free_block(u64 b) {
        if (b < first_data_block || b >= blocks_count)
            return;
        u64 rel = b - first_data_block;
        u32 g = static_cast<u32>(rel / blocks_per_group);
        u32 i = static_cast<u32>(rel % blocks_per_group);
        u64 bm = gd_block_bitmap(g);
        u8 byte;
        block::read(dev, block_offset(bm) + i / 8, &byte, 1);
        if (!(byte & (1 << (i % 8))))
            return; // already free
        byte &= static_cast<u8>(~(1 << (i % 8)));
        block::write(dev, block_offset(bm) + i / 8, &byte, 1);
        gd_add16(g, 12, 1);
        sb_add32(12, 1);
    }

    u32 alloc_inode(u32 goal_group, bool dir) {
        u8* bitmap = static_cast<u8*>(kmalloc(block_size));
        if (!bitmap)
            return 0;
        u32 result = 0;
        for (u32 n = 0; n < group_count && !result; n++) {
            u32 g = (goal_group + n) % group_count;
            if (gd16(g, 14) == 0)
                continue;
            u64 bm = gd_inode_bitmap(g);
            block::read(dev, block_offset(bm), bitmap, block_size);
            for (u32 i = 0; i < inodes_per_group; i++) {
                u32 ino = g * inodes_per_group + i + 1;
                if (ino < 11 || (bitmap[i / 8] & (1 << (i % 8))))
                    continue; // inodes 1-10 are reserved
                bitmap[i / 8] |= static_cast<u8>(1 << (i % 8));
                block::write(dev, block_offset(bm) + i / 8, &bitmap[i / 8], 1);
                gd_add16(g, 14, -1);
                if (dir)
                    gd_add16(g, 16, 1);
                sb_add32(16, -1);
                result = ino;
                break;
            }
        }
        kfree(bitmap);
        return result;
    }

    void free_inode(u32 ino, bool dir) {
        u32 g = (ino - 1) / inodes_per_group;
        u32 i = (ino - 1) % inodes_per_group;
        u64 bm = gd_inode_bitmap(g);
        u8 byte;
        block::read(dev, block_offset(bm) + i / 8, &byte, 1);
        byte &= static_cast<u8>(~(1 << (i % 8)));
        block::write(dev, block_offset(bm) + i / 8, &byte, 1);
        gd_add16(g, 14, 1);
        if (dir)
            gd_add16(g, 16, -1);
        sb_add32(16, 1);
    }

    Node* node(u32 ino, Node* parent);
    void forget(u32 ino);
};

class Node : public vfs::Inode {
public:
    Volume* vol;
    u32 number;
    RawInode raw;
    Node* next_cached = nullptr;

    Node(Volume* v, u32 inode_number) : vol(v), number(inode_number) {
        v->read_inode(inode_number, &raw);
        ino = inode_number;
        dev = v->fs_dev;
        refresh();
    }

    void refresh() {
        mode = raw.mode;
        uid = raw.uid;
        gid = raw.gid;
        nlink = raw.links;
        size = raw.size_lo | (vfs::is_reg(raw.mode) ? static_cast<u64>(raw.size_hi) << 32 : 0);
        atime = vfs::Timespec{raw.atime, 0};
        mtime = vfs::Timespec{raw.mtime, 0};
        ctime = vfs::Timespec{raw.ctime, 0};
    }

    void save() {
        raw.mode = static_cast<u16>(mode);
        raw.links = static_cast<u16>(nlink);
        raw.size_lo = static_cast<u32>(size);
        if (vfs::is_reg(mode))
            raw.size_hi = static_cast<u32>(size >> 32);
        raw.mtime = static_cast<u32>(mtime.sec);
        raw.ctime = static_cast<u32>(ctime.sec);
        raw.atime = static_cast<u32>(atime.sec);
        vol->write_inode(number, raw);
    }

    bool uses_extents() const { return raw.flags & FLAG_EXTENTS; }

    // ---- block mapping ----

    // Physical block for file block `n` via the extent tree (0 = hole).
    u64 extent_block(u64 n) {
        u8 node_buf[4096];
        const u8* hdr = reinterpret_cast<const u8*>(raw.block);
        u8* owned = nullptr;
        for (int level = 0; level < 8; level++) {
            u16 magic, entries, depth;
            memcpy(&magic, hdr, 2);
            memcpy(&entries, hdr + 2, 2);
            memcpy(&depth, hdr + 6, 2);
            if (magic != 0xF30A)
                break;
            const u8* e = hdr + 12;
            if (depth == 0) {
                for (u16 i = 0; i < entries; i++, e += 12) {
                    u32 first;
                    u16 len, hi;
                    u32 lo;
                    memcpy(&first, e, 4);
                    memcpy(&len, e + 4, 2);
                    memcpy(&hi, e + 6, 2);
                    memcpy(&lo, e + 8, 4);
                    bool uninit = len > 32768;
                    if (uninit) len -= 32768;
                    if (n >= first && n < first + len) {
                        kfree(owned);
                        return uninit ? 0 : (static_cast<u64>(hi) << 32 | lo) + (n - first);
                    }
                }
                break;
            }
            // Index node: follow the last entry whose first block <= n.
            u64 child = 0;
            for (u16 i = 0; i < entries; i++, e += 12) {
                u32 first, lo;
                u16 hi;
                memcpy(&first, e, 4);
                memcpy(&lo, e + 4, 4);
                memcpy(&hi, e + 8, 2);
                if (first > n)
                    break;
                child = static_cast<u64>(hi) << 32 | lo;
            }
            if (!child || vol->block_size > sizeof(node_buf))
                break;
            block::read(vol->dev, vol->block_offset(child), node_buf, vol->block_size);
            hdr = node_buf;
        }
        kfree(owned);
        return 0;
    }

    // Block-mapped (ext2/ext3) files: direct, single, double, triple indirect.
    u64 mapped_block(u64 n, bool create) {
        u32 ppb = vol->ptrs_per_block();
        u32 goal = (number - 1) / vol->inodes_per_group;
        if (n < 12) {
            if (!raw.block[n] && create) {
                raw.block[n] = static_cast<u32>(vol->alloc_block(goal));
                if (raw.block[n]) raw.blocks_lo += vol->block_size / 512;
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
            raw.blocks_lo += vol->block_size / 512;
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
                entry = static_cast<u32>(vol->alloc_block(goal));
                if (!entry) return 0;
                block::write(vol->dev, off, &entry, 4);
                raw.blocks_lo += vol->block_size / 512;
                save();
            }
            table = entry;
        }
        return table;
    }

    u64 file_block(u64 n, bool create) {
        if (uses_extents())
            return create ? 0 : extent_block(n);
        return mapped_block(n, create);
    }

    // Free every data and indirect block from file block `from` on (block-mapped files).
    void free_blocks_from(u64 from) {
        u32 ppb = vol->ptrs_per_block();
        for (u64 n = from; n < 12; n++) {
            if (raw.block[n]) { vol->free_block(raw.block[n]); raw.block[n] = 0; raw.blocks_lo -= vol->block_size / 512; }
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
            vol->free_block(blk);
            raw.blocks_lo -= vol->block_size / 512;
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

    isize write(u64 offset, const void* buf, usize count) override {
        if (vfs::is_dir(mode))
            return -err::ISDIR;
        if (vol->read_only || uses_extents())
            return -err::ROFS;
        LockGuard guard(vol->lock);
        return write_locked(offset, buf, count);
    }

    isize write_locked(u64 offset, const void* buf, usize count) {
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
        if (vol->read_only || uses_extents())
            return -err::ROFS;
        LockGuard guard(vol->lock);
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
                u32 ino;
                u16 rec_len;
                memcpy(&ino, blockbuf + off, 4);
                memcpy(&rec_len, blockbuf + off + 4, 2);
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

    // Add a directory entry (directory must be block-mapped and writable).
    long add_entry(const char* name, usize len, u32 ino, u8 type) {
        u16 needed = entry_size(static_cast<u32>(len));
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
            while (off + 8 <= vol->block_size) {
                u32 e_ino;
                u16 rec_len;
                memcpy(&e_ino, blockbuf + off, 4);
                memcpy(&rec_len, blockbuf + off + 4, 2);
                if (rec_len < 8)
                    break;
                u16 used = e_ino ? entry_size(blockbuf[off + 6]) : 0;
                if (rec_len - used >= needed) {
                    u32 at = off + used;
                    if (e_ino) { // split: shrink the existing entry, ours takes the rest
                        memcpy(blockbuf + off + 4, &used, 2);
                    }
                    u16 ours = static_cast<u16>(rec_len - used);
                    memcpy(blockbuf + at, &ino, 4);
                    memcpy(blockbuf + at + 4, &ours, 2);
                    blockbuf[at + 6] = static_cast<u8>(len);
                    blockbuf[at + 7] = vol->has_filetype ? type : 0;
                    memcpy(blockbuf + at + 8, name, len);
                    block::write(vol->dev, vol->block_offset(b), blockbuf, vol->block_size);
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
        u16 whole = static_cast<u16>(vol->block_size);
        memcpy(blockbuf, &ino, 4);
        memcpy(blockbuf + 4, &whole, 2);
        blockbuf[6] = static_cast<u8>(len);
        blockbuf[7] = vol->has_filetype ? type : 0;
        memcpy(blockbuf + 8, name, len);
        block::write(vol->dev, vol->block_offset(b), blockbuf, vol->block_size);
        kfree(blockbuf);
        size += vol->block_size;
        touch_dir();
        return 0;
    }

    void touch_dir() {
        raw.flags &= ~FLAG_INDEX; // we don't maintain htree indexes: fall back to linear lookup
        mtime = ctime = vfs::now();
        save();
    }

    // Remove the entry for `name`; returns its inode number (0 if absent).
    u32 remove_entry(const char* name, usize len) {
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
        if (prev >= block_start && prev && prev != at) {
            // Merge into the previous entry of the same block.
            u16 prev_len, our_len;
            block::read(vol->dev, prev + 4, &prev_len, 2);
            block::read(vol->dev, at + 4, &our_len, 2);
            prev_len = static_cast<u16>(prev_len + our_len);
            block::write(vol->dev, prev + 4, &prev_len, 2);
        } else {
            u32 zero = 0; // first entry in its block: just mark it unused
            block::write(vol->dev, at, &zero, 4);
        }
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
        if (vol->read_only || uses_extents())
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
        u32 goal = (number - 1) / vol->inodes_per_group;
        u32 ino = vol->alloc_inode(goal, dir);
        if (!ino)
            return -err::NOSPC;

        RawInode r{};
        u32 t = now_seconds();
        r.mode = static_cast<u16>(type | (new_mode & 07777));
        r.links = dir ? 2 : 1;
        r.atime = r.ctime = r.mtime = t;
        vol->write_inode(ino, r);
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
            u16 dot_len = 12, dotdot_len = static_cast<u16>(vol->block_size - 12);
            memcpy(blockbuf, &ino, 4);
            memcpy(blockbuf + 4, &dot_len, 2);
            blockbuf[6] = 1;
            blockbuf[7] = vol->has_filetype ? FT_DIR : 0;
            blockbuf[8] = '.';
            memcpy(blockbuf + 12, &number, 4);
            memcpy(blockbuf + 16, &dotdot_len, 2);
            blockbuf[18] = 2;
            blockbuf[19] = vol->has_filetype ? FT_DIR : 0;
            blockbuf[20] = '.';
            blockbuf[21] = '.';
            block::write(vol->dev, vol->block_offset(b), blockbuf, vol->block_size);
            kfree(blockbuf);
            n->size = vol->block_size;
            n->save();
            nlink++; // the new ".." refers to us
            save();
        } else if (type == vfs::S_IFLNK && target) {
            usize tl = strlen(target);
            if (tl < 60) {
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
        if (!victim->uses_extents())
            victim->free_blocks_from(0);
        victim->raw.dtime = now_seconds();
        victim->nlink = 0;
        victim->size = 0;
        victim->save();
        vol->free_inode(victim->number, dir);
        vol->forget(victim->number);
    }

    long unlink(const char* name, usize len, bool remove_dir) override {
        if (!vfs::is_dir(mode))
            return -err::NOTDIR;
        if (vol->read_only || uses_extents())
            return -err::ROFS;
        LockGuard guard(vol->lock);
        Node* victim = find_locked(name, len);
        if (!victim)
            return -err::NOENT;
        bool dir = vfs::is_dir(victim->mode);
        if (remove_dir && !dir) return -err::NOTDIR;
        if (!remove_dir && dir) return -err::ISDIR;
        if (dir && !victim->dir_empty()) return -err::NOTEMPTY;
        if (victim->uses_extents()) return -err::ROFS;
        remove_entry(name, len);
        if (dir) {
            nlink--;
            save();
            release_inode(victim);
        } else if (--victim->nlink == 0) {
            release_inode(victim);
        } else {
            victim->save();
        }
        return 0;
    }

    long rename(const char* old_name, usize old_len, vfs::Inode* new_dir_inode, const char* new_name,
                usize new_len) override {
        auto* new_dir = static_cast<Node*>(new_dir_inode);
        if (new_dir->vol != vol)
            return -err::XDEV;
        if (vol->read_only || uses_extents() || new_dir->uses_extents())
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
                release_inode(existing);
            } else {
                existing->save();
            }
        }
        if (long r = new_dir->add_entry(new_name, new_len, moving->number, type_for_mode(moving->mode)); r < 0)
            return r;
        remove_entry(old_name, old_len);
        if (vfs::is_dir(moving->mode) && new_dir != this) {
            // Point the moved directory's ".." at its new parent.
            moving->iterate([&](const char* n, u8 nl, u32, u8, u64 here, u64) {
                if (nl == 2 && n[0] == '.' && n[1] == '.') {
                    block::write(vol->dev, here, &new_dir->number, 4);
                    return true;
                }
                return false;
            });
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

} // namespace

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
    auto u32_at = [&](u32 off) { u32 v; memcpy(&v, sb + off, 4); return v; };
    auto u16_at = [&](u32 off) { u16 v; memcpy(&v, sb + off, 2); return v; };

    auto* v = new Volume;
    v->dev = dev;
    v->block_size = 1024u << u32_at(24);
    v->blocks_per_group = u32_at(32);
    v->inodes_per_group = u32_at(40);
    v->first_data_block = u32_at(20);
    u32 rev = u32_at(76);
    v->inode_size = rev >= 1 ? u16_at(88) : 128;
    v->incompat = rev >= 1 ? u32_at(96) : 0;
    v->ro_compat = rev >= 1 ? u32_at(100) : 0;
    v->has_filetype = v->incompat & INCOMPAT_FILETYPE;
    v->blocks_count = u32_at(4);
    v->desc_size = 32;
    if (v->incompat & INCOMPAT_64BIT) {
        v->blocks_count |= static_cast<u64>(u32_at(0x150)) << 32;
        v->desc_size = max<u32>(u16_at(0xFE), 32);
    }
    if (v->block_size > 4096 || !v->blocks_per_group || !v->inodes_per_group) {
        delete v;
        return -err::INVAL;
    }
    if (v->incompat & ~INCOMPAT_READ_OK) {
        klog::warn("ext: %s uses unsupported features (incompat %#x)", dev->name, v->incompat & ~INCOMPAT_READ_OK);
        delete v;
        return -err::INVAL;
    }
    v->group_count = static_cast<u32>((v->blocks_count - v->first_data_block + v->blocks_per_group - 1) / v->blocks_per_group);
    v->gdt_block = v->first_data_block + 1;
    // We can write only what we fully understand; anything else stays safe.
    v->read_only = (v->incompat & ~INCOMPAT_WRITE_OK) || (v->ro_compat & ~RO_WRITE_OK);
    *read_only = v->read_only;

    const char* kind = (v->incompat & INCOMPAT_EXTENTS) ? "ext4" : (u32_at(92) & 0x4) ? "ext3" : "ext2";
    if (kind_out)
        *kind_out = kind;
    char label[17] = {};
    memcpy(label, sb + 120, 16);
    klog::ok("%s on %s%s%s%s: %lu MiB, %u-byte blocks, %s", kind, dev->name, label[0] ? " (\"" : "", label,
             label[0] ? "\")" : "", v->blocks_count * v->block_size / MiB, v->block_size,
             v->read_only ? "read-only" : "read-write");
    *root = v->node(ROOT_INO, nullptr);
    (void)RO_GDT_CSUM;
    (void)RO_METADATA_CSUM;
    return 0;
}

} // namespace ext2
