#pragma once

#include <dev/device.hpp>
#include <lib/errno.hpp>
#include <lib/types.hpp>

// Block devices (disks and their partitions) and the disk cache.
//
// Drivers implement read/write of whole sectors; everything above (file
// systems, /dev/sdX) goes through the cache, which works in 4 KiB blocks and
// writes dirty data back on sync, on eviction, and every few seconds.
namespace block {

class Device {
public:
    virtual ~Device() = default;

    char name[16] = {};       // "sda", "sda1", "nvme0n1"
    char model[48] = {};
    u64 sector_count = 0;
    u32 sector_size = 512;
    dev::Device* hw = nullptr;
    Device* parent = nullptr;  // partitions: the disk they're on
    bool removable = false;    // USB sticks, card readers (/sys/block/<name>/removable)
    // Not cached here (loop devices: their file is cached by its own disk, and
    // reading it through the cache from inside the cache would deadlock).
    bool uncached = false;

    u64 size_bytes() const { return sector_count * sector_size; }

    virtual long read_sectors(u64 lba, u32 count, void* buf) = 0;
    virtual long write_sectors(u64 lba, u32 count, const void* buf) = 0;
    virtual long flush() { return 0; }
};

// Make a disk known: creates /dev/<name>, then scans for partitions
// (which become devices of their own: sda1, sda2, ...).
void register_disk(Device* disk);
Device* find(const char* name);
// Devices with a /dev node of their own (loop): in the table, nothing else.
// detach also drops their cached blocks (unwritten data is lost).
void attach_nodeless(Device* d);
void detach_nodeless(Device* d);
// The disk was unplugged: drop its cached blocks (unwritten data is lost, as
// on Linux), remove /dev/<name> and its partitions. The objects stay
// allocated - a mounted file system may still point at them - and the
// driver makes their I/O fail from now on.
void unregister_disk(Device* disk);

// Re-read a disk's partition table (BLKRRPART, after sfdisk/fdisk wrote a
// new one): old partition devices go away, new ones appear as /dev/<name>N.
long rescan_partitions(Device* disk);
// Lowest free SCSI-style disk name: "sda", "sdb", ... (shared by SATA and USB storage).
void next_sd_name(char* out, usize size);
void for_each(void (*fn)(Device* d, void* ctx), void* ctx);

// Byte-granular access through the cache.
long read(Device* d, u64 offset, void* buf, usize len);
long write(Device* d, u64 offset, const void* buf, usize len);

// Write every dirty cached block back to its disk (after running the sync
// hooks: journaling file systems commit there).
void sync_all();
void add_sync_hook(void (*fn)(void*), void* ctx);

// Journaling support. Held bytes (delta +1 / -1, counted, tracked per
// sector) stay in the cache and aren't written back until every hold is
// released; the rest of the same cache block is written back as usual.
long hold(Device* d, u64 offset, usize len, int delta);
// Write through the cache straight to the disk (no flush; see sync_device).
long write_through(Device* d, u64 offset, const void* buf, usize len);
// Write back d's dirty (unheld) blocks, then flush the disk's write cache.
long sync_device(Device* d);

void init(); // start the background writeback thread

struct CacheStats {
    u64 blocks, dirty, hits, misses;
};
CacheStats cache_stats();

} // namespace block
