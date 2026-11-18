#include <core/log.hpp>
#include <fs/fat.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <sched/sync.hpp>
#include <time/rtc.hpp>

namespace fat {

namespace {

constexpr u32 FAT_EOC = 0x0FFFFFF8; // >= this: end of chain
constexpr u32 FAT_MASK = 0x0FFFFFFF;
constexpr u8 ATTR_READONLY = 0x01, ATTR_VOLUME = 0x08, ATTR_DIR = 0x10, ATTR_ARCHIVE = 0x20, ATTR_LFN = 0x0F;

struct PACKED DirEntry {
    char name[11];
    u8 attr;
    u8 ntres;      // bit 3: base name lowercase, bit 4: extension lowercase
    u8 crt_tenth;
    u16 crt_time, crt_date, acc_date;
    u16 cluster_hi;
    u16 mod_time, mod_date;
    u16 cluster_lo;
    u32 size;
};
static_assert(sizeof(DirEntry) == 32);

struct PACKED LfnEntry {
    u8 order;
    u16 name1[5];
    u8 attr;
    u8 type;
    u8 checksum;
    u16 name2[6];
    u16 cluster;
    u16 name3[2];
};
static_assert(sizeof(LfnEntry) == 32);

class Node;

class Volume {
public:
    block::Device* dev;
    u32 cluster_size;
    u64 fat_start;   // byte offset of the first FAT
    u64 fat_bytes;   // size of one FAT
    u32 fat_count;
    u64 data_start;  // byte offset of cluster 2
    u32 cluster_count;
    u32 root_cluster;
    u32 free_hint = 2;
    u64 fs_dev = vfs::next_fs_dev();
    sched::Mutex lock; // one operation at a time per volume
    Node* nodes = nullptr; // node cache

    u64 cluster_offset(u32 c) const { return data_start + static_cast<u64>(c - 2) * cluster_size; }

    u32 fat_get(u32 c) {
        u32 v = 0;
        block::read(dev, fat_start + static_cast<u64>(c) * 4, &v, 4);
        return v & FAT_MASK;
    }
    void fat_set(u32 c, u32 v) {
        for (u32 i = 0; i < fat_count; i++) {
            u64 off = fat_start + i * fat_bytes + static_cast<u64>(c) * 4;
            u32 old = 0;
            block::read(dev, off, &old, 4);
            v = (v & FAT_MASK) | (old & ~FAT_MASK); // top 4 bits are reserved
            block::write(dev, off, &v, 4);
        }
    }

    // Allocate a zeroed cluster, linked after `prev` (0 = start a new chain).
    u32 alloc_cluster(u32 prev) {
        for (u32 n = 0; n < cluster_count; n++) {
            u32 c = 2 + (free_hint - 2 + n) % cluster_count;
            if (fat_get(c) != 0)
                continue;
            fat_set(c, FAT_MASK);
            if (prev)
                fat_set(prev, c);
            free_hint = c + 1;
            u8* zeros = static_cast<u8*>(kzalloc(cluster_size));
            if (zeros) {
                block::write(dev, cluster_offset(c), zeros, cluster_size);
                kfree(zeros);
            }
            return c;
        }
        return 0; // disk full
    }

    void free_chain(u32 c) {
        while (c >= 2 && c < FAT_EOC) {
            u32 next = fat_get(c);
            fat_set(c, 0);
            c = next;
        }
    }

