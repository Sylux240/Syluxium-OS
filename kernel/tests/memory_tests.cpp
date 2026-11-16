#include <core/log.hpp>
#include <core/panic.hpp>
#include <cxx/runtime.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <mm/vmalloc.hpp>
#include <mm/vmm.hpp>
#include <tests/selftest.hpp>

namespace selftest {

namespace {

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond))                                                               \
            panic("self-test failed: %s\n  at %s:%d", #cond, __FILE__, __LINE__);  \
    } while (0)

struct Rng {
    u64 state;
    u64 next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    u64 below(u64 n) { return next() % n; }
};

u64 free_pages() { return pmm::stats().free_pages; }

// Fill / verify a buffer with a pattern derived from a seed.
void fill(void* p, usize n, u64 seed) {
    auto b = static_cast<u8*>(p);
    for (usize i = 0; i < n; i++)
        b[i] = static_cast<u8>((seed * 31 + i * 7) ^ (i >> 8));
}
bool verify(const void* p, usize n, u64 seed) {
    auto b = static_cast<const u8*>(p);
    for (usize i = 0; i < n; i++)
        if (b[i] != static_cast<u8>((seed * 31 + i * 7) ^ (i >> 8)))
            return false;
    return true;
}

void test_pmm() {
    u64 before = free_pages();

    // Many single pages: distinct, writable, and all returned afterwards.
    constexpr int N = 2000;
    static u64 pages[N];
    for (int i = 0; i < N; i++) {
        pages[i] = pmm::alloc_page();
        CHECK(pages[i] != 0 && (pages[i] & (mm::PAGE_SIZE - 1)) == 0);
        *mm::phys_to_virt<u64>(pages[i]) = pages[i] ^ 0x5A5A5A5A5A5A5A5A;
    }
    for (int i = 0; i < N; i++)
        CHECK(*mm::phys_to_virt<u64>(pages[i]) == (pages[i] ^ 0x5A5A5A5A5A5A5A5A));
    CHECK(free_pages() == before - N);
    // Free in a scrambled order to exercise buddy merging.
    for (int i = 0; i < N; i += 2) pmm::free_page(pages[i]);
    for (int i = N - 1; i > 0; i -= 2) pmm::free_page(pages[i]);
    CHECK(free_pages() == before);

    // Largest block: 4 MiB, naturally aligned, zeroed on request.
    u64 big = pmm::alloc_pages(pmm::MAX_ORDER - 1, true);
    CHECK(big != 0 && (big & (4 * MiB - 1)) == 0);
    auto* words = mm::phys_to_virt<u64>(big);
    for (usize i = 0; i < 4 * MiB / 8; i += 512)
        CHECK(words[i] == 0);
    pmm::free_pages(big, pmm::MAX_ORDER - 1);
    CHECK(free_pages() == before);

    klog::ok("  pmm: %d page alloc/free, 4 MiB aligned block, buddy merging", N);
}

void test_vmm() {
    auto& ks = vmm::AddressSpace::kernel();
    u64 phys = pmm::alloc_page(true);
    CHECK(phys);
    uptr virt = mm::VMALLOC_END - 64 * mm::PAGE_SIZE; // far from vmalloc's first-fit area

    CHECK(ks.map(virt, phys, vmm::WRITE));
    CHECK(!ks.map(virt, phys, vmm::WRITE)); // double map refused
    u64 found = 0;
    CHECK(ks.translate(virt, &found) && found == phys);

    // Same memory through two different virtual addresses.
    *reinterpret_cast<volatile u64*>(virt) = 0xC0FFEE;
    CHECK(*mm::phys_to_virt<volatile u64>(phys) == 0xC0FFEE);

    CHECK(ks.unmap(virt) == phys);
    CHECK(!ks.translate(virt, &found));
    pmm::free_page(phys);

    // Direct map and kernel image translate to the right physical pages.
    u64 kphys;
    CHECK(ks.translate(reinterpret_cast<uptr>(&test_vmm), &kphys));
    CHECK(ks.translate(reinterpret_cast<uptr>(mm::phys_to_virt(phys)), &found) && found == phys);

    klog::ok("  vmm: map/translate/unmap, aliasing, double-map rejection");
}

