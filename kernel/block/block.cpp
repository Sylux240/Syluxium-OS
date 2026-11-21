#include <block/block.hpp>
#include <core/log.hpp>
#include <fs/genfs.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/list.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <proc/process.hpp>
#include <proc/uaccess.hpp>
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
    u8 dirty;        // one bit per sector (512-byte sectors: 8 per block)
    u16 held[8];     // per sector: > 0 = a journal transaction owns it (not written back yet)
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
// sdX letters handed out by next_sd_name() (before the disk is registered).
u32 g_sd_taken = 0;
sched::Mutex g_sd_lock;

usize hash_of(const Device* d, u64 n) { return ((reinterpret_cast<uptr>(d) >> 4) ^ (n * 0x9E3779B1)) % HASH_SIZE; }

u32 sectors_per_block(const Device* d) { return min<u32>(BLOCK / d->sector_size, 8); }
u32 sector_unit(const Device* d) { return BLOCK / sectors_per_block(d); } // bytes per dirty/held bit

bool any_held(const CacheBlock* b) {
    for (u16 h : b->held)
        if (h)
            return true;
    return false;
}

// Write the dirty sectors that no journal transaction holds (in runs).
long write_back(CacheBlock* b) {
    if (!b->dirty)
        return 0;
    Device* d = b->dev;
    u32 units = sectors_per_block(d), unit = sector_unit(d);
    u32 valid_units = (b->valid_bytes + unit - 1) / unit;
    for (u32 i = 0; i < valid_units;) {
        if (!(b->dirty & (1u << i)) || b->held[i]) {
            i++;
            continue;
        }
        u32 j = i;
        while (j < valid_units && (b->dirty & (1u << j)) && !b->held[j])
            j++;
        u32 first_byte = i * unit, bytes = min<u32>(j * unit, b->valid_bytes) - first_byte;
        u32 count = (bytes + d->sector_size - 1) / d->sector_size;
        u64 lba = (b->number * BLOCK + first_byte) / d->sector_size;
        if (long r = d->write_sectors(lba, count, b->data + first_byte); r != 0)
            return r;
        for (u32 k = i; k < j; k++)
            b->dirty &= static_cast<u8>(~(1u << k));
        i = j;
    }
    (void)units;
    return 0;
}

u8 unit_mask(const Device* d, usize in_block, usize len) {
    u32 unit = sector_unit(d);
    u8 mask = 0;
    for (usize k = in_block / unit; k <= (in_block + len - 1) / unit && k < 8; k++)
        mask |= static_cast<u8>(1u << k);
    return mask;
}

// Unlink and free a cached block. Cache lock held.
void drop_block(CacheBlock* victim) {
    g_lru.remove(victim);
    CacheBlock** link = &g_hash[hash_of(victim->dev, victim->number)];
    while (*link != victim)
        link = &(*link)->hash_next;
    *link = victim->hash_next;
    pmm::free_page(mm::direct_virt_to_phys(victim->data));
    delete victim;
    g_cached--;
}

void evict_one() {
    CacheBlock* victim = g_lru.front();
    while (victim && any_held(victim))
        victim = g_lru.next_of(victim);
    if (!victim)
        return;
    write_back(victim);
    drop_block(victim);
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
    auto* b = new CacheBlock{d, n, mm::phys_to_virt<u8>(phys), 0, 0, {}, nullptr, {}};
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
        this->parent = parent;
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

    u64 start_lba() const { return m_start; }

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
        gid = vfs::GID_DISK;
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

    // The Linux block-device ioctls disk tools use (sfdisk, mkfs, blkid, dd).
    long ioctl(u64 request, u64 arg) override {
        constexpr u64 BLKROGET = 0x125e, BLKRRPART = 0x125f, BLKGETSIZE = 0x1260, BLKFLSBUF = 0x1261,
                      BLKSSZGET = 0x1268, BLKIOMIN = 0x1278, BLKIOOPT = 0x1279, BLKALIGNOFF = 0x127a,
                      BLKPBSZGET = 0x127b, BLKDISCARDZEROES = 0x127c, BLKGETSIZE64 = 0x80081272,
                      HDIO_GETGEO = 0x0301;
        void* uarg = reinterpret_cast<void*>(arg);
        switch (request) {
        case BLKGETSIZE64: {
            u64 bytes = m_dev->size_bytes();
            return uaccess::put(static_cast<u64*>(uarg), bytes);
        }
        case BLKGETSIZE: {
            u64 sectors = m_dev->size_bytes() / 512;
            return uaccess::put(static_cast<u64*>(uarg), sectors);
        }
        case BLKSSZGET:
        case BLKPBSZGET:
        case BLKIOMIN: {
            i32 v = static_cast<i32>(m_dev->sector_size);
            return uaccess::put(static_cast<i32*>(uarg), v);
        }
        case BLKIOOPT: case BLKALIGNOFF: case BLKROGET: case BLKDISCARDZEROES: {
            i32 zero = 0;
            return uaccess::put(static_cast<i32*>(uarg), zero);
        }
        case HDIO_GETGEO: {
            struct { u8 heads, sectors; u16 cylinders; u64 start; } g{255, 63, 0, 0};
            u64 cyl = m_dev->size_bytes() / 512 / (255 * 63);
            g.cylinders = static_cast<u16>(cyl > 0xFFFF ? 0xFFFF : cyl);
            if (m_dev->parent)
                g.start = static_cast<Partition*>(m_dev)->start_lba();
            return uaccess::copy_to(uarg, &g, sizeof(g));
        }
        case BLKFLSBUF:
            return sync_device(m_dev);
        case BLKRRPART:
            if (m_dev->parent)
                return -err::INVAL; // only whole disks have partition tables
            if (!proc::capable())
                return -err::ACCES;
            return rescan_partitions(m_dev);
        default:
            return -err::NOTTY;
        }
    }

private:
    Device* m_dev;
};

