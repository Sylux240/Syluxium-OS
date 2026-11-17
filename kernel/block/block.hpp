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

    u64 size_bytes() const { return sector_count * sector_size; }

    virtual long read_sectors(u64 lba, u32 count, void* buf) = 0;
    virtual long write_sectors(u64 lba, u32 count, const void* buf) = 0;
    virtual long flush() { return 0; }
};

// Make a disk known: creates /dev/<name>, then scans for partitions
// (which become devices of their own: sda1, sda2, ...).
void register_disk(Device* disk);
Device* find(const char* name);
// Next free SCSI-style disk name: "sda", "sdb", ... (shared by SATA and USB storage).
void next_sd_name(char* out, usize size);
void for_each(void (*fn)(Device* d, void* ctx), void* ctx);

// Byte-granular access through the cache.
long read(Device* d, u64 offset, void* buf, usize len);
long write(Device* d, u64 offset, const void* buf, usize len);

// Write every dirty cached block back to its disk.
void sync_all();

void init(); // start the background writeback thread

struct CacheStats {
    u64 blocks, dirty, hits, misses;
};
CacheStats cache_stats();

} // namespace block
