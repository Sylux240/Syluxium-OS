#pragma once

#include <lib/types.hpp>

// General-purpose kernel allocator.
//
//   <= 2 KiB   slab allocator: power-of-two size classes, objects packed in pages
//   <= 4 MiB   whole buddy blocks (physically contiguous)
//   larger     vmalloc (virtually contiguous)
//
// Every block is aligned to its size class (so up to 4 KiB alignment for free).
void* kmalloc(usize size);
void* kzalloc(usize size);
void* kcalloc(usize count, usize size);
void* krealloc(void* ptr, usize new_size);
void kfree(void* ptr);
usize ksize(const void* ptr); // usable size of a block

namespace heap {

struct ClassStats {
    u32 object_size;
    u64 slabs;
    u64 objects_in_use;
};

constexpr usize SIZE_CLASS_COUNT = 8; // 16 .. 2048 bytes
constexpr usize MAX_SLAB_OBJECT = 2048;

ClassStats class_stats(usize index);
void dump_stats();
extern u64 g_large_pages;
u64 kernel_heap_pages(); // slab pages + large kmalloc blocks (for /proc/meminfo Slab)

} // namespace heap
