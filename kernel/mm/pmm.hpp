#pragma once

#include <lib/types.hpp>

// Physical memory manager: a buddy allocator over 4 KiB page frames.
//
// Free memory is kept in power-of-two blocks ("orders": order 0 = 1 page,
// order 10 = 1024 pages = 4 MiB). Allocating splits a bigger block in half
// until it fits; freeing merges a block with its "buddy" (the other half
// it was split from) whenever both are free. Blocks are always aligned to
// their own size, which DMA-capable drivers will rely on later.
namespace pmm {

constexpr unsigned MAX_ORDER = 11;      // orders 0..10
constexpr u32 NO_PAGE = 0xFFFFFFFF;

enum PageFlags : u16 {
    PF_RESERVED   = 1 << 0, // not managed (firmware, MMIO hole, kernel image, ...)
    PF_FREE       = 1 << 1, // first page of a free block
    PF_ALLOCATED  = 1 << 2, // first page of an allocated block
    PF_SLAB       = 1 << 3, // owned by the kernel heap's slab allocator
    PF_HEAP_LARGE = 1 << 4, // multi-page kmalloc() block
    PF_PAGE_TABLE = 1 << 5, // holds a page table
};

// One entry per physical page frame ("page frame database").
struct PageFrame {
    u32 next;          // free-list / slab-list links (page frame numbers)
    u32 prev;
    u32 refcount;
    u16 flags;
    u8 order;          // block size of a free/allocated block head
    u8 slab_class;     // heap: size class of this slab
    u16 slab_inuse;    // heap: objects handed out from this slab
    u16 slab_free;     // heap: index of first free object, 0xFFFF = full
    u32 reserved;
};
static_assert(sizeof(PageFrame) == 24);

struct Stats {
    u64 total_pages;   // pages managed by the allocator
    u64 free_pages;
    u64 database_bytes;
};

void init();

// Give bootloader-reclaimable memory (Limine's page tables, stacks, info
// structures) to the allocator. Only safe once every CPU runs on kernel
// stacks and kernel page tables. Returns the number of bytes reclaimed.
u64 reclaim_bootloader_memory();

// Returns the physical address of a 2^order-page block, or 0 when out of memory.
// Physical page 0 is never handed out, so 0 is a safe failure value.
u64 alloc_pages(unsigned order, bool zero = false);
inline u64 alloc_page(bool zero = false) { return alloc_pages(0, zero); }
// The same, for a block that must end at or below `limit` (devices that can
// only address 32 bits, ...). Slower: it searches the free lists.
u64 alloc_pages_below(unsigned order, u64 limit, bool zero = false);
void free_pages(u64 phys, unsigned order);
inline void free_page(u64 phys) { free_pages(phys, 0); }

// Smallest order whose block holds `bytes`.
unsigned order_for_size(usize bytes);

// Page frame for a physical address, or nullptr if it isn't managed.
PageFrame* frame(u64 phys);
u64 frame_to_phys(const PageFrame* frame);

Stats stats();
void dump_free_lists();

} // namespace pmm
