#include <core/log.hpp>
#include <core/panic.hpp>
#include <lib/format.hpp>
#include <lib/spinlock.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <mm/vmalloc.hpp>

// The region is tracked with two simple sorted lists (free ranges, used
// areas). Fine for now; a balanced tree can replace it when user address
// spaces need the same thing at scale.

namespace vmalloc {

using mm::PAGE_SIZE;

namespace {

struct FreeRange {
    uptr base;
    usize size;
    FreeRange* next;
};

struct Area {
    uptr base;       // start of the reserved range (the guard page)
    usize size;      // whole reserved range, guard included
    uptr data;       // first mapped byte returned to the caller
    usize pages;     // mapped pages
    bool owns_pages; // false for map_physical(): don't free the frames
    Area* next;
};

FreeRange g_initial_range{mm::VMALLOC_START, mm::VMALLOC_END - mm::VMALLOC_START, nullptr};
FreeRange* g_free = &g_initial_range;
Area* g_areas = nullptr;
u64 g_area_count = 0;
u64 g_mapped_pages = 0;
IrqSpinlock g_lock;

// First-fit reservation of `size` bytes of address space. Lock held.
uptr reserve(usize size) {
    FreeRange** link = &g_free;
    for (FreeRange* r = g_free; r; link = &r->next, r = r->next) {
        if (r->size < size)
            continue;
        uptr base = r->base;
        r->base += size;
        r->size -= size;
        if (r->size == 0) {
            *link = r->next;
            if (r != &g_initial_range)
                kfree(r);
        }
        return base;
    }
    return 0;
}

// Return a range, merging with neighbors. Lock held.
void release(uptr base, usize size) {
    FreeRange* prev = nullptr;
    FreeRange* next = g_free;
    while (next && next->base < base) {
        prev = next;
        next = next->next;
    }

    bool merge_prev = prev && prev->base + prev->size == base;
    bool merge_next = next && base + size == next->base;

    if (merge_prev && merge_next) {
        prev->size += size + next->size;
        prev->next = next->next;
        if (next != &g_initial_range)
            kfree(next);
    } else if (merge_prev) {
        prev->size += size;
    } else if (merge_next) {
        next->base = base;
        next->size += size;
    } else {
        auto* r = static_cast<FreeRange*>(kmalloc(sizeof(FreeRange)));
        if (!r)
            panic("vmalloc: out of memory while freeing address space");
        *r = FreeRange{base, size, next};
        if (prev)
            prev->next = r;
        else
            g_free = r;
    }
}

Area* find_area(uptr data, Area*** link_out) {
    Area** link = &g_areas;
    for (Area* a = g_areas; a; link = &a->next, a = a->next) {
        if (a->data == data) {
            if (link_out)
                *link_out = link;
            return a;
        }
    }
    return nullptr;
}

void unmap_area(const Area* a) {
    auto& ks = vmm::AddressSpace::kernel();
    u64 phys_pages[64];
    // Unmap in batches; the frames are only freed once no CPU can still
    // reach them through a stale TLB entry.
    for (usize done = 0; done < a->pages;) {
        usize n = min<usize>(a->pages - done, array_size(phys_pages));
        for (usize i = 0; i < n; i++)
            phys_pages[i] = ks.unmap(a->data + (done + i) * PAGE_SIZE);
        vmm::shootdown(a->data + done * PAGE_SIZE, n);
        if (a->owns_pages) {
            for (usize i = 0; i < n; i++)
                if (phys_pages[i])
                    pmm::free_page(phys_pages[i]);
        }
        done += n;
    }
}

// Reserve address space + record an area; mapping is left to the caller.
Area* create_area(usize pages, bool owns_pages) {
    auto* area = static_cast<Area*>(kmalloc(sizeof(Area)));
    if (!area)
        return nullptr;
    usize size = (pages + 1) * PAGE_SIZE; // +1 guard page below the data

    LockGuard guard(g_lock);
    uptr base = reserve(size);
    if (!base) {
        kfree(area);
        return nullptr;
    }
    *area = Area{base, size, base + PAGE_SIZE, pages, owns_pages, g_areas};
    g_areas = area;
    g_area_count++;
    g_mapped_pages += pages;
    return area;
}

// The call chain of a bad free, for the log (frame pointers are kept).
void log_caller_chain() {
    char line[160];
    usize n = 0;
    const u64* fp = static_cast<const u64*>(__builtin_frame_address(0));
    for (int i = 0; i < 10 && reinterpret_cast<u64>(fp) >= 0xFFFF800000000000ull &&
                    (reinterpret_cast<u64>(fp) & 7) == 0 && n + 18 < sizeof(line); i++) {
        u64 ret = fp[1];
        if (ret < 0xFFFF800000000000ull)
            break;
        n += snprintf(line + n, sizeof(line) - n, " %016lx", ret);
        const u64* next = reinterpret_cast<const u64*>(fp[0]);
        if (next <= fp)
            break;
        fp = next;
    }
    line[n] = 0;
    klog::error("vmalloc:   called from:%s", line);
}

void destroy_area(void* ptr, bool expect_owned) {
    Area** link;
    Area* area;
    {
        LockGuard guard(g_lock);
        area = find_area(reinterpret_cast<uptr>(ptr), &link);
        // A bad free is a driver bug; Linux warns and carries on (the memory
        // stays mapped), it doesn't take the whole machine down.
        if (!area) {
            uptr p = reinterpret_cast<uptr>(ptr);
            const Area* in = nullptr;
            for (const Area* a = g_areas; a; a = a->next)
                if (p >= a->base && p < a->base + a->size)
                    in = a;
            if (in)
                klog::error("vmalloc: free of %p, inside the %s area at %#lx (%lu pages): ignored", ptr,
                            in->owns_pages ? "allocated" : "mapped", in->data, in->pages);
            else
                klog::error("vmalloc: free of unknown pointer %p (double free?): ignored", ptr);
            log_caller_chain();
            return;
        }
        if (area->owns_pages != expect_owned) {
            // vfree() of a mapping or unmap of an allocation: free it the way it was made.
            klog::warn("vmalloc: %p is %s, freed as %s", ptr, area->owns_pages ? "an allocation" : "a mapping",
                       expect_owned ? "an allocation" : "a mapping");
            log_caller_chain();
        }
        *link = area->next;
        g_area_count--;
        g_mapped_pages -= area->pages;
    }
    unmap_area(area);
    {
        LockGuard guard(g_lock);
        release(area->base, area->size);
    }
    kfree(area);
}

} // namespace

bool is_mapping(const void* ptr) {
    LockGuard guard(g_lock);
    Area* area = find_area(reinterpret_cast<uptr>(ptr), nullptr);
    return area && !area->owns_pages;
}

void* alloc(usize size) {
    if (size == 0)
        return nullptr;
    usize pages = align_up<usize>(size, PAGE_SIZE) / PAGE_SIZE;
    Area* area = create_area(pages, true);
    if (!area)
        return nullptr;

    auto& ks = vmm::AddressSpace::kernel();
    for (usize i = 0; i < pages; i++) {
        u64 phys = pmm::alloc_page(true);
        if (!phys || !ks.map(area->data + i * PAGE_SIZE, phys, vmm::WRITE | vmm::GLOBAL)) {
            if (phys)
                pmm::free_page(phys);
            area->pages = i; // only unmap what we mapped
            {
                LockGuard guard(g_lock);
                g_mapped_pages -= pages - i;
            }
            destroy_area(reinterpret_cast<void*>(area->data), true);
            return nullptr;
        }
    }
    return reinterpret_cast<void*>(area->data);
}

void free(void* ptr) {
    if (ptr)
        destroy_area(ptr, true);
}

usize size_of(const void* ptr) {
    LockGuard guard(g_lock);
    Area* area = find_area(reinterpret_cast<uptr>(ptr), nullptr);
    return area ? area->pages * PAGE_SIZE : 0;
}

void* map_physical(u64 phys, usize size, u32 flags) {
    u64 base = align_down(phys, PAGE_SIZE);
    usize offset = phys - base;
    usize pages = align_up<usize>(size + offset, PAGE_SIZE) / PAGE_SIZE;
    Area* area = create_area(pages, false);
    if (!area)
        return nullptr;

    auto& ks = vmm::AddressSpace::kernel();
    for (usize i = 0; i < pages; i++) {
        if (!ks.map(area->data + i * PAGE_SIZE, base + i * PAGE_SIZE, flags | vmm::GLOBAL))
            panic("vmalloc: failed to map physical %#lx", base + i * PAGE_SIZE);
    }
    return reinterpret_cast<void*>(area->data + offset);
}

void* map_pages(const u64* phys, usize count, u32 flags) {
    Area* area = create_area(count, false);
    if (!area)
        return nullptr;
    auto& ks = vmm::AddressSpace::kernel();
    for (usize i = 0; i < count; i++) {
        if (!ks.map(area->data + i * PAGE_SIZE, phys[i], flags | vmm::GLOBAL))
            panic("vmalloc: failed to map page %#lx", phys[i]);
    }
    return reinterpret_cast<void*>(area->data);
}

void unmap_physical(void* virt) {
    destroy_area(reinterpret_cast<void*>(align_down(reinterpret_cast<uptr>(virt), PAGE_SIZE)), false);
}

Stats stats() {
    LockGuard guard(g_lock);
    return Stats{g_area_count, g_mapped_pages};
}

} // namespace vmalloc
