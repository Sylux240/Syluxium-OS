#pragma once

#include <lib/types.hpp>
#include <mm/vmm.hpp>

// Virtually contiguous kernel memory in the vmalloc region.
//
// Unlike kmalloc, the pages behind a vmalloc block need not be physically
// contiguous, so it works for huge allocations, and every block has an
// unmapped guard page below it so overruns (e.g. kernel stack overflow)
// fault immediately instead of corrupting neighbors.
namespace vmalloc {

void* alloc(usize size);       // zero-filled, page granular
void free(void* ptr);
usize size_of(const void* ptr); // usable size of a block, 0 if unknown

// Map device registers (or other physical memory) into kernel space.
void* map_physical(u64 phys, usize size, u32 flags = vmm::WRITE | vmm::CACHE_UC);
void unmap_physical(void* virt);
// Map scattered physical pages contiguously (Linux vmap); undo with unmap_physical.
void* map_pages(const u64* phys, usize count, u32 flags = vmm::WRITE);

struct Stats {
    u64 areas;
    u64 mapped_pages;
};
Stats stats();

} // namespace vmalloc
