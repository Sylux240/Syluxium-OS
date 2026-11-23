#include <core/log.hpp>
#include <core/panic.hpp>
#include <lib/spinlock.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <mm/vmalloc.hpp>

// Slab design: each slab is one 4 KiB page holding objects of a single size.
// All bookkeeping lives in the page's PageFrame (not inside the page), so
// objects start at offset 0 and are naturally aligned to their size.
// Free objects form a chain: the first 2 bytes of a free object hold the
// index of the next free one.

namespace heap {

using mm::PAGE_SIZE;
using pmm::NO_PAGE;
using pmm::PageFrame;

namespace {

// Debug aids: fill freed objects with a pattern, detect writes after free
// and double frees. Costs some speed; worth it while the kernel is young.
constexpr bool HEAP_DEBUG = true;
constexpr u8 POISON = 0xDE;
constexpr u16 SLAB_FULL = 0xFFFF;

struct SizeClass {
    constexpr SizeClass(u32 size, u16 per_slab) : object_size(size), objects_per_slab(per_slab) {}

    u32 object_size;
    u16 objects_per_slab;
    u32 partial_head = NO_PAGE; // slabs with at least one free object
    u64 slabs = 0;
    u64 in_use = 0;
    u32 empty_slabs = 0;        // fully free slabs kept around (max 1)
    IrqSpinlock lock;
};

SizeClass g_classes[SIZE_CLASS_COUNT] = {
    {16, 256}, {32, 128}, {64, 64}, {128, 32}, {256, 16}, {512, 8}, {1024, 4}, {2048, 2},
};

unsigned class_for(usize size) {
    unsigned c = 0;
    while (g_classes[c].object_size < size)
        c++;
    return c;
}

PageFrame& frame_of(u32 pfn) { return *pmm::frame(static_cast<u64>(pfn) << mm::PAGE_SHIFT); }
u32 pfn_of(u64 phys) { return static_cast<u32>(phys >> mm::PAGE_SHIFT); }
u8* slab_base(u32 pfn) { return mm::phys_to_virt<u8>(static_cast<u64>(pfn) << mm::PAGE_SHIFT); }

void partial_push(SizeClass& sc, u32 pfn) {
    PageFrame& f = frame_of(pfn);
    f.prev = NO_PAGE;
    f.next = sc.partial_head;
    if (f.next != NO_PAGE)
        frame_of(f.next).prev = pfn;
    sc.partial_head = pfn;
}

void partial_remove(SizeClass& sc, u32 pfn) {
    PageFrame& f = frame_of(pfn);
    if (f.prev != NO_PAGE)
        frame_of(f.prev).next = f.next;
    else
        sc.partial_head = f.next;
    if (f.next != NO_PAGE)
        frame_of(f.next).prev = f.prev;
    f.next = f.prev = NO_PAGE;
}

bool new_slab(SizeClass& sc, unsigned class_index) {
    u64 phys = pmm::alloc_page();
    if (!phys)
        return false;
    u32 pfn = pfn_of(phys);
    PageFrame& f = frame_of(pfn);
    f.flags |= pmm::PF_SLAB;
    f.slab_class = static_cast<u8>(class_index);
    f.slab_inuse = 0;
    f.slab_free = 0;

    u8* base = slab_base(pfn);
    if (HEAP_DEBUG)
        memset(base, POISON, PAGE_SIZE);
    for (u16 i = 0; i < sc.objects_per_slab; i++) {
        u16 next = (i + 1 < sc.objects_per_slab) ? static_cast<u16>(i + 1) : SLAB_FULL;
        memcpy(base + i * sc.object_size, &next, sizeof(next));
    }
    partial_push(sc, pfn);
    sc.slabs++;
    sc.empty_slabs++;
    return true;
}

// Who freed an object: objects of 64 bytes and more keep the return
// addresses of the kfree() call chain in their bytes 8..55 while free, so a
// write after free can name the code that freed the object.
constexpr u32 TRACE_AT = 8, TRACE_DEPTH = 6;

bool in_free_trace(u32 size, u32 i) { return size >= 64 && i >= TRACE_AT && i < TRACE_AT + TRACE_DEPTH * 8; }

void record_free_trace(u8* obj, u32 size) {
    if (size < 64)
        return;
    u64 trace[TRACE_DEPTH] = {};
    auto* fp = static_cast<u64*>(__builtin_frame_address(0));
    // Walk only while the frame pointers look like a kernel stack: aligned,
    // growing, close together (the outermost frames hold user values).
    for (u32 i = 0; i < TRACE_DEPTH; i++) {
        u64 f = reinterpret_cast<u64>(fp);
        if (f < mm::KERNEL_HALF || f > ~0xFFFFull || (f & 7))
            break;
        trace[i] = fp[1];
        auto* next = reinterpret_cast<u64*>(fp[0]);
        if (next <= fp || reinterpret_cast<u64>(next) - f > 64 * 1024 || trace[i] < mm::KERNEL_HALF)
            break;
        fp = next;
    }
    memcpy(obj + TRACE_AT, trace, sizeof(trace));
}

[[noreturn]] void report_write_after_free(u8* obj, u32 size, u32 first) {
    u32 last = first;
    for (u32 i = first; i < size; i++)
        if (!in_free_trace(size, i) && obj[i] != POISON)
            last = i;
    klog::warn("heap: write after free in %p (size %u): bytes %u..%u changed, first ones %02x %02x %02x %02x "
               "%02x %02x %02x %02x", obj, size, first, last, obj[first], obj[first + 1], obj[first + 2],
               obj[first + 3], obj[first + 4], obj[first + 5], obj[first + 6], obj[first + 7]);
    if (size >= 64) {
        u64 trace[TRACE_DEPTH];
        memcpy(trace, obj + TRACE_AT, sizeof(trace));
        klog::warn("heap: it was freed by %#lx %#lx %#lx %#lx %#lx %#lx", trace[0], trace[1], trace[2], trace[3],
                   trace[4], trace[5]);
    }
    panic("heap: object %p (size %u) was written after being freed", obj, size);
}

void* slab_alloc(unsigned class_index) {
    SizeClass& sc = g_classes[class_index];
    LockGuard guard(sc.lock);

    if (sc.partial_head == NO_PAGE && !new_slab(sc, class_index))
        return nullptr;

    u32 pfn = sc.partial_head;
    PageFrame& f = frame_of(pfn);
    u8* obj = slab_base(pfn) + f.slab_free * sc.object_size;

    if (HEAP_DEBUG) {
        for (u32 i = 2; i < sc.object_size; i++) {
            if (in_free_trace(sc.object_size, i))
                continue;
            if (obj[i] != POISON)
                report_write_after_free(obj, sc.object_size, i);
        }
    }

    if (f.slab_inuse == 0)
        sc.empty_slabs--;
    memcpy(&f.slab_free, obj, sizeof(u16));
    f.slab_inuse++;
    sc.in_use++;
    if (f.slab_free == SLAB_FULL)
        partial_remove(sc, pfn);
    return obj;
}

void slab_free(void* ptr, PageFrame& f) {
    SizeClass& sc = g_classes[f.slab_class];
    u32 pfn = pfn_of(pmm::frame_to_phys(&f));
    usize offset = static_cast<u8*>(ptr) - slab_base(pfn);
    if (offset % sc.object_size)
        panic("heap: kfree(%p) is not the start of an object", ptr);
    u16 index = static_cast<u16>(offset / sc.object_size);

    LockGuard guard(sc.lock);
    if (HEAP_DEBUG) {
        for (u16 i = f.slab_free; i != SLAB_FULL;) {
            if (i == index)
                panic("heap: double free of %p", ptr);
            memcpy(&i, slab_base(pfn) + i * sc.object_size, sizeof(u16));
        }
        memset(ptr, POISON, sc.object_size);
        record_free_trace(static_cast<u8*>(ptr), sc.object_size);
    }

    bool was_full = f.slab_free == SLAB_FULL;
    memcpy(ptr, &f.slab_free, sizeof(u16));
    f.slab_free = index;
    f.slab_inuse--;
    sc.in_use--;
    if (was_full)
        partial_push(sc, pfn);

    if (f.slab_inuse == 0) {
        // Keep one empty slab per class to avoid thrashing the page allocator.
        if (sc.empty_slabs >= 1) {
            partial_remove(sc, pfn);
            f.flags &= ~pmm::PF_SLAB;
            sc.slabs--;
            pmm::free_page(static_cast<u64>(pfn) << mm::PAGE_SHIFT);
        } else {
            sc.empty_slabs++;
        }
    }
}

} // namespace

ClassStats class_stats(usize index) {
    SizeClass& sc = g_classes[index];
    LockGuard guard(sc.lock);
    return ClassStats{sc.object_size, sc.slabs, sc.in_use};
}

u64 g_large_pages = 0; // pages in large (multi-page) kmalloc blocks

u64 kernel_heap_pages() {
    u64 n = __atomic_load_n(&g_large_pages, __ATOMIC_RELAXED);
    for (usize i = 0; i < SIZE_CLASS_COUNT; i++)
        n += class_stats(i).slabs; // one page per slab
    return n;
}

void dump_stats() {
    kprintf("    slab classes (size: objects in use / slabs):");
    for (usize i = 0; i < SIZE_CLASS_COUNT; i++) {
        auto s = class_stats(i);
        kprintf(" %u:%lu/%lu", s.object_size, s.objects_in_use, s.slabs);
    }
    kprintf("\n");
}

} // namespace heap

