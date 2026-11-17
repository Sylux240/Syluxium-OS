#include <block/block.hpp>
#include <core/log.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/list.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>

namespace block {

namespace {

constexpr usize BLOCK = 4096;
constexpr usize HASH_SIZE = 4096;
constexpr usize MAX_CACHED = 16384; // 64 MiB of cache

struct CacheBlock {
    Device* dev;
    u64 number;      // in 4 KiB units from the start of the device
    u8* data;
    u32 valid_bytes; // the last block of a device can be partial
    bool dirty;
    CacheBlock* hash_next;
    ListNode lru_node; // most recently used at the back
};

CacheBlock* g_hash[HASH_SIZE];
IntrusiveList<CacheBlock, &CacheBlock::lru_node> g_lru;
usize g_cached = 0;
u64 g_hits = 0, g_misses = 0;
sched::Mutex g_cache_lock; // I/O happens under it: disk requests may sleep

constexpr usize MAX_DEVICES = 64;
Device* g_devices[MAX_DEVICES];
usize g_device_count = 0;

usize hash_of(const Device* d, u64 n) { return ((reinterpret_cast<uptr>(d) >> 4) ^ (n * 0x9E3779B1)) % HASH_SIZE; }

long write_back(CacheBlock* b) {
    if (!b->dirty)
        return 0;
    u32 sectors = (b->valid_bytes + b->dev->sector_size - 1) / b->dev->sector_size;
    u64 lba = b->number * (BLOCK / b->dev->sector_size);
    long r = b->dev->write_sectors(lba, sectors, b->data);
    if (r == 0)
        b->dirty = false;
    return r;
}

void evict_one() {
    CacheBlock* victim = g_lru.front();
    if (!victim)
        return;
    write_back(victim);
    g_lru.remove(victim);
    CacheBlock** link = &g_hash[hash_of(victim->dev, victim->number)];
    while (*link != victim)
        link = &(*link)->hash_next;
    *link = victim->hash_next;
    pmm::free_page(mm::direct_virt_to_phys(victim->data));
    delete victim;
    g_cached--;
}

// Find or load block `n` of `d`. Cache lock held.
CacheBlock* get_block(Device* d, u64 n, bool will_overwrite) {
    for (CacheBlock* b = g_hash[hash_of(d, n)]; b; b = b->hash_next) {
        if (b->dev == d && b->number == n) {
            g_hits++;
            g_lru.remove(b);
            g_lru.push_back(b);
            return b;
        }
    }
    g_misses++;
    if (g_cached >= MAX_CACHED)
        evict_one();

    u64 phys = pmm::alloc_page();
    if (!phys)
        return nullptr;
    auto* b = new CacheBlock{d, n, mm::phys_to_virt<u8>(phys), 0, false, nullptr, {}};
    u64 start = n * BLOCK;
    u64 end = min<u64>(start + BLOCK, d->size_bytes());
    b->valid_bytes = static_cast<u32>(end - start);
    if (!will_overwrite) {
        u32 sectors = (b->valid_bytes + d->sector_size - 1) / d->sector_size;
        if (d->read_sectors(start / d->sector_size, sectors, b->data) != 0) {
            pmm::free_page(phys);
            delete b;
            return nullptr;
        }
    }
    usize h = hash_of(d, n);
    b->hash_next = g_hash[h];
    g_hash[h] = b;
    g_lru.push_back(b);
    g_cached++;
    return b;
}

// ---- partitions ----

class Partition : public Device {
public:
    Partition(Device* parent, u64 start, u64 count, int number) : m_parent(parent), m_start(start) {
        snprintf(name, sizeof(name), "%s%s%d", parent->name,
                 parent->name[strlen(parent->name) - 1] >= '0' && parent->name[strlen(parent->name) - 1] <= '9'
                     ? "p" : "", number);
        sector_count = count;
        sector_size = parent->sector_size;
        snprintf(model, sizeof(model), "%s", parent->model);
        hw = parent->hw;
    }
    long read_sectors(u64 lba, u32 count, void* buf) override {
        if (lba + count > sector_count)
            return -err::IO;
        return m_parent->read_sectors(m_start + lba, count, buf);
    }
    long write_sectors(u64 lba, u32 count, const void* buf) override {
        if (lba + count > sector_count)
            return -err::IO;
        return m_parent->write_sectors(m_start + lba, count, buf);
    }
    long flush() override { return m_parent->flush(); }

private:
    Device* m_parent;
    u64 m_start;
};

// /dev/sdX, /dev/sdXN: byte access through the cache.
class BlockNode : public vfs::Inode {
public:
    explicit BlockNode(Device* d, u32 minor) : m_dev(d) {
        ino = vfs::next_ino();
        mode = vfs::S_IFBLK | 0660;
        rdev = (8u << 8) | minor; // major 8: SCSI/SATA disks on Linux
        size = d->size_bytes();
        atime = mtime = ctime = vfs::now();
    }
    isize read(u64 offset, void* buf, usize count) override {
        if (offset >= m_dev->size_bytes())
            return 0;
        count = min<u64>(count, m_dev->size_bytes() - offset);
        long r = block::read(m_dev, offset, buf, count);
        return r < 0 ? r : static_cast<isize>(count);
    }
    isize write(u64 offset, const void* buf, usize count) override {
        if (offset >= m_dev->size_bytes())
            return -err::NOSPC;
        count = min<u64>(count, m_dev->size_bytes() - offset);
        long r = block::write(m_dev, offset, buf, count);
        return r < 0 ? r : static_cast<isize>(count);
    }

private:
    Device* m_dev;
};

void add_device(Device* d) {
    if (g_device_count >= MAX_DEVICES)
        return;
    g_devices[g_device_count] = d;
    char path[32];
    snprintf(path, sizeof(path), "/dev/%s", d->name);
    vfs::add_node(path, new BlockNode(d, static_cast<u32>(g_device_count)));
    g_device_count++;
}

struct PACKED MbrEntry {
    u8 status, chs_first[3], type, chs_last[3];
    u32 lba_start, sectors;
};

struct PACKED GptHeader {
    char signature[8];
    u32 revision, header_size, crc, reserved;
    u64 current_lba, backup_lba, first_usable, last_usable;
    u8 disk_guid[16];
    u64 entries_lba;
    u32 entry_count, entry_size, entries_crc;
};

void scan_partitions(Device* disk) {
    u8* sector = static_cast<u8*>(kmalloc(4096));
    if (!sector)
        return;
    int found = 0;
    if (read(disk, 0, sector, 512) == 0 && sector[510] == 0x55 && sector[511] == 0xAA) {
        MbrEntry entries[4];
        memcpy(entries, sector + 446, sizeof(entries));
        if (entries[0].type == 0xEE) {
            // Protective MBR: the real table is GPT.
            GptHeader h;
            if (read(disk, disk->sector_size, &h, sizeof(h)) == 0 && memcmp(h.signature, "EFI PART", 8) == 0) {
                for (u32 i = 0; i < h.entry_count && i < 128; i++) {
                    u8 e[128];
                    if (read(disk, h.entries_lba * disk->sector_size + static_cast<u64>(i) * h.entry_size, e, 128) != 0)
                        break;
                    u64 first, last;
                    memcpy(&first, e + 32, 8);
                    memcpy(&last, e + 40, 8);
                    bool empty = true;
                    for (int k = 0; k < 16; k++) empty &= e[k] == 0;
                    if (empty || last < first)
                        continue;
                    add_device(new Partition(disk, first, last - first + 1, static_cast<int>(i) + 1));
                    found++;
                }
            }
        } else {
            for (int i = 0; i < 4; i++) {
                if (entries[i].type == 0 || entries[i].sectors == 0)
                    continue;
                add_device(new Partition(disk, entries[i].lba_start, entries[i].sectors, i + 1));
                found++;
            }
        }
    }
    kfree(sector);
    if (found)
        klog::info("  %s: %d partition(s)", disk->name, found);
}

void writeback_thread(void*) {
    for (;;) {
        sched::sleep_ms(5000);
        sync_all();
    }
}

} // namespace

void register_disk(Device* disk) {
    add_device(disk);
    klog::ok("Disk %s: %s, %lu MiB", disk->name, disk->model, disk->size_bytes() / MiB);
    scan_partitions(disk);
}

void next_sd_name(char* out, usize size) {
    static int next = 0;
    snprintf(out, size, "sd%c", 'a' + __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED));
}

