// exFAT file system.
//
// Layout: boot region, one FAT (u32 per cluster), the cluster heap. Free
// space is tracked by an allocation bitmap (a file in the root directory),
// not by the FAT. Files may be contiguous ("NoFatChain": FAT entries
// unused) or chained through the FAT. A file is a "directory entry set":
// a File entry (0x85), a Stream Extension (0xC0: size, first cluster) and
// File Name entries (0xC1, 15 UTF-16 units each), protected by a checksum.
//
// When we grow a contiguous file we convert it to a FAT chain, which is
// always valid; the allocation bitmap is kept in memory and written
// through.

#include <core/log.hpp>
#include <fs/exfat.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <sched/sync.hpp>
#include <time/rtc.hpp>

namespace exfat {

namespace {

constexpr u32 EOC = 0xFFFFFFFF;
constexpr u32 BAD = 0xFFFFFFF7;
constexpr u8 TYPE_BITMAP = 0x81, TYPE_UPCASE = 0x82, TYPE_FILE = 0x85, TYPE_STREAM = 0xC0, TYPE_NAME = 0xC1;
constexpr u16 ATTR_READONLY = 0x01, ATTR_DIR = 0x10, ATTR_ARCHIVE = 0x20;
constexpr u8 FLAG_ALLOC_POSSIBLE = 0x01, FLAG_NO_FAT_CHAIN = 0x02;
constexpr usize MAX_SET = 2 + 17; // file + stream + up to 255 chars / 15

struct Entry {
    u8 raw[32];
    u8 type() const { return raw[0]; }
    template <typename T> T get(usize off) const {
        T v;
        memcpy(&v, raw + off, sizeof(T));
        return v;
    }
    template <typename T> void set(usize off, T v) { memcpy(raw + off, &v, sizeof(T)); }
};
static_assert(sizeof(Entry) == 32);

// ---- time (same packing as FAT: date << 16 | time, local = UTC) ----

u64 decode_time(u32 ts) {
    u16 date = static_cast<u16>(ts >> 16), time = static_cast<u16>(ts);
    if (!date)
        return 0;
    u32 year = 1980 + (date >> 9), month = (date >> 5) & 0xF, day = date & 0x1F;
    u32 h = time >> 11, m = (time >> 5) & 0x3F, s = (time & 0x1F) * 2;
    i64 y = year - (month <= 2);
    i64 era = (y >= 0 ? y : y - 399) / 400;
    u32 yoe = static_cast<u32>(y - era * 400);
    u32 doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    i64 days = era * 146097 + static_cast<i64>(doe) - 719468;
    return static_cast<u64>(days) * 86400 + h * 3600 + m * 60 + s;
}

u32 encode_time(i64 t) {
    i64 days = t / 86400, secs = t % 86400;
    days += 719468;
    i64 era = (days >= 0 ? days : days - 146096) / 146097;
    u32 doe = static_cast<u32>(days - era * 146097);
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = static_cast<i64>(yoe) + era * 400;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u32 mp = (5 * doy + 2) / 153;
    u32 d = doy - (153 * mp + 2) / 5 + 1;
    u32 m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    if (y < 1980)
        return (1u << 21) | (1u << 16); // 1980-01-01
    u32 date = static_cast<u32>(((y - 1980) << 9) | (m << 5) | d);
    u32 time = static_cast<u32>(((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2));
    return (date << 16) | time;
}

u32 now_stamp() { return encode_time(static_cast<i64>(rtc::realtime_ns() / 1'000'000'000)); }

// ---- names: UTF-8 <-> UTF-16 (BMP) ----

usize utf8_to_utf16(const char* s, usize len, u16* out, usize cap) {
    usize n = 0;
    for (usize i = 0; i < len && n < cap;) {
        u8 c = static_cast<u8>(s[i]);
        u32 cp;
        if (c < 0x80) { cp = c; i += 1; }
        else if ((c >> 5) == 6 && i + 1 < len) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2; }
        else if ((c >> 4) == 14 && i + 2 < len) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            i += 3;
        } else { cp = '?'; i += 1; while (i < len && (static_cast<u8>(s[i]) >> 6) == 2) i++; }
        out[n++] = static_cast<u16>(cp);
    }
    return n;
}

void utf16_to_utf8(const u16* in, usize n, char* out, usize cap) {
    usize len = 0;
    for (usize i = 0; i < n; i++) {
        u16 c = in[i];
        if (c < 0x80) {
            if (len + 1 < cap) out[len++] = static_cast<char>(c);
        } else if (c < 0x800) {
            if (len + 2 < cap) {
                out[len++] = static_cast<char>(0xC0 | (c >> 6));
                out[len++] = static_cast<char>(0x80 | (c & 0x3F));
            }
        } else if (len + 3 < cap) {
            out[len++] = static_cast<char>(0xE0 | (c >> 12));
            out[len++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out[len++] = static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    out[len] = 0;
}

u16 set_checksum(const Entry* set, usize count) {
    u16 sum = 0;
    for (usize e = 0; e < count; e++)
        for (usize i = 0; i < 32; i++) {
            if (e == 0 && (i == 2 || i == 3))
                continue;
            sum = static_cast<u16>(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + set[e].raw[i]);
        }
    return sum;
}

class Node;

class Volume {
public:
    block::Device* dev;
    u32 cluster_size;
    u64 fat_offset;
    u64 heap_offset;
    u32 cluster_count;
    u32 root_cluster;
    u64 fs_dev = vfs::next_fs_dev();
    sched::Mutex lock;
    u16* upcase = nullptr;   // 65536 entries
    u8* bitmap = nullptr;    // in-memory copy, written through
    u32* bitmap_clusters = nullptr;
    u32 bitmap_cluster_count = 0;
    u32 free_hint = 0;
    Node* nodes = nullptr;

    u64 cluster_offset(u32 c) const { return heap_offset + static_cast<u64>(c - 2) * cluster_size; }

    u32 fat_get(u32 c) {
        u32 v = 0;
        block::read(dev, fat_offset + static_cast<u64>(c) * 4, &v, 4);
        return v;
    }
    void fat_set(u32 c, u32 v) { block::write(dev, fat_offset + static_cast<u64>(c) * 4, &v, 4); }

    bool in_use(u32 c) const { u32 b = c - 2; return bitmap[b / 8] & (1u << (b % 8)); }
    void mark(u32 c, bool used) {
        u32 b = c - 2;
        if (used) bitmap[b / 8] |= static_cast<u8>(1u << (b % 8));
        else bitmap[b / 8] &= static_cast<u8>(~(1u << (b % 8)));
        u32 byte = b / 8;
        u32 bc = byte / cluster_size;
        if (bc < bitmap_cluster_count)
            block::write(dev, cluster_offset(bitmap_clusters[bc]) + byte % cluster_size, &bitmap[byte], 1);
    }

    // Allocate a zeroed cluster; chained after `prev` if nonzero.
    u32 alloc_cluster(u32 prev) {
        for (u32 n = 0; n < cluster_count; n++) {
            u32 c = 2 + (free_hint + n) % cluster_count;
            if (in_use(c))
                continue;
            mark(c, true);
            fat_set(c, EOC);
            if (prev)
                fat_set(prev, c);
            free_hint = c - 1;
            u8* zeros = static_cast<u8*>(kzalloc(cluster_size));
            if (zeros) {
                block::write(dev, cluster_offset(c), zeros, cluster_size);
                kfree(zeros);
            }
            return c;
        }
        return 0;
    }

    void free_clusters(u32 first, bool contiguous, u32 count) {
        if (contiguous) {
            for (u32 i = 0; i < count; i++)
                mark(first + i, false);
            return;
        }
        for (u32 c = first, i = 0; c >= 2 && c < BAD && i < cluster_count; i++) {
            u32 next = fat_get(c);
            mark(c, false);
            fat_set(c, 0);
            c = next;
        }
    }

    u16 up(u16 c) const { return upcase[c]; }

    u16 name_hash(const u16* name, usize n) const {
        u16 h = 0;
        for (usize i = 0; i < n; i++) {
            u16 c = up(name[i]);
            h = static_cast<u16>(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
            h = static_cast<u16>(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
        }
        return h;
    }

    bool names_equal(const u16* a, usize an, const u16* b, usize bn) const {
        if (an != bn)
            return false;
        for (usize i = 0; i < an; i++)
            if (up(a[i]) != up(b[i]))
                return false;
        return true;
    }
};

// What a directory entry set says about a file.
struct SetInfo {
    u32 index;     // position of the File entry in the directory (in entries)
    u32 count;     // entries in the set
    u16 attr;
    u32 first_cluster;
    u64 data_length, valid_length;
    bool contiguous;
    u32 mtime;
    u16 name[255];
    usize name_len;
};

class Node : public vfs::Inode {
public:
    Volume* vol;
    Node* parent_dir;  // nullptr for the root
    u32 entry_index;   // where our set starts in the parent
    u32 entry_count;
    bool is_directory;
    u32 first_cluster;
    bool contiguous;
    u64 valid_length;
    u16 attributes;
    Node* next_cached = nullptr;

    Node(Volume* v, Node* p, u32 index, u32 count, const SetInfo* info)
        : vol(v), parent_dir(p), entry_index(index), entry_count(count) {
        ino = vfs::next_ino();
        dev = v->fs_dev;
        if (info) {
            is_directory = info->attr & ATTR_DIR;
            first_cluster = info->first_cluster;
            contiguous = info->contiguous;
            size = info->data_length;
            valid_length = info->valid_length;
            attributes = info->attr;
            u64 t = decode_time(info->mtime);
            mtime = ctime = atime = vfs::Timespec{static_cast<i64>(t), 0};
        } else { // root
            is_directory = true;
            first_cluster = v->root_cluster;
            contiguous = false;
            attributes = ATTR_DIR;
            size = 0;
            for (u32 c = first_cluster; c >= 2 && c < BAD; c = v->fat_get(c))
                size += v->cluster_size;
            valid_length = size;
            mtime = ctime = atime = vfs::now();
        }
        mode = is_directory ? (vfs::S_IFDIR | 0755) : (vfs::S_IFREG | 0755);
        if (attributes & ATTR_READONLY)
            mode &= ~0222u;
        nlink = is_directory ? 2 : 1;
        parent = p;
    }

    u32 allocated_clusters() const { return static_cast<u32>((size + vol->cluster_size - 1) / vol->cluster_size); }

    // Turn a contiguous allocation into an explicit FAT chain.
    void make_chain() {
        if (!contiguous)
            return;
        u32 n = allocated_clusters();
        for (u32 i = 0; i < n; i++)
            vol->fat_set(first_cluster + i, i + 1 < n ? first_cluster + i + 1 : EOC);
        contiguous = false;
    }

    // Cluster holding byte `offset` (allocating when `extend`).
    u32 cluster_at(u64 offset, bool extend) {
        u32 index = static_cast<u32>(offset / vol->cluster_size);
        if (contiguous && first_cluster) {
            if (index < allocated_clusters())
                return first_cluster + index;
            if (!extend)
                return 0;
            make_chain();
        }
        if (!first_cluster) {
            if (!extend)
                return 0;
            first_cluster = vol->alloc_cluster(0);
            if (!first_cluster)
                return 0;
            contiguous = false;
        }
        u32 c = first_cluster;
        for (u32 i = 0; i < index; i++) {
            u32 next = vol->fat_get(c);
            if (next < 2 || next >= BAD) {
                if (!extend)
                    return 0;
                next = vol->alloc_cluster(c);
                if (!next)
                    return 0;
            }
            c = next;
        }
        return c;
    }

    // ---- raw directory entries (by index) ----

    bool entry_offset(u32 index, u64* out) {
        u64 byte = static_cast<u64>(index) * 32;
        if (byte >= size)
            return false;
        u32 c = cluster_at(byte, false);
        if (!c)
            return false;
        *out = vol->cluster_offset(c) + byte % vol->cluster_size;
        return true;
    }
    bool read_entry(u32 index, Entry* e) {
        u64 off;
        return entry_offset(index, &off) && block::read(vol->dev, off, e, 32) == 0;
    }
    bool write_entry(u32 index, const Entry& e) {
        u64 off;
        return entry_offset(index, &off) && block::write(vol->dev, off, &e, 32) == 0;
    }
    u32 entry_capacity() const { return static_cast<u32>(size / 32); }

    // Calls fn(info) for each file set; stops when fn returns true.
    template <typename Fn>
    bool iterate(Fn fn) {
        u32 cap = entry_capacity();
        for (u32 i = 0; i < cap;) {
            Entry e;
            if (!read_entry(i, &e) || e.type() == 0x00)
                return false;
            if (e.type() != TYPE_FILE) {
                i++;
                continue;
            }
            u32 count = 1u + e.raw[1];
            if (count < 3 || count > MAX_SET || i + count > cap) {
                i++;
                continue;
            }
            Entry set[MAX_SET];
            set[0] = e;
            bool ok = true;
            for (u32 k = 1; k < count && ok; k++)
                ok = read_entry(i + k, &set[k]);
            if (!ok || set[1].type() != TYPE_STREAM ||
                set_checksum(set, count) != set[0].get<u16>(2)) {
                i++;
                continue;
            }
            SetInfo info{};
            parse_set(set, count, i, &info);
            if (fn(info))
                return true;
            i += count;
        }
        return false;
    }

    static void parse_set(const Entry* set, u32 count, u32 index, SetInfo* info) {
        info->index = index;
        info->count = count;
        info->attr = set[0].get<u16>(4);
        info->mtime = set[0].get<u32>(12);
        info->contiguous = set[1].raw[1] & FLAG_NO_FAT_CHAIN;
        info->first_cluster = set[1].get<u32>(20);
        info->valid_length = set[1].get<u64>(8);
        info->data_length = set[1].get<u64>(24);
        usize len = set[1].raw[3], n = 0;
        for (u32 k = 2; k < count && n < len; k++) {
            if (set[k].type() != TYPE_NAME)
                break;
            for (int j = 0; j < 15 && n < len; j++)
                info->name[n++] = set[k].get<u16>(2 + j * 2);
        }
        info->name_len = n;
    }

    bool find_set(const char* name, usize len, SetInfo* out) {
        u16 wanted[255];
        usize wn = utf8_to_utf16(name, len, wanted, 255);
        return iterate([&](const SetInfo& info) {
            if (!vol->names_equal(info.name, info.name_len, wanted, wn))
                return false;
            *out = info;
            return true;
        });
    }

    // Rewrite our stream/file entries (size, cluster, flags, time).
    void update_set() {
        if (!parent_dir)
            return; // the root has no entry set
        Entry set[MAX_SET];
        for (u32 k = 0; k < entry_count; k++)
            if (!parent_dir->read_entry(entry_index + k, &set[k]))
                return;
        if (set[0].type() != TYPE_FILE || set[1].type() != TYPE_STREAM)
            return;
        u16 attr = set[0].get<u16>(4);
        if (!is_directory)
            attr |= ATTR_ARCHIVE;
        set[0].set<u16>(4, attr);
        set[0].set<u32>(12, encode_time(mtime.sec));
        set[0].set<u32>(16, encode_time(atime.sec));
        set[0].raw[21] = 0;
        set[0].raw[23] = 0x80; // UTC offset valid, +0
        set[0].raw[24] = 0x80;
        set[1].raw[1] = static_cast<u8>(FLAG_ALLOC_POSSIBLE | (contiguous && first_cluster ? FLAG_NO_FAT_CHAIN : 0));
        set[1].set<u32>(20, first_cluster);
        set[1].set<u64>(24, size);
        set[1].set<u64>(8, is_directory ? size : valid_length);
        set[0].set<u16>(2, set_checksum(set, entry_count));
        for (u32 k = 0; k < entry_count; k++)
            parent_dir->write_entry(entry_index + k, set[k]);
    }

    vfs::Inode* lookup(const char* name, usize len) override {
        if (!is_directory)
            return nullptr;
        LockGuard guard(vol->lock);
        return find_locked(name, len);
    }

    Node* find_locked(const char* name, usize len) {
        SetInfo info;
        if (!find_set(name, len, &info))
            return nullptr;
        return node_from(info);
    }

    Node* node_from(const SetInfo& info) {
        for (Node* n = vol->nodes; n; n = n->next_cached)
            if (n->parent_dir == this && n->entry_index == info.index)
                return n;
        auto* n = new Node(vol, this, info.index, info.count, &info);
        n->next_cached = vol->nodes;
        vol->nodes = n;
        return n;
    }

    long readdir(u64 index, vfs::DirEntry* out) override {
        if (!is_directory)
            return -err::NOTDIR;
        if (index < 2) {
            out->ino = index == 0 ? ino : (parent ? parent->ino : ino);
            out->type = vfs::DT_DIR;
            snprintf(out->name, sizeof(out->name), "%s", index == 0 ? "." : "..");
            return 1;
        }
        LockGuard guard(vol->lock);
        u64 i = 2;
        bool found = iterate([&](const SetInfo& info) {
            if (i++ != index)
                return false;
            out->ino = (static_cast<u64>(first_cluster) << 20) ^ info.index ^ 0x1000000000ull;
            out->type = (info.attr & ATTR_DIR) ? vfs::DT_DIR : vfs::DT_REG;
            utf16_to_utf8(info.name, info.name_len, out->name, sizeof(out->name));
            return true;
        });
        return found ? 1 : 0;
    }

    isize read(u64 offset, void* buf, usize count) override {
        if (is_directory)
            return -err::ISDIR;
        LockGuard guard(vol->lock);
        if (offset >= size)
            return 0;
        count = min<u64>(count, size - offset);
        auto* out = static_cast<u8*>(buf);
        usize done = 0;
        while (done < count) {
            u64 pos = offset + done;
            u32 within = static_cast<u32>(pos % vol->cluster_size);
            usize chunk = min<usize>(count - done, vol->cluster_size - within);
            if (pos >= valid_length) { // beyond the valid data: zeros by definition
                memset(out + done, 0, chunk);
            } else {
                chunk = min<u64>(chunk, valid_length - pos);
                u32 c = cluster_at(pos, false);
                if (!c || block::read(vol->dev, vol->cluster_offset(c) + within, out + done, chunk) < 0)
                    return done ? static_cast<isize>(done) : -err::IO;
            }
            done += chunk;
        }
        return static_cast<isize>(done);
    }

    // Write bytes at `offset` (no locking, no size bookkeeping).
    usize write_raw(u64 offset, const u8* in, usize count) {
        usize done = 0;
        while (done < count) {
            u32 c = cluster_at(offset + done, true);
            if (!c)
                break;
            u32 within = static_cast<u32>((offset + done) % vol->cluster_size);
            usize chunk = min<usize>(count - done, vol->cluster_size - within);
            if (block::write(vol->dev, vol->cluster_offset(c) + within, in ? in + done : nullptr, chunk) < 0)
                break;
            done += chunk;
        }
        return done;
    }

    // Zero [from, to) of the file (fills the gap when writing past the valid length).
    void zero_range(u64 from, u64 to) {
        static u8 zeros[4096];
        while (from < to) {
            usize n = min<u64>(to - from, sizeof(zeros));
            if (write_raw(from, zeros, n) != n)
                return;
            from += n;
        }
    }

    isize write(u64 offset, const void* buf, usize count) override {
        if (is_directory)
            return -err::ISDIR;
        LockGuard guard(vol->lock);
        if (offset > valid_length)
            zero_range(valid_length, offset);
        usize done = write_raw(offset, static_cast<const u8*>(buf), count);
        if (offset + done > size)
            size = offset + done;
        if (offset + done > valid_length)
            valid_length = offset + done;
        mtime = ctime = vfs::now();
        update_set();
        if (done == 0 && count)
            return -err::NOSPC;
        return static_cast<isize>(done);
    }

    long truncate(u64 new_size) override {
        if (is_directory)
            return -err::ISDIR;
        LockGuard guard(vol->lock);
        if (new_size == 0) {
            vol->free_clusters(first_cluster, contiguous, allocated_clusters());
            first_cluster = 0;
            contiguous = false;
            valid_length = 0;
        } else if (new_size < size) {
            u32 keep = static_cast<u32>((new_size + vol->cluster_size - 1) / vol->cluster_size);
            if (contiguous) {
                for (u32 i = keep; i < allocated_clusters(); i++)
                    vol->mark(first_cluster + i, false);
            } else {
                u32 last = cluster_at(new_size - 1, false);
                if (last) {
                    u32 rest = vol->fat_get(last);
                    vol->fat_set(last, EOC);
                    vol->free_clusters(rest, false, 0);
                }
            }
            if (valid_length > new_size)
                valid_length = new_size;
        } else if (new_size > size) {
            if (!cluster_at(new_size - 1, true))
                return -err::NOSPC;
            // valid_length stays: the new tail reads as zeros.
        }
        size = new_size;
        mtime = ctime = vfs::now();
        update_set();
        return 0;
    }

    long set_times(vfs::Timespec a, vfs::Timespec m) override {
        LockGuard guard(vol->lock);
        atime = a;
        mtime = m;
        update_set();
        return 0;
    }

    long statfs(FsStats* out) override {
        LockGuard guard(vol->lock);
        u64 free = 0;
        for (u32 c = 2; c < vol->cluster_count + 2; c++)
            free += !vol->in_use(c);
        *out = FsStats{0x2011BAB0 /* EXFAT_SUPER_MAGIC */, vol->cluster_size, vol->cluster_count, free, 0, 0};
        return 0;
    }

    // `count` consecutive free entry slots (growing the directory if needed).
    bool find_free(u32 count, u32* out) {
        u32 cap = entry_capacity(), run = 0;
        for (u32 i = 0; i < cap; i++) {
            Entry e;
            if (!read_entry(i, &e))
                return false;
            if (e.type() == 0x00 || !(e.type() & 0x80)) {
                if (++run == count) {
                    *out = i + 1 - count;
                    return true;
                }
            } else {
                run = 0;
            }
        }
        // Grow by clusters until the run fits (new clusters are zeroed = free).
        u32 start = cap - run;
        while (entry_capacity() < start + count) {
            if (!cluster_at(size, true))
                return false;
            size += vol->cluster_size;
            valid_length = size;
        }
        update_set();
        *out = start;
        return true;
    }

    // Build and write a directory entry set for `name` at a free spot.
    long write_new_set(const u16* name, usize n, u16 attr, u32 cluster, bool contig, u64 length, u64 valid,
                       u32 stamp, u32* index_out, u32* count_out) {
        u32 names = static_cast<u32>((n + 14) / 15);
        u32 count = 2 + names;
        u32 index;
        if (!find_free(count, &index))
            return -err::NOSPC;
        Entry set[MAX_SET] = {};
        set[0].raw[0] = TYPE_FILE;
        set[0].raw[1] = static_cast<u8>(count - 1);
        set[0].set<u16>(4, attr);
        set[0].set<u32>(8, stamp);
        set[0].set<u32>(12, stamp);
        set[0].set<u32>(16, stamp);
        set[0].raw[22] = set[0].raw[23] = set[0].raw[24] = 0x80;
        set[1].raw[0] = TYPE_STREAM;
        set[1].raw[1] = static_cast<u8>(FLAG_ALLOC_POSSIBLE | (contig && cluster ? FLAG_NO_FAT_CHAIN : 0));
        set[1].raw[3] = static_cast<u8>(n);
        set[1].set<u16>(4, vol->name_hash(name, n));
        set[1].set<u64>(8, valid);
        set[1].set<u32>(20, cluster);
        set[1].set<u64>(24, length);
        for (u32 k = 0; k < names; k++) {
            set[2 + k].raw[0] = TYPE_NAME;
            for (u32 j = 0; j < 15; j++) {
                usize pos = k * 15 + j;
                set[2 + k].set<u16>(2 + j * 2, pos < n ? name[pos] : 0);
            }
        }
        set[0].set<u16>(2, set_checksum(set, count));
        // Secondary entries first, the File entry last: a crash leaves no half-made file.
        for (u32 k = count; k-- > 0;)
            if (!write_entry(index + k, set[k]))
                return -err::IO;
        *index_out = index;
        *count_out = count;
        return 0;
    }

    void delete_set(u32 index, u32 count) {
        for (u32 k = 0; k < count; k++) {
            Entry e;
            if (read_entry(index + k, &e)) {
                e.raw[0] &= 0x7F; // clear "in use"
                write_entry(index + k, e);
            }
        }
    }

    void forget(Node* node) {
        Node** link = &vol->nodes;
        while (*link && *link != node)
            link = &(*link)->next_cached;
        if (*link)
            *link = node->next_cached;
    }

    long create(const char* name, usize len, u32 new_mode, const char*, vfs::Inode** out) override {
        if (!is_directory)
            return -err::NOTDIR;
        u32 type = new_mode & vfs::S_IFMT;
        if (type != 0 && type != vfs::S_IFREG && type != vfs::S_IFDIR)
            return -err::OPNOTSUPP;
        u16 wname[255];
        usize n = utf8_to_utf16(name, len, wname, 255);
        if (len > 255 * 3 || n == 0)
            return -err::NAMETOOLONG;
        LockGuard guard(vol->lock);
        if (find_locked(name, len))
            return -err::EXIST;
        bool dir = type == vfs::S_IFDIR;
        u32 cluster = 0;
        u64 length = 0;
        if (dir) { // directories always own at least one (zeroed) cluster
            cluster = vol->alloc_cluster(0);
            if (!cluster)
                return -err::NOSPC;
            length = vol->cluster_size;
        }
        u32 index, count;
        if (long r = write_new_set(wname, n, dir ? ATTR_DIR : ATTR_ARCHIVE, cluster, false, length, length, now_stamp(),
                                   &index, &count);
            r < 0) {
            if (cluster)
                vol->free_clusters(cluster, false, 1);
            return r;
        }
        SetInfo info{};
        info.index = index;
        info.count = count;
        info.attr = dir ? ATTR_DIR : ATTR_ARCHIVE;
        info.first_cluster = cluster;
        info.data_length = info.valid_length = length;
        info.mtime = now_stamp();
        Node* node = node_from(info);
        if (out)
            *out = node;
        return 0;
    }

    long unlink(const char* name, usize len, bool remove_dir) override {
        if (!is_directory)
            return -err::NOTDIR;
        LockGuard guard(vol->lock);
        SetInfo info;
        if (!find_set(name, len, &info))
            return -err::NOENT;
        bool dir = info.attr & ATTR_DIR;
        if (remove_dir && !dir)
            return -err::NOTDIR;
        if (!remove_dir && dir)
            return -err::ISDIR;
        Node* node = node_from(info);
        if (dir && node->iterate([](const SetInfo&) { return true; }))
            return -err::NOTEMPTY;
        delete_set(info.index, info.count);
        vol->free_clusters(node->first_cluster, node->contiguous, node->allocated_clusters());
        node->first_cluster = 0;
        node->nlink = 0;
        node->parent_dir = nullptr; // detached
        forget(node);
        return 0;
    }

    long rename(const char* old_name, usize old_len, vfs::Inode* new_dir_inode, const char* new_name,
                usize new_len) override {
        auto* target = static_cast<Node*>(new_dir_inode);
        if (!is_directory || target->vol != vol)
            return -err::XDEV;
        u16 wname[255];
        usize n = utf8_to_utf16(new_name, new_len, wname, 255);
        if (!n)
            return -err::INVAL;
        LockGuard guard(vol->lock);
        SetInfo info;
        if (!find_set(old_name, old_len, &info))
            return -err::NOENT;
        Node* node = node_from(info);
        // Moving a directory into itself or its own subtree is not allowed.
        for (Node* p = target; p; p = p->parent_dir)
            if (p == node)
                return -err::INVAL;
        SetInfo existing;
        if (target->find_set(new_name, new_len, &existing)) {
            if (existing.index == info.index && target == this) { // same entry (case change)
            } else {
                bool edir = existing.attr & ATTR_DIR;
                if (edir != node->is_directory)
                    return edir ? -err::ISDIR : -err::NOTDIR;
                Node* victim = target->node_from(existing);
                if (edir && victim->iterate([](const SetInfo&) { return true; }))
                    return -err::NOTEMPTY;
                target->delete_set(existing.index, existing.count);
                vol->free_clusters(victim->first_cluster, victim->contiguous, victim->allocated_clusters());
                victim->first_cluster = 0;
                victim->nlink = 0;
                victim->parent_dir = nullptr;
                forget(victim);
            }
        }
        // Remove the old set first so a case-only rename in place finds room.
        delete_set(info.index, info.count);
        u32 index, count;
        if (long r = target->write_new_set(wname, n, info.attr, node->first_cluster, node->contiguous, node->size,
                                           node->is_directory ? node->size : node->valid_length, info.mtime, &index,
                                           &count);
            r < 0) {
            // Put the old set back (its entries are only marked unused).
            for (u32 k = 0; k < info.count; k++) {
                Entry e;
                if (read_entry(info.index + k, &e)) {
                    e.raw[0] |= 0x80;
                    write_entry(info.index + k, e);
                }
            }
            return r;
        }
        node->parent_dir = target;
        node->parent = target;
        node->entry_index = index;
        node->entry_count = count;
        return 0;
    }
};

} // namespace

bool probe(block::Device* dev) {
    u8 bs[512];
    if (dev->size_bytes() < 1024 * 1024 || block::read(dev, 0, bs, 512) != 0)
        return false;
    return memcmp(bs + 3, "EXFAT   ", 8) == 0 && bs[510] == 0x55 && bs[511] == 0xAA;
}

long mount(block::Device* dev, vfs::Inode** root) {
    if (!probe(dev))
        return -err::INVAL;
    u8 bs[512];
    block::read(dev, 0, bs, 512);
    u32 fat_off, heap_off, clusters, root_cluster;
    memcpy(&fat_off, bs + 80, 4);
    memcpy(&heap_off, bs + 88, 4);
    memcpy(&clusters, bs + 92, 4);
    memcpy(&root_cluster, bs + 96, 4);
    u8 sector_shift = bs[108], cluster_shift = bs[109];
    if (sector_shift < 9 || sector_shift > 12 || sector_shift + cluster_shift > 25 || clusters < 1)
        return -err::INVAL;

    auto* v = new Volume;
    v->dev = dev;
    v->cluster_size = 1u << (sector_shift + cluster_shift);
    v->fat_offset = static_cast<u64>(fat_off) << sector_shift;
    v->heap_offset = static_cast<u64>(heap_off) << sector_shift;
    v->cluster_count = clusters;
    v->root_cluster = root_cluster;

    auto* r = new Node(v, nullptr, 0, 0, nullptr);

    // Find the allocation bitmap and the up-case table in the root directory.
    u32 bitmap_cluster = 0, upcase_cluster = 0;
    u64 bitmap_len = 0, upcase_len = 0;
    for (u32 i = 0; i < r->entry_capacity(); i++) {
        Entry e;
        if (!r->read_entry(i, &e) || e.type() == 0)
            break;
        if (e.type() == TYPE_BITMAP && !(e.raw[1] & 1)) { // first bitmap (TexFAT has two)
            bitmap_cluster = e.get<u32>(20);
            bitmap_len = e.get<u64>(24);
        } else if (e.type() == TYPE_UPCASE) {
            upcase_cluster = e.get<u32>(20);
            upcase_len = e.get<u64>(24);
        }
    }
    if (!bitmap_cluster || bitmap_len < (clusters + 7) / 8) {
        klog::warn("exfat: %s: no allocation bitmap", dev->name);
        return -err::INVAL;
    }

    // Bitmap: load into memory, remembering its clusters for write-through.
    v->bitmap = static_cast<u8*>(kzalloc(bitmap_len));
    v->bitmap_cluster_count = static_cast<u32>((bitmap_len + v->cluster_size - 1) / v->cluster_size);
    v->bitmap_clusters = static_cast<u32*>(kmalloc(v->bitmap_cluster_count * sizeof(u32)));
    if (!v->bitmap || !v->bitmap_clusters)
        return -err::NOMEM;
    for (u32 i = 0, c = bitmap_cluster; i < v->bitmap_cluster_count; i++) {
        v->bitmap_clusters[i] = c;
        u64 chunk = min<u64>(v->cluster_size, bitmap_len - static_cast<u64>(i) * v->cluster_size);
        block::read(dev, v->cluster_offset(c), v->bitmap + static_cast<u64>(i) * v->cluster_size, chunk);
        u32 next = v->fat_get(c);
        c = next >= 2 && next < BAD ? next : c + 1; // chain, or contiguous
    }

    // Up-case table (compressed: 0xFFFF n = n identity mappings).
    v->upcase = static_cast<u16*>(kmalloc(65536 * sizeof(u16)));
    if (!v->upcase)
        return -err::NOMEM;
    for (u32 i = 0; i < 65536; i++)
        v->upcase[i] = static_cast<u16>(i < 128 && i >= 'a' && i <= 'z' ? i - 32 : i);
    if (upcase_cluster && upcase_len && upcase_len <= 256 * 1024) {
        u16* table = static_cast<u16*>(kmalloc(upcase_len));
        if (table) {
            u64 done = 0;
            for (u32 c = upcase_cluster; done < upcase_len;) {
                u64 chunk = min<u64>(v->cluster_size, upcase_len - done);
                block::read(dev, v->cluster_offset(c), reinterpret_cast<u8*>(table) + done, chunk);
                done += chunk;
                u32 next = v->fat_get(c);
                c = next >= 2 && next < BAD ? next : c + 1;
            }
            u32 ch = 0;
            for (u64 i = 0; i < upcase_len / 2 && ch < 65536; i++) {
                if (table[i] == 0xFFFF && i + 1 < upcase_len / 2) {
                    ch += table[++i];
                } else {
                    v->upcase[ch++] = table[i];
                }
            }
            kfree(table);
        }
    }

    *root = r;
    klog::ok("exFAT on %s: %u clusters of %u bytes (%lu MiB)", dev->name, clusters, v->cluster_size,
             static_cast<u64>(clusters) * v->cluster_size / MiB);
    return 0;
}

} // namespace exfat
