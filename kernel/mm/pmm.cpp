#include <boot/limine_requests.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <lib/spinlock.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>

namespace pmm {

using mm::PAGE_SHIFT;
using mm::PAGE_SIZE;

namespace {

PageFrame* g_frames = nullptr;
u64 g_frame_count = 0;          // frames covered by the database (0 .. highest RAM address)
u32 g_free_head[MAX_ORDER];
u64 g_free_pages = 0;
u64 g_total_pages = 0;
u64 g_db_phys = 0;
u64 g_db_size = 0;
IrqSpinlock g_lock;

void list_push(u64 pfn, unsigned order) {
    PageFrame& f = g_frames[pfn];
    f.flags = PF_FREE;
    f.order = static_cast<u8>(order);
    f.prev = NO_PAGE;
    f.next = g_free_head[order];
    if (f.next != NO_PAGE)
        g_frames[f.next].prev = static_cast<u32>(pfn);
    g_free_head[order] = static_cast<u32>(pfn);
}

void list_remove(u64 pfn, unsigned order) {
    PageFrame& f = g_frames[pfn];
    if (f.prev != NO_PAGE)
        g_frames[f.prev].next = f.next;
    else
        g_free_head[order] = f.next;
    if (f.next != NO_PAGE)
        g_frames[f.next].prev = f.prev;
    f.next = f.prev = NO_PAGE;
    f.flags = 0;
}

// Insert a block, merging with its buddy as long as possible. Lock must be held.
void free_block(u64 pfn, unsigned order) {
    g_free_pages += 1ull << order;
    while (order < MAX_ORDER - 1) {
        u64 buddy = pfn ^ (1ull << order);
        if (buddy + (1ull << order) > g_frame_count)
            break;
        const PageFrame& b = g_frames[buddy];
        if (b.flags != PF_FREE || b.order != order)
            break;
        list_remove(buddy, order);
        pfn = min(pfn, buddy);
        order++;
    }
    list_push(pfn, order);
}

// Hand a page-aligned physical range [start, end) to the allocator.
void add_range(u64 start, u64 end) {
    if (start < PAGE_SIZE)
        start = PAGE_SIZE; // keep page 0 out: 0 means "allocation failed"
    u64 pfn = start >> PAGE_SHIFT;
    u64 end_pfn = end >> PAGE_SHIFT;

    while (pfn < end_pfn) {
        // Biggest block that is aligned at pfn and still fits.
        unsigned order = MAX_ORDER - 1;
        while (order > 0 && ((pfn & ((1ull << order) - 1)) || pfn + (1ull << order) > end_pfn))
            order--;
        for (u64 i = 0; i < (1ull << order); i++)
            g_frames[pfn + i].flags = 0;
        free_block(pfn, order);
        pfn += 1ull << order;
    }
}

bool is_ram(u64 type) {
    return type == LIMINE_MEMMAP_USABLE || type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
           type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES;
}

} // namespace

void init() {
    auto* mmap = boot::memory_map();
    KASSERT(mmap != nullptr);
    mm::g_hhdm_offset = boot::hhdm_offset();

    // 1. How many page frames do we need to describe?
    u64 highest = 0;
    for (u64 i = 0; i < mmap->entry_count; i++) {
        auto* e = mmap->entries[i];
        if (is_ram(e->type))
            highest = max(highest, e->base + e->length);
    }
    g_frame_count = align_up(highest, PAGE_SIZE) >> PAGE_SHIFT;
    if (g_frame_count >= NO_PAGE)
        panic("pmm: %lu GiB of physical address space is more than supported", highest / GiB);

    // 2. Find a usable region to hold the page frame database itself.
    g_db_size = align_up(g_frame_count * sizeof(PageFrame), PAGE_SIZE);
    for (u64 i = 0; i < mmap->entry_count && !g_db_phys; i++) {
        auto* e = mmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        u64 base = max(align_up(e->base, PAGE_SIZE), PAGE_SIZE);
        u64 end = align_down(e->base + e->length, PAGE_SIZE);
        if (end > base && end - base >= g_db_size)
            g_db_phys = base;
    }
    if (!g_db_phys)
        panic("pmm: no room for the page frame database (%lu KiB)", g_db_size / KiB);

    g_frames = mm::phys_to_virt<PageFrame>(g_db_phys);
    for (u64 i = 0; i < g_frame_count; i++)
        g_frames[i] = PageFrame{NO_PAGE, NO_PAGE, 0, PF_RESERVED, 0, 0, 0, 0, 0};
    for (auto& head : g_free_head)
        head = NO_PAGE;

    // 3. Free every usable page except the database.
    // Bootloader-reclaimable memory still holds our boot stack and Limine's
    // responses; it gets reclaimed once the scheduler gives us real stacks.
    u64 db_end = g_db_phys + g_db_size;
    for (u64 i = 0; i < mmap->entry_count; i++) {
        auto* e = mmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        u64 start = align_up(e->base, PAGE_SIZE);
        u64 end = align_down(e->base + e->length, PAGE_SIZE);
        if (start >= end)
            continue;
        if (db_end <= start || g_db_phys >= end) {
            add_range(start, end);
        } else {
            if (start < g_db_phys) add_range(start, g_db_phys);
            if (db_end < end) add_range(db_end, end);
        }
    }
    g_total_pages = g_free_pages;
}

u64 reclaim_bootloader_memory() {
    auto* mmap = boot::memory_map();
    if (!mmap)
        return 0;

    // The memory map itself lives in the memory we're about to free: copy first.
    struct Range { u64 start, end; };
    constexpr usize MAX_RANGES = 128;
    Range ranges[MAX_RANGES];
    usize count = 0;
    for (u64 i = 0; i < mmap->entry_count && count < MAX_RANGES; i++) {
        auto* e = mmap->entries[i];
        if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
            ranges[count++] = {align_up(e->base, PAGE_SIZE), align_down(e->base + e->length, PAGE_SIZE)};
    }
    boot::release();

    u64 bytes = 0;
    LockGuard guard(g_lock);
    u64 before = g_free_pages;
    for (usize i = 0; i < count; i++) {
        if (ranges[i].end > ranges[i].start && (ranges[i].end >> PAGE_SHIFT) <= g_frame_count)
            add_range(ranges[i].start, ranges[i].end);
    }
    bytes = (g_free_pages - before) * PAGE_SIZE;
    g_total_pages += g_free_pages - before;
    return bytes;
}

u64 alloc_pages(unsigned order, bool zero) {
    if (order >= MAX_ORDER)
        return 0;

    u64 pfn;
    {
        LockGuard guard(g_lock);
        unsigned k = order;
        while (k < MAX_ORDER && g_free_head[k] == NO_PAGE)
            k++;
        if (k == MAX_ORDER)
            return 0;

        pfn = g_free_head[k];
        list_remove(pfn, k);
        // Split down to the requested size; upper halves go back on the lists.
        while (k > order) {
            k--;
            list_push(pfn + (1ull << k), k);
        }

        PageFrame& f = g_frames[pfn];
        f.flags = PF_ALLOCATED;
        f.order = static_cast<u8>(order);
        f.refcount = 1;
        g_free_pages -= 1ull << order;
    }

    u64 phys = pfn << PAGE_SHIFT;
    if (zero)
        memset(mm::phys_to_virt(phys), 0, PAGE_SIZE << order);
    return phys;
}

void free_pages(u64 phys, unsigned order) {
    u64 pfn = phys >> PAGE_SHIFT;
    if ((phys & (PAGE_SIZE - 1)) || pfn >= g_frame_count || order >= MAX_ORDER)
        panic("pmm: free of invalid block %#lx (order %u)", phys, order);

    LockGuard guard(g_lock);
    PageFrame& f = g_frames[pfn];
    if (!(f.flags & PF_ALLOCATED) || f.order != order)
        panic("pmm: bad free of %#lx order %u (flags %#x, order %u) - double free?",
              phys, order, f.flags, f.order);
    f = PageFrame{NO_PAGE, NO_PAGE, 0, 0, 0, 0, 0, 0, 0};
    free_block(pfn, order);
}

unsigned order_for_size(usize bytes) {
    u64 pages = align_up<u64>(bytes, PAGE_SIZE) >> PAGE_SHIFT;
    unsigned order = 0;
    while ((1ull << order) < pages)
        order++;
    return order;
}

PageFrame* frame(u64 phys) {
    u64 pfn = phys >> PAGE_SHIFT;
    return pfn < g_frame_count ? &g_frames[pfn] : nullptr;
}

u64 frame_to_phys(const PageFrame* f) { return static_cast<u64>(f - g_frames) << PAGE_SHIFT; }

Stats stats() {
    LockGuard guard(g_lock);
    return Stats{g_total_pages, g_free_pages, g_db_size};
}

void dump_free_lists() {
    LockGuard guard(g_lock);
    kprintf("    free blocks by order:");
    for (unsigned order = 0; order < MAX_ORDER; order++) {
        u64 n = 0;
        for (u32 pfn = g_free_head[order]; pfn != NO_PAGE; pfn = g_frames[pfn].next)
            n++;
        kprintf(" %u:%lu", order, n);
    }
    kprintf("\n");
}

} // namespace pmm