void publish_partition(Device* part);
void unpublish_partition(Device* part);

void add_device(Device* d) {
    if (g_device_count >= MAX_DEVICES)
        return;
    g_devices[g_device_count] = d;
    char path[32];
    snprintf(path, sizeof(path), "/dev/%s", d->name);
    vfs::add_node(path, new BlockNode(d, static_cast<u32>(g_device_count)));
    g_device_count++;
    if (d->parent)
        publish_partition(d);
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

// /sys/block/<disk>/{removable,size}: how user space (the automounter)
// tells USB sticks from internal disks.
void publish_sysfs(Device* disk) {
    char dir[48], path[80], text[32];
    snprintf(dir, sizeof(dir), "/sys/block/%s", disk->name);
    if (vfs::mkdir_p(dir, 0755) < 0)
        return;
    snprintf(path, sizeof(path), "%s/removable", dir);
    genfs::static_file(path, disk->removable ? "1\n" : "0\n");
    snprintf(path, sizeof(path), "%s/size", dir);
    snprintf(text, sizeof(text), "%lu\n", disk->size_bytes() / 512);
    genfs::static_file(path, text);
    // device/model: the disk's name for Disk Management and the System page.
    snprintf(path, sizeof(path), "%s/device", dir);
    vfs::mkdir_p(path, 0755);
    snprintf(path, sizeof(path), "%s/device/model", dir);
    char model[64];
    snprintf(model, sizeof(model), "%s\n", disk->model);
    genfs::static_file(path, model);
}

namespace {

// /sys/block/<disk>/<partition>/{size,partition}, as on Linux.
void publish_partition(Device* part) {
    char dir[64], path[96], text[32];
    snprintf(dir, sizeof(dir), "/sys/block/%s/%s", part->parent->name, part->name);
    if (vfs::mkdir_p(dir, 0755) < 0)
        return;
    snprintf(path, sizeof(path), "%s/size", dir);
    snprintf(text, sizeof(text), "%lu\n", part->size_bytes() / 512);
    genfs::static_file(path, text);
    const char* digits = part->name + strlen(part->name);
    while (digits > part->name && digits[-1] >= '0' && digits[-1] <= '9')
        digits--;
    snprintf(path, sizeof(path), "%s/partition", dir);
    snprintf(text, sizeof(text), "%s\n", digits);
    genfs::static_file(path, text);
}

void unpublish_partition(Device* part) {
    char dir[64], path[96];
    snprintf(dir, sizeof(dir), "/sys/block/%s/%s", part->parent->name, part->name);
    snprintf(path, sizeof(path), "%s/size", dir);
    vfs::remove_node(path);
    snprintf(path, sizeof(path), "%s/partition", dir);
    vfs::remove_node(path);
    snprintf(path, sizeof(path), "/sys/block/%s", part->parent->name);
    if (vfs::Inode* d; vfs::resolve(path, nullptr, true, &d) == 0)
        d->unlink(part->name, strlen(part->name), true);
}

} // namespace

void unpublish_sysfs(Device* disk) {
    char path[80];
    for (usize i = 0; i < g_device_count; i++)
        if (g_devices[i]->parent == disk)
            unpublish_partition(g_devices[i]);
    snprintf(path, sizeof(path), "/sys/block/%s/device/model", disk->name);
    vfs::remove_node(path);
    snprintf(path, sizeof(path), "/sys/block/%s/device", disk->name);
    if (vfs::Inode* dd; vfs::resolve(path, nullptr, true, &dd) == 0) {
        char diskdir[48];
        snprintf(diskdir, sizeof(diskdir), "/sys/block/%s", disk->name);
        if (vfs::Inode* parent; vfs::resolve(diskdir, nullptr, true, &parent) == 0)
            parent->unlink("device", 6, true);
    }
    snprintf(path, sizeof(path), "/sys/block/%s/removable", disk->name);
    vfs::remove_node(path);
    snprintf(path, sizeof(path), "/sys/block/%s/size", disk->name);
    vfs::remove_node(path);
    vfs::Inode* blockdir;
    if (vfs::resolve("/sys/block", nullptr, true, &blockdir) == 0)
        blockdir->unlink(disk->name, strlen(disk->name), true);
}

void register_disk(Device* disk) {
    add_device(disk);
    publish_sysfs(disk);
    klog::ok("Disk %s: %s, %lu MiB", disk->name, disk->model, disk->size_bytes() / MiB);
    scan_partitions(disk);
}

void next_sd_name(char* out, usize size) {
    LockGuard guard(g_sd_lock);
    for (int c = 0; c < 26; c++) {
        char name[4] = {'s', 'd', static_cast<char>('a' + c), 0};
        if (find(name) || (g_sd_taken & (1u << c)))
            continue;
        g_sd_taken |= 1u << c;
        snprintf(out, size, "%s", name);
        return;
    }
    snprintf(out, size, "sdz");
}

void unregister_disk(Device* disk) {
    unpublish_sysfs(disk);
    LockGuard guard(g_cache_lock);
    for (CacheBlock* b = g_lru.front(); b;) {
        CacheBlock* next = g_lru.next_of(b);
        if (b->dev == disk || b->dev->parent == disk)
            drop_block(b);
        b = next;
    }
    usize kept = 0;
    for (usize i = 0; i < g_device_count; i++) {
        Device* d = g_devices[i];
        if (d == disk || d->parent == disk) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/%s", d->name);
            vfs::remove_node(path);
            klog::info("Disk %s removed", d->name);
            continue;
        }
        g_devices[kept++] = d;
    }
    g_device_count = kept;
    // The letter may be handed out again (the next stick becomes sdb again).
    if (disk->name[0] == 's' && disk->name[1] == 'd' && disk->name[2] >= 'a' && disk->name[2] <= 'z' && !disk->name[3])
        __atomic_and_fetch(&g_sd_taken, ~(1u << (disk->name[2] - 'a')), __ATOMIC_RELAXED);
}