void* kmalloc(usize size) {
    if (size == 0)
        size = 1; // like malloc(0): a unique, freeable pointer
    if (size <= heap::MAX_SLAB_OBJECT)
        return heap::slab_alloc(heap::class_for(size));

    unsigned order = pmm::order_for_size(size);
    if (order < pmm::MAX_ORDER) {
        u64 phys = pmm::alloc_pages(order);
        if (phys) {
            pmm::frame(phys)->flags |= pmm::PF_HEAP_LARGE;
            __atomic_add_fetch(&heap::g_large_pages, 1ull << order, __ATOMIC_RELAXED);
            return mm::phys_to_virt(phys);
        }
        // Physically contiguous memory too fragmented: fall back to vmalloc.
    }
    return vmalloc::alloc(size);
}

void* kzalloc(usize size) {
    void* p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void* kcalloc(usize count, usize size) {
    usize total;
    if (__builtin_mul_overflow(count, size, &total))
        return nullptr;
    return kzalloc(total);
}

usize ksize(const void* ptr) {
    if (!ptr)
        return 0;
    if (mm::is_vmalloc_address(ptr))
        return vmalloc::size_of(ptr);
    pmm::PageFrame* f = pmm::frame(mm::direct_virt_to_phys(ptr));
    if (f && (f->flags & pmm::PF_SLAB))
        return heap::g_classes[f->slab_class].object_size;
    if (f && (f->flags & pmm::PF_HEAP_LARGE))
        return mm::PAGE_SIZE << f->order;
    panic("ksize: %p is not a heap pointer", ptr);
}

void* krealloc(void* ptr, usize new_size) {
    if (!ptr)
        return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return nullptr;
    }
    usize old_size = ksize(ptr);
    if (new_size <= old_size)
        return ptr;
    void* p = kmalloc(new_size);
    if (!p)
        return nullptr;
    memcpy(p, ptr, old_size);
    kfree(ptr);
    return p;
}

void kfree(void* ptr) {
    if (!ptr)
        return;
    if (mm::is_vmalloc_address(ptr)) {
        vmalloc::free(ptr);
        return;
    }
    if (!mm::is_direct_mapped(ptr))
        panic("kfree: %p is not a heap pointer", ptr);

    u64 phys = mm::direct_virt_to_phys(ptr);
    pmm::PageFrame* f = pmm::frame(phys);
    if (f && (f->flags & pmm::PF_SLAB)) {
        heap::slab_free(ptr, *f);
    } else if (f && (f->flags & pmm::PF_HEAP_LARGE) && (phys & (mm::PAGE_SIZE - 1)) == 0) {
        f->flags &= ~pmm::PF_HEAP_LARGE;
        __atomic_sub_fetch(&heap::g_large_pages, 1ull << f->order, __ATOMIC_RELAXED);
        pmm::free_pages(phys, f->order);
    } else {
        panic("kfree: %p is not a heap pointer (double free?)", ptr);
    }
}