void test_vmalloc() {
    u64 before = free_pages();
    usize size = 6 * MiB + 123; // bigger than the largest buddy block
    auto* p = static_cast<u8*>(vmalloc::alloc(size));
    CHECK(p && mm::is_vmalloc_address(p));
    CHECK(vmalloc::size_of(p) == align_up<usize>(size, mm::PAGE_SIZE));
    fill(p, size, 42);
    CHECK(verify(p, size, 42));

    // The guard page below the block must be unmapped.
    u64 phys;
    CHECK(!vmm::AddressSpace::kernel().translate(reinterpret_cast<uptr>(p) - 1, &phys));
    vmalloc::free(p);

    // MMIO-style mapping of a known physical page.
    u64 page = pmm::alloc_page(true);
    *mm::phys_to_virt<u32>(page + 0x10) = 0xFEEDBEEF;
    auto* mapped = static_cast<volatile u32*>(vmalloc::map_physical(page + 0x10, 4));
    CHECK(*mapped == 0xFEEDBEEF);
    vmalloc::unmap_physical(const_cast<u32*>(mapped));
    pmm::free_page(page);

    // Page tables created on the way may stay, everything else must be back.
    CHECK(free_pages() + 8 >= before);
    klog::ok("  vmalloc: 6 MiB block, guard page, physical mapping");
}

// One round of random kmalloc/kfree traffic.
void heap_round(Rng& rng) {
    constexpr int N = 3000;
    static void* ptrs[N];
    static usize sizes[N];

    for (int i = 0; i < N; i++) {
        u64 r = rng.below(100);
        usize size = r < 70 ? 1 + rng.below(256)        // mostly small
                   : r < 95 ? 257 + rng.below(4096)     // medium
                   : r < 99 ? 4097 + rng.below(64 * KiB)
                            : 1 * MiB + rng.below(MiB); // occasional big one
        ptrs[i] = kmalloc(size);
        sizes[i] = size;
        CHECK(ptrs[i] != nullptr);
        CHECK(ksize(ptrs[i]) >= size);
        // Size-class alignment: up to 4 KiB for blocks that big.
        usize align = 16;
        while (align < size && align < mm::PAGE_SIZE) align <<= 1;
        CHECK((reinterpret_cast<uptr>(ptrs[i]) & (align - 1)) == 0);
        fill(ptrs[i], size, i);
    }
    for (int i = 0; i < N; i++)
        CHECK(verify(ptrs[i], sizes[i], i));

    // krealloc keeps the contents.
    for (int i = 0; i < N; i += 97) {
        ptrs[i] = krealloc(ptrs[i], sizes[i] * 3);
        CHECK(ptrs[i] && verify(ptrs[i], sizes[i], i));
    }

    // Free in random order.
    for (int i = N - 1; i > 0; i--) {
        int j = static_cast<int>(rng.below(i + 1));
        void* tp = ptrs[i]; ptrs[i] = ptrs[j]; ptrs[j] = tp;
    }
    for (int i = 0; i < N; i++)
        kfree(ptrs[i]);
}

void test_heap() {
    Rng rng{0x5EED1234ABCDull};
    heap_round(rng); // warm-up: lets slab caches and page tables settle

    u64 before = free_pages();
    heap_round(rng);
    u64 after = free_pages();
    // Each size class may keep one empty slab cached.
    CHECK(after + heap::SIZE_CLASS_COUNT >= before);

    for (usize i = 0; i < heap::SIZE_CLASS_COUNT; i++)
        CHECK(heap::class_stats(i).objects_in_use == 0);

    klog::ok("  heap: 6000 random kmalloc/krealloc/kfree, no leaks");
}

struct Base {
    static inline int destroyed = 0;
    virtual ~Base() { destroyed++; }
    virtual int value() const { return 1; }
};
struct Derived : Base {
    int payload[10] = {};
    int value() const override { return 2; }
};
struct alignas(64) CacheLine {
    u8 bytes[64];
};

void test_cxx() {
    Base* b = new Derived;
    CHECK(b->value() == 2);
    delete b; // virtual destructor through the base pointer
    CHECK(Base::destroyed == 1);

    auto* arr = new int[1000];
    for (int i = 0; i < 1000; i++) arr[i] = i;
    CHECK(arr[999] == 999);
    delete[] arr;

    auto* line = new CacheLine; // over-aligned type: uses aligned operator new
    CHECK((reinterpret_cast<uptr>(line) & 63) == 0);
    delete line;

    klog::ok("  c++: new/delete, virtual destructors, arrays, alignas(64)");
}

} // namespace

void run_memory_tests() {
    klog::info("Running memory self-tests...");
    test_pmm();
    test_vmm();
    test_vmalloc();
    test_heap();
    test_cxx();
    klog::ok("All memory self-tests passed");
}

} // namespace selftest