long rescan_partitions(Device* disk) {
    sync_device(disk);
    {
        LockGuard guard(g_cache_lock);
        // Forget the old partitions: their cached blocks and /dev nodes. (The
        // objects stay allocated, as on unplug: a stale mount may point at them.)
        for (CacheBlock* b = g_lru.front(); b;) {
            CacheBlock* next = g_lru.next_of(b);
            if (b->dev->parent == disk) {
                write_back(b);
                drop_block(b);
            }
            b = next;
        }
        usize kept = 0;
        for (usize i = 0; i < g_device_count; i++) {
            Device* d = g_devices[i];
            if (d->parent == disk) {
                char path[32];
                snprintf(path, sizeof(path), "/dev/%s", d->name);
                vfs::remove_node(path);
                unpublish_partition(d);
                continue;
            }
            g_devices[kept++] = d;
        }
        g_device_count = kept;
        // The disk's own cached copy of the table area may be stale too.
        for (CacheBlock* b = g_lru.front(); b;) {
            CacheBlock* next = g_lru.next_of(b);
            if (b->dev == disk && !b->dirty)
                drop_block(b);
            b = next;
        }
    }
    scan_partitions(disk);
    return 0;
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
        b->dirty |= unit_mask(d, in_block, chunk);
        in += chunk;
        offset += chunk;
        len -= chunk;
    }
    return 0;
}

namespace {
struct SyncHook {
    void (*fn)(void*);
    void* ctx;
};
SyncHook g_sync_hooks[16];
usize g_sync_hook_count = 0;
} // namespace

void add_sync_hook(void (*fn)(void*), void* ctx) {
    if (g_sync_hook_count < 16)
        g_sync_hooks[g_sync_hook_count++] = SyncHook{fn, ctx};
}

long hold(Device* d, u64 offset, usize len, int delta) {
    LockGuard guard(g_cache_lock);
    while (len) {
        u64 n = offset / BLOCK;
        usize in_block = offset % BLOCK;
        usize chunk = min<usize>(len, BLOCK - in_block);
        CacheBlock* b = get_block(d, n, false);
        if (!b)
            return -err::IO;
        u8 mask = unit_mask(d, in_block, chunk);
        for (u32 k = 0; k < 8; k++)
            if (mask & (1u << k))
                b->held[k] = static_cast<u16>(b->held[k] + delta);
        offset += chunk;
        len -= chunk;
    }
    return 0;
}

long write_through(Device* d, u64 offset, const void* buf, usize len) {
    if (long r = write(d, offset, buf, len); r < 0)
        return r;
    LockGuard guard(g_cache_lock);
    for (u64 n = offset / BLOCK; n <= (offset + len - 1) / BLOCK; n++) {
        CacheBlock* b = get_block(d, n, false);
        if (!b)
            return -err::IO;
        if (long r = write_back(b); r < 0)
            return r;
    }
    return 0;
}

long sync_device(Device* d) {
    LockGuard guard(g_cache_lock);
    for (CacheBlock* b : g_lru)
        if (b->dirty && b->dev == d)
            if (long r = write_back(b); r < 0)
                return r;
    return d->flush();
}

void sync_all() {
    for (usize i = 0; i < g_sync_hook_count; i++) // file systems commit their journals
        g_sync_hooks[i].fn(g_sync_hooks[i].ctx);
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
        dirty += b->dirty != 0;
    return CacheStats{g_cached, dirty, g_hits, g_misses};
}

} // namespace block