Device* find(const char* name) {
    for (usize i = 0; i < g_device_count; i++)
        if (strcmp(g_devices[i]->name, name) == 0)
            return g_devices[i];
    return nullptr;
}

void for_each(void (*fn)(Device*, void*), void* ctx) {
    for (usize i = 0; i < g_device_count; i++)
        fn(g_devices[i], ctx);
}

long read(Device* d, u64 offset, void* buf, usize len) {
    if (offset + len > d->size_bytes())
        return -err::IO;
    LockGuard guard(g_cache_lock);
    auto* out = static_cast<u8*>(buf);
    while (len) {
        u64 n = offset / BLOCK;
        usize in_block = offset % BLOCK;
        usize chunk = min<usize>(len, BLOCK - in_block);
        CacheBlock* b = get_block(d, n, false);
        if (!b)
            return -err::IO;
        memcpy(out, b->data + in_block, chunk);
        out += chunk;
        offset += chunk;
        len -= chunk;
    }
    return 0;
}

long write(Device* d, u64 offset, const void* buf, usize len) {
    if (offset + len > d->size_bytes())
        return -err::IO;
    LockGuard guard(g_cache_lock);
    auto* in = static_cast<const u8*>(buf);
    while (len) {
        u64 n = offset / BLOCK;
        usize in_block = offset % BLOCK;
        usize chunk = min<usize>(len, BLOCK - in_block);
        bool whole = in_block == 0 && chunk == min<u64>(BLOCK, d->size_bytes() - n * BLOCK);
        CacheBlock* b = get_block(d, n, whole);
        if (!b)
            return -err::IO;
        memcpy(b->data + in_block, in, chunk);
        b->dirty = true;
        in += chunk;
        offset += chunk;
        len -= chunk;
    }
    return 0;
}

void sync_all() {
    LockGuard guard(g_cache_lock);
    bool wrote = false;
    for (CacheBlock* b : g_lru) {
        if (b->dirty) {
            write_back(b);
            wrote = true;
        }
    }
    if (wrote)
        for (usize i = 0; i < g_device_count; i++)
            g_devices[i]->flush();
}

void init() { sched::spawn("writeback", writeback_thread, nullptr, -1, true); }

CacheStats cache_stats() {
    LockGuard guard(g_cache_lock);
    u64 dirty = 0;
    for (CacheBlock* b : g_lru)
        dirty += b->dirty;
    return CacheStats{g_cached, dirty, g_hits, g_misses};
}

} // namespace block
