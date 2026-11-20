// LinuxKPI page model, C++ side: physical page blocks and kernel mappings
// for glue/mm.c. Kept apart from core.cpp because the Linux headers turn
// "vmalloc" into a macro.
#include <lib/types.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <mm/vmalloc.hpp>
#include <mm/vmm.hpp>

// ---- Pages for the Linux page model (glue/mm.c) ----
// Physical page blocks from the buddy allocator, used through the direct map.
extern "C" void* lkpi_phys_alloc(unsigned order, bool zero) {
    u64 phys = pmm::alloc_pages(order, zero);
    return phys ? mm::phys_to_virt(phys) : nullptr;
}
extern "C" void lkpi_phys_free(void* virt, unsigned order) {
    pmm::free_pages(mm::direct_virt_to_phys(virt), order);
}
extern "C" u64 lkpi_direct_to_phys(const void* virt) { return mm::direct_virt_to_phys(virt); }
extern "C" void* lkpi_phys_to_direct(u64 phys) { return mm::phys_to_virt(phys); }
// wc: write-combining (scan-out buffers), otherwise normal cached memory.
extern "C" void* lkpi_vmap_phys(const u64* phys, unsigned long count, bool wc) {
    return vmalloc::map_pages(phys, count, vmm::WRITE | (wc ? vmm::CACHE_WC : 0));
}
extern "C" void lkpi_vunmap(const void* virt) { vmalloc::unmap_physical(const_cast<void*>(virt)); }

// ---- GPU drivers (glue/gpu.c) ----
extern "C" u64 lkpi_kernel_virt_to_phys(const void* virt) {
    u64 phys = 0;
    if (!vmm::AddressSpace::kernel().translate(reinterpret_cast<uptr>(virt) & ~0xFFFull, &phys))
        return 0;
    return phys + (reinterpret_cast<uptr>(virt) & 0xFFF);
}
extern "C" bool is_vmalloc_addr(const void* virt) {
    uptr v = reinterpret_cast<uptr>(virt);
    return v >= mm::VMALLOC_START && v < mm::VMALLOC_END;
}
extern "C" unsigned long lkpi_totalram_pages(void) { return pmm::stats().total_pages; }
extern "C" unsigned long lkpi_freeram_pages(void) { return pmm::stats().free_pages; }
extern "C" void* ioremap_wc(u64 phys, usize size) {
    u64 base = phys & ~0xFFFull;
    void* v = vmalloc::map_physical(base, size + (phys - base), vmm::WRITE | vmm::CACHE_WC);
    return v ? static_cast<u8*>(v) + (phys - base) : nullptr;
}
extern "C" void* ioremap_cache(u64 phys, usize size) {
    u64 base = phys & ~0xFFFull;
    void* v = vmalloc::map_physical(base, size + (phys - base), vmm::WRITE);
    return v ? static_cast<u8*>(v) + (phys - base) : nullptr;
}