    Node* node_for(u64 dirent_offset, const DirEntry& e, Node* parent);
};

// Encode/decode FAT timestamps (local time = UTC here).
u64 decode_time(u16 date, u16 time) {
    if (!date)
        return 0;
    u32 year = 1980 + (date >> 9), month = (date >> 5) & 0xF, day = date & 0x1F;
    u32 h = time >> 11, m = (time >> 5) & 0x3F, s = (time & 0x1F) * 2;
    // days from civil (same algorithm as the RTC driver)
    i64 y = year - (month <= 2);
    i64 era = (y >= 0 ? y : y - 399) / 400;
    u32 yoe = static_cast<u32>(y - era * 400);
    u32 doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    i64 days = era * 146097 + static_cast<i64>(doe) - 719468;
    return static_cast<u64>(days) * 86400 + h * 3600 + m * 60 + s;
}

void encode_now(u16* date, u16* time) {
    i64 t = static_cast<i64>(rtc::realtime_ns() / 1'000'000'000);
    i64 days = t / 86400, secs = t % 86400;
    // civil from days
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
    *date = static_cast<u16>(((y - 1980) << 9) | (m << 5) | d);
    *time = static_cast<u16>(((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2));
}

u8 lfn_checksum(const char* short_name) {
    u8 sum = 0;
    for (int i = 0; i < 11; i++)
        sum = static_cast<u8>(((sum & 1) << 7) + (sum >> 1) + static_cast<u8>(short_name[i]));
    return sum;
}

bool equal_nocase(const char* a, usize alen, const char* b) {
    if (strlen(b) != alen)
        return false;
    for (usize i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y)
            return false;
    }
    return true;
}

// UTF-16 code unit -> UTF-8 (BMP only; FAT names are UCS-2 in practice).
void put_utf8(char* out, usize* len, usize cap, u16 c) {
    if (c < 0x80) {
        if (*len + 1 < cap) out[(*len)++] = static_cast<char>(c);
    } else if (c < 0x800) {
        if (*len + 2 < cap) {
            out[(*len)++] = static_cast<char>(0xC0 | (c >> 6));
            out[(*len)++] = static_cast<char>(0x80 | (c & 0x3F));
        }
    } else if (*len + 3 < cap) {
        out[(*len)++] = static_cast<char>(0xE0 | (c >> 12));
        out[(*len)++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out[(*len)++] = static_cast<char>(0x80 | (c & 0x3F));
    }
}

void short_to_name(const DirEntry& e, char* out) {
    usize n = 0;
    for (int i = 0; i < 8 && e.name[i] != ' '; i++) {
        char c = e.name[i];
        if ((e.ntres & 0x08) && c >= 'A' && c <= 'Z') c += 32;
        out[n++] = c;
    }
    if (out[0] == 0x05) out[0] = static_cast<char>(0xE5); // escaped first byte
    if (e.name[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e.name[i] != ' '; i++) {
            char c = e.name[i];
            if ((e.ntres & 0x10) && c >= 'A' && c <= 'Z') c += 32;
            out[n++] = c;
        }
    }
    out[n] = 0;
}

class Node : public vfs::Inode {
public:
    Volume* vol;
    u32 first_cluster;
    u64 dirent_offset; // where our short entry lives (0 for the root)
    bool is_directory;
    Node* next_cached = nullptr;

    Node(Volume* v, u32 cluster, u64 dirent, bool dir, u32 file_size, u64 mtime_s)
        : vol(v), first_cluster(cluster), dirent_offset(dirent), is_directory(dir) {
        ino = vfs::next_ino();
        dev = v->fs_dev;
        mode = dir ? (vfs::S_IFDIR | 0755) : (vfs::S_IFREG | 0755);
        nlink = dir ? 2 : 1;
        size = file_size;
        mtime = ctime = atime = vfs::Timespec{static_cast<i64>(mtime_s), 0};
    }

    // ---- directory iteration ----
    // Calls fn(name, entry, entry_offset, first_slot_offset, slots) for each
    // live entry; stops when fn returns true. Returns whether it stopped.
    template <typename Fn>
    bool iterate(Fn fn) {
        u16 lfn[260];
        int lfn_slots = 0;
        u8 lfn_sum = 0;
        u64 lfn_first = 0;
        for (u32 c = first_cluster; c >= 2 && c < FAT_EOC; c = vol->fat_get(c)) {
            u64 base = vol->cluster_offset(c);
            for (u32 off = 0; off < vol->cluster_size; off += 32) {
                DirEntry e;
                block::read(vol->dev, base + off, &e, 32);
                u8 first = static_cast<u8>(e.name[0]);
                if (first == 0x00)
                    return false; // end of directory
                if (first == 0xE5) {
                    lfn_slots = 0;
                    continue;
                }
                if (e.attr == ATTR_LFN) {
                    auto& l = reinterpret_cast<LfnEntry&>(e);
                    int seq = l.order & 0x1F;
                    if (l.order & 0x40) {
                        lfn_slots = seq;
                        lfn_sum = l.checksum;
                        lfn_first = base + off;
                        for (int i = 0; i < 260; i++) lfn[i] = 0xFFFF;
                    }
                    if (seq < 1 || seq > 20 || l.checksum != lfn_sum) {
                        lfn_slots = 0;
                        continue;
                    }
                    u16* dst = lfn + (seq - 1) * 13;
                    for (int i = 0; i < 5; i++) dst[i] = l.name1[i];
                    for (int i = 0; i < 6; i++) dst[5 + i] = l.name2[i];
                    for (int i = 0; i < 2; i++) dst[11 + i] = l.name3[i];
                    continue;
                }
                if (e.attr & ATTR_VOLUME) {
                    lfn_slots = 0;
                    continue;
                }
                char name[256];
                u64 first_slot = base + off;
                int slots = 1;
                if (lfn_slots && lfn_sum == lfn_checksum(e.name)) {
                    usize n = 0;
                    for (int i = 0; i < lfn_slots * 13 && lfn[i] && lfn[i] != 0xFFFF; i++)
                        put_utf8(name, &n, sizeof(name), lfn[i]);
                    name[n] = 0;
                    first_slot = lfn_first;
                    slots = lfn_slots + 1;
                } else {
                    short_to_name(e, name);
                }
                lfn_slots = 0;
                if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                    continue;
                if (fn(name, e, base + off, first_slot, slots))
                    return true;
            }
        }
        return false;
    }

    vfs::Inode* lookup(const char* name, usize len) override {
        if (!is_directory)
            return nullptr;
        LockGuard guard(vol->lock);
        return find_locked(name, len);
    }

    Node* find_locked(const char* name, usize len) {
        Node* result = nullptr;
        iterate([&](const char* n, const DirEntry& e, u64 off, u64, int) {
            if (!equal_nocase(name, len, n))
                return false;
            result = vol->node_for(off, e, this);
            return true;
        });
        return result;
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
        bool found = iterate([&](const char* n, const DirEntry& e, u64 off, u64, int) {
            if (i++ != index)
                return false;
            out->ino = off; // stable per entry
            out->type = (e.attr & ATTR_DIR) ? vfs::DT_DIR : vfs::DT_REG;
            snprintf(out->name, sizeof(out->name), "%s", n);
            return true;
        });
        return found ? 1 : 0;
    }

    // Cluster holding byte `offset` of this file (allocating when `extend`).
    u32 cluster_at(u64 offset, bool extend) {
        u32 index = static_cast<u32>(offset / vol->cluster_size);
        if (!first_cluster) {
            if (!extend)
                return 0;
            first_cluster = vol->alloc_cluster(0);
            if (!first_cluster)
                return 0;
            update_dirent();
        }
        u32 c = first_cluster;
        for (u32 i = 0; i < index; i++) {
            u32 next = vol->fat_get(c);
            if (next >= FAT_EOC || next < 2) {
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

    void update_dirent() {
        if (!dirent_offset)
            return; // root directory has no entry
        DirEntry e;
        block::read(vol->dev, dirent_offset, &e, 32);
        e.cluster_hi = static_cast<u16>(first_cluster >> 16);
        e.cluster_lo = static_cast<u16>(first_cluster);
        if (!is_directory)
            e.size = static_cast<u32>(size);
        u16 d, t;
        encode_now(&d, &t);
        e.mod_date = d;
        e.mod_time = t;
        e.attr |= is_directory ? 0 : ATTR_ARCHIVE;
        block::write(vol->dev, dirent_offset, &e, 32);
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
            u32 c = cluster_at(offset + done, false);
            if (!c)
                break;
            u32 in = static_cast<u32>((offset + done) % vol->cluster_size);
            usize chunk = min<usize>(count - done, vol->cluster_size - in);
            if (block::read(vol->dev, vol->cluster_offset(c) + in, out + done, chunk) < 0)
                return done ? static_cast<isize>(done) : -err::IO;
            done += chunk;
        }
        return static_cast<isize>(done);
    }

    isize write(u64 offset, const void* buf, usize count) override {
        if (is_directory)
            return -err::ISDIR;
        if (offset + count > 0xFFFFFFFFull)
            return -err::FBIG; // FAT32's 4 GiB file size limit
        LockGuard guard(vol->lock);
        auto* in = static_cast<const u8*>(buf);
        usize done = 0;
        while (done < count) {
            u32 c = cluster_at(offset + done, true);
            if (!c)
                break; // disk full
            u32 within = static_cast<u32>((offset + done) % vol->cluster_size);
            usize chunk = min<usize>(count - done, vol->cluster_size - within);
            if (block::write(vol->dev, vol->cluster_offset(c) + within, in + done, chunk) < 0)
                break;
            done += chunk;
        }
        if (offset + done > size)
            size = offset + done;
        mtime = ctime = vfs::now();
        update_dirent();
        if (done == 0 && count)
            return -err::NOSPC;
        return static_cast<isize>(done);
    }

    long statfs(FsStats* out) override {
        LockGuard guard(vol->lock);
        u64 free = 0;
        for (u32 c = 2; c < vol->cluster_count + 2; c++)
            free += vol->fat_get(c) == 0;
        *out = FsStats{0x4d44 /* MSDOS_SUPER_MAGIC */, vol->cluster_size, vol->cluster_count, free, 0, 0};
        return 0;
    }

    long set_times(vfs::Timespec a, vfs::Timespec m) override {
        LockGuard guard(vol->lock);
        atime = a;
        mtime = m;
        update_dirent(); // FAT stores one modification time (we write "now")
        return 0;
    }

    long truncate(u64 new_size) override {
        if (is_directory)
            return -err::ISDIR;
        LockGuard guard(vol->lock);
        if (new_size == 0) {
            vol->free_chain(first_cluster);
            first_cluster = 0;
        } else if (new_size < size) {
            u32 last = cluster_at(new_size - 1, false);
            if (last) {
                u32 rest = vol->fat_get(last);
                vol->fat_set(last, FAT_MASK);
                vol->free_chain(rest);
            }
        } else if (new_size > size) {
            if (!cluster_at(new_size - 1, true))
                return -err::NOSPC;
            // New bytes read as zero: alloc_cluster zero-fills fresh clusters;
            // zero the tail of the old last cluster.
            u32 c = size ? cluster_at(size, false) : 0;
            if (c) {
                u32 within = static_cast<u32>(size % vol->cluster_size);
                if (within) {
                    usize n = vol->cluster_size - within;
                    u8* zeros = static_cast<u8*>(kzalloc(n));
                    if (zeros) {
                        block::write(vol->dev, vol->cluster_offset(c) + within, zeros, n);
                        kfree(zeros);
                    }
                }
            }
        }
        size = new_size;
        update_dirent();
        return 0;
    }

    // Find `slots` consecutive free directory entries (growing the directory if needed).
    u64 find_free_slots(int slots) {
        int run = 0;
        u64 run_start = 0;
        u32 last = 0;
        for (u32 c = first_cluster; c >= 2 && c < FAT_EOC; c = vol->fat_get(c)) {
            last = c;
            u64 base = vol->cluster_offset(c);
            run = 0; // entries are written as one contiguous block: runs can't span clusters
            for (u32 off = 0; off < vol->cluster_size; off += 32) {
                u8 first;
                block::read(vol->dev, base + off, &first, 1);
                if (first == 0x00 || first == 0xE5) {
                    if (run++ == 0)
                        run_start = base + off;
                    if (run == slots)
                        return run_start;
                } else {
                    run = 0;
                }
            }
        }
        if (static_cast<u32>(slots) * 32 > vol->cluster_size)
            return 0; // name too long for this volume's cluster size
        u32 c = vol->alloc_cluster(last); // directory full: add a (zeroed) cluster
        if (!c)
            return 0;
        return vol->cluster_offset(c);
    }

    bool short_name_taken(const char* sn) {
        return iterate([&](const char*, const DirEntry& e, u64, u64, int) { return memcmp(e.name, sn, 11) == 0; });
    }

    void make_short_name(const char* name, usize len, char* out) {
        memset(out, ' ', 11);
        const char* dot = nullptr;
        for (usize i = 0; i < len; i++)
            if (name[i] == '.') dot = name + i;
        usize base_len = dot ? static_cast<usize>(dot - name) : len;
        auto clean = [](char c) -> char {
            if (c >= 'a' && c <= 'z') return static_cast<char>(c - 32);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') return c;
            return '_';
        };
        int n = 0;
        for (usize i = 0; i < base_len && n < 8; i++)
            if (name[i] != ' ' && name[i] != '.') out[n++] = clean(name[i]);
        if (dot)
            for (int i = 0; i < 3 && dot[1 + i] && dot + 1 + i < name + len; i++)
                out[8 + i] = clean(dot[1 + i]);
        // Always use a numeric tail: guarantees uniqueness without case analysis.
        for (int tail = 1; tail < 1000000; tail++) {
            char digits[8];
            int dl = snprintf(digits, sizeof(digits), "~%d", tail);
            int keep = min(n, 8 - dl);
            char candidate[11];
            memcpy(candidate, out, 11);
            for (int i = keep; i < 8; i++) candidate[i] = ' ';
            memcpy(candidate + keep, digits, static_cast<usize>(dl));
            if (keep == 0) candidate[0] = '_';
            if (!short_name_taken(candidate)) {
                memcpy(out, candidate, 11);
                return;
            }
        }
    }

    long create(const char* name, usize len, u32 new_mode, const char*, vfs::Inode** out) override {
        if (!is_directory)
            return -err::NOTDIR;
        u32 type = new_mode & vfs::S_IFMT;
        if (type != 0 && type != vfs::S_IFREG && type != vfs::S_IFDIR)
            return -err::OPNOTSUPP; // no symlinks/devices on FAT
        if (len > 255)
            return -err::NAMETOOLONG;
        LockGuard guard(vol->lock);
        if (find_locked(name, len))
            return -err::EXIST;

        bool dir = type == vfs::S_IFDIR;
        u32 cluster = 0;
        if (dir) {
            cluster = vol->alloc_cluster(0);
            if (!cluster)
                return -err::NOSPC;
        }
        DirEntry e{};
        e.attr = dir ? ATTR_DIR : ATTR_ARCHIVE;
        e.cluster_hi = static_cast<u16>(cluster >> 16);
        e.cluster_lo = static_cast<u16>(cluster);
        u16 d, t;
        encode_now(&d, &t);
        e.mod_date = e.crt_date = d;
        e.mod_time = e.crt_time = t;
        u64 entry_offset = write_entry_locked(name, len, &e);
        if (!entry_offset) {
            if (cluster)
                vol->free_chain(cluster);
            return -err::NOSPC;
        }

        if (dir) { // "." and ".." in the new directory
            DirEntry dots[2] = {};
            memcpy(dots[0].name, ".          ", 11);
            dots[0].attr = ATTR_DIR;
            dots[0].cluster_hi = e.cluster_hi;
            dots[0].cluster_lo = e.cluster_lo;
            memcpy(dots[1].name, "..         ", 11);
            dots[1].attr = ATTR_DIR;
            u32 parent_cluster = dirent_offset ? first_cluster : 0; // 0 means "root" on FAT
            dots[1].cluster_hi = static_cast<u16>(parent_cluster >> 16);
            dots[1].cluster_lo = static_cast<u16>(parent_cluster);
            block::write(vol->dev, vol->cluster_offset(cluster), dots, sizeof(dots));
        }
        Node* n = vol->node_for(entry_offset, e, this);
        if (out)
            *out = n;
        return 0;
    }

    // Write a directory entry for `name` (long-name slots + the short entry,
    // whose name field is filled in here). Returns its offset, 0 if full.
    u64 write_entry_locked(const char* name, usize len, DirEntry* e) {
        char sn[11];
        make_short_name(name, len, sn);
        int lfn_count = static_cast<int>((len + 12) / 13);
        u64 slot = find_free_slots(lfn_count + 1);
        if (!slot)
            return 0;
        u8 sum = lfn_checksum(sn);
        for (int i = 0; i < lfn_count; i++) {
            LfnEntry l{};
            int seq = lfn_count - i;
            l.order = static_cast<u8>(seq | (i == 0 ? 0x40 : 0));
            l.attr = ATTR_LFN;
            l.checksum = sum;
            u16 chars[13];
            for (int k = 0; k < 13; k++) {
                usize pos = static_cast<usize>(seq - 1) * 13 + k;
                chars[k] = pos < len ? static_cast<u8>(name[pos]) : (pos == len ? 0 : 0xFFFF);
            }
            memcpy(l.name1, chars, 10);
            memcpy(l.name2, chars + 5, 12);
            memcpy(l.name3, chars + 11, 4);
            block::write(vol->dev, slot + static_cast<u64>(i) * 32, &l, 32);
        }
        memcpy(e->name, sn, 11);
        u64 entry_offset = slot + static_cast<u64>(lfn_count) * 32;
        block::write(vol->dev, entry_offset, e, 32);
        return entry_offset;
    }

    // Find `name`: entry offset (0 = absent), first slot, slot count, entry.
    u64 find_entry_locked(const char* name, usize len, u64* first_slot, int* slots, DirEntry* found) {
        u64 entry = 0;
        iterate([&](const char* n, const DirEntry& e, u64 off, u64 fs, int s) {
            if (!equal_nocase(name, len, n))
                return false;
            entry = off;
            *first_slot = fs;
            *slots = s;
            *found = e;
            return true;
        });
        return entry;
    }

    // Mark an entry's long-name slots and the entry itself deleted.
    void erase_slots(u64 entry, u64 first_slot, int slots) {
        u8 deleted = 0xE5;
        if (first_slot + static_cast<u64>(slots - 1) * 32 == entry) {
            for (int i = 0; i < slots; i++)
                block::write(vol->dev, first_slot + static_cast<u64>(i) * 32, &deleted, 1);
        } else {
            block::write(vol->dev, entry, &deleted, 1);
        }
    }

    long rename(const char* old_name, usize old_len, vfs::Inode* new_dir_inode, const char* new_name,
                usize new_len) override {
        auto* new_dir = static_cast<Node*>(new_dir_inode);
        if (new_dir->dev != dev || new_dir->vol != vol)
            return -err::XDEV;
        if (!is_directory || !new_dir->is_directory)
            return -err::NOTDIR;
        if (new_len > 255)
            return -err::NAMETOOLONG;
        LockGuard guard(vol->lock);
        u64 first = 0;
        int slots = 0;
        DirEntry e{};
        u64 entry = find_entry_locked(old_name, old_len, &first, &slots, &e);
        if (!entry)
            return -err::NOENT;
        Node* moving = vol->node_for(entry, e, this);
        bool dir = e.attr & ATTR_DIR;
        // An existing target is replaced (files by files, empty dirs by dirs).
        u64 t_first = 0;
        int t_slots = 0;
        DirEntry te{};
        if (u64 target = new_dir->find_entry_locked(new_name, new_len, &t_first, &t_slots, &te)) {
            if (target == entry) { // same file (maybe a case change): rewrite the name below
            } else {
                bool tdir = te.attr & ATTR_DIR;
                if (tdir && !dir) return -err::ISDIR;
                if (!tdir && dir) return -err::NOTDIR;
                Node* victim = vol->node_for(target, te, new_dir);
                if (tdir && victim->iterate([](const char*, const DirEntry&, u64, u64, int) { return true; }))
                    return -err::NOTEMPTY;
                new_dir->erase_slots(target, t_first, t_slots);
                vol->free_chain(victim->first_cluster);
                victim->first_cluster = 0;
                victim->nlink = 0;
                victim->dirent_offset = 0;
                Node** link = &vol->nodes;
                while (*link && *link != victim)
                    link = &(*link)->next_cached;
                if (*link)
                    *link = victim->next_cached;
            }
        }
        // Old entry out first, so a rename in place can reuse its slots.
        erase_slots(entry, first, slots);
        DirEntry copy = e;
        u64 new_entry = new_dir->write_entry_locked(new_name, new_len, &copy);
        if (!new_entry) { // directory full: put the old entry back
            u8 first_byte = static_cast<u8>(e.name[0]);
            block::write(vol->dev, entry, &first_byte, 1);
            return -err::NOSPC;
        }
        moving->dirent_offset = new_entry;
        moving->parent = new_dir;
        if (dir && new_dir != this) { // its ".." now names the new parent
            u32 parent_cluster = new_dir->dirent_offset ? new_dir->first_cluster : 0;
            u64 dotdot = vol->cluster_offset(moving->first_cluster) + 32;
            u16 hi = static_cast<u16>(parent_cluster >> 16), lo = static_cast<u16>(parent_cluster);
            block::write(vol->dev, dotdot + 20, &hi, 2);
            block::write(vol->dev, dotdot + 26, &lo, 2);
        }
        return 0;
    }

    long unlink(const char* name, usize len, bool remove_dir) override {
        if (!is_directory)
            return -err::NOTDIR;
        LockGuard guard(vol->lock);
        u64 entry = 0, first_slot = 0;
        int slots = 0;
        DirEntry found{};
        iterate([&](const char* n, const DirEntry& e, u64 off, u64 fs, int s) {
            if (!equal_nocase(name, len, n))
                return false;
            entry = off;
            first_slot = fs;
            slots = s;
            found = e;
            return true;
        });
        if (!entry)
            return -err::NOENT;
        bool dir = found.attr & ATTR_DIR;
        if (remove_dir && !dir)
            return -err::NOTDIR;
        if (!remove_dir && dir)
            return -err::ISDIR;
        Node* node = vol->node_for(entry, found, this);
        if (dir && node->iterate([](const char*, const DirEntry&, u64, u64, int) { return true; }))
            return -err::NOTEMPTY;

        // Mark the long-name slots and the entry itself deleted. LFN slots
        // directly precede the entry (they may span a cluster boundary in
        // theory; we only write in place when they're contiguous).
        u8 deleted = 0xE5;
        if (first_slot + static_cast<u64>(slots - 1) * 32 == entry) {
            for (int i = 0; i < slots; i++)
                block::write(vol->dev, first_slot + static_cast<u64>(i) * 32, &deleted, 1);
        } else {
            block::write(vol->dev, entry, &deleted, 1);
        }
        vol->free_chain(node->first_cluster);
        node->first_cluster = 0;
        node->nlink = 0;
        node->dirent_offset = 0; // detached: nothing on disk refers to it any more
        // Drop it from the cache so a new file with the same entry slot gets a fresh node.
        Node** link = &vol->nodes;
        while (*link && *link != node)
            link = &(*link)->next_cached;
        if (*link)
            *link = node->next_cached;
        return 0;
    }
};

Node* Volume::node_for(u64 dirent_offset, const DirEntry& e, Node* parent) {
    for (Node* n = nodes; n; n = n->next_cached)
        if (n->dirent_offset == dirent_offset)
            return n;
    u32 cluster = (static_cast<u32>(e.cluster_hi) << 16) | e.cluster_lo;
    bool dir = e.attr & ATTR_DIR;
    auto* n = new Node(this, cluster, dirent_offset, dir, e.size, decode_time(e.mod_date, e.mod_time));
    if (e.attr & ATTR_READONLY)
        n->mode &= ~0222u;
    n->parent = parent;
    n->next_cached = nodes;
    nodes = n;
    return n;
}

} // namespace

bool probe(block::Device* dev) {
    u8 bs[512];
    if (dev->size_bytes() < 64 * 512 || block::read(dev, 0, bs, 512) != 0)
        return false;
    return bs[510] == 0x55 && bs[511] == 0xAA && memcmp(bs + 82, "FAT32", 5) == 0;
}

long mount(block::Device* dev, vfs::Inode** root) {
    u8 bs[512];
    if (block::read(dev, 0, bs, 512) != 0)
        return -err::IO;
    if (!probe(dev))
        return -err::INVAL; // FAT12/16 aren't supported yet
    u16 bytes_per_sector;
    memcpy(&bytes_per_sector, bs + 11, 2);
    u8 sectors_per_cluster = bs[13];
    u16 reserved;
    memcpy(&reserved, bs + 14, 2);
    u8 fats = bs[16];
    u32 total_sectors, fat_sectors, root_cluster;
    u16 total16;
    memcpy(&total16, bs + 19, 2);
    memcpy(&total_sectors, bs + 32, 4);
    if (total16)
        total_sectors = total16;
    memcpy(&fat_sectors, bs + 36, 4);
    memcpy(&root_cluster, bs + 44, 4);
    if (!bytes_per_sector || !sectors_per_cluster || !fats || !fat_sectors)
        return -err::INVAL;

    auto* v = new Volume;
    v->dev = dev;
    v->cluster_size = static_cast<u32>(bytes_per_sector) * sectors_per_cluster;
    v->fat_start = static_cast<u64>(reserved) * bytes_per_sector;
    v->fat_bytes = static_cast<u64>(fat_sectors) * bytes_per_sector;
    v->fat_count = fats;
    v->data_start = v->fat_start + v->fat_bytes * fats;
    u64 data_sectors = total_sectors - (v->data_start / bytes_per_sector);
    v->cluster_count = static_cast<u32>(data_sectors / sectors_per_cluster);
    v->root_cluster = root_cluster;

    // FSInfo's free-cluster count would go stale as we allocate: mark it
    // "unknown" (what Linux does on a read-write mount) so fsck stays happy.
    u16 fsinfo_sector;
    memcpy(&fsinfo_sector, bs + 48, 2);
    if (fsinfo_sector && fsinfo_sector != 0xFFFF) {
        u64 off = static_cast<u64>(fsinfo_sector) * bytes_per_sector;
        u32 sig = 0;
        block::read(dev, off, &sig, 4);
        if (sig == 0x41615252) {
            u32 unknown = 0xFFFFFFFF;
            block::write(dev, off + 488, &unknown, 4);
        }
    }

    auto* r = new Node(v, root_cluster, 0, true, 0, 0);
    r->atime = r->mtime = r->ctime = vfs::now();
    *root = r;
    klog::ok("FAT32 on %s: %u clusters of %u bytes (%lu MiB)", dev->name, v->cluster_count, v->cluster_size,
             static_cast<u64>(v->cluster_count) * v->cluster_size / MiB);
    return 0;
}

} // namespace fat
