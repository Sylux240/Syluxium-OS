#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <arch/x86_64/paging.hpp>
#include <boot/limine_requests.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <mm/vmm.hpp>

// Linker script symbols marking the kernel's sections.
extern "C" char __kernel_start[], __text_start[], __text_end[];
extern "C" char __rodata_start[], __rodata_end[], __data_start[], __data_end[];

namespace vmm {

namespace {
void shootdown_handler(interrupts::Frame*);
}

using namespace paging;
using mm::PAGE_SIZE;

namespace {

AddressSpace g_kernel_space;
bool g_nx = false;
bool g_1g_pages = false;
u64 g_table_pages = 0;

unsigned table_index(uptr virt, int level) { return (virt >> (12 + 9 * (level - 1))) & 0x1FF; }

u64 page_bytes(PageSize size) {
    switch (size) {
    case PageSize::Size1G: return mm::HUGE_PAGE_SIZE;
    case PageSize::Size2M: return mm::LARGE_PAGE_SIZE;
    default:               return PAGE_SIZE;
    }
}

int page_level(PageSize size) {
    switch (size) {
    case PageSize::Size1G: return 3;
    case PageSize::Size2M: return 2;
    default:               return 1;
    }
}

u64 alloc_table() {
    u64 phys = pmm::alloc_page(true);
    if (phys) {
        pmm::frame(phys)->flags |= pmm::PF_PAGE_TABLE;
        __atomic_add_fetch(&g_table_pages, 1, __ATOMIC_RELAXED);
    }
    return phys;
}

u64 leaf_entry(u64 phys, u32 flags, PageSize size) {
    u64 e = phys | PTE_PRESENT;
    if (flags & WRITE)  e |= PTE_WRITABLE;
    if (flags & USER)   e |= PTE_USER;
    if (flags & GLOBAL) e |= PTE_GLOBAL;
    if (!(flags & EXEC) && g_nx) e |= PTE_NX;
    if (size != PageSize::Size4K) e |= PTE_HUGE;

    u64 pat_bit = size == PageSize::Size4K ? PTE_PAT_4K : PTE_PAT_LARGE;
    if (flags & CACHE_WC)      e |= pat_bit | PTE_PWT;  // PAT index 5
    else if (flags & CACHE_UC) e |= PTE_PCD | PTE_PWT;  // PAT index 3
    else if (flags & CACHE_WT) e |= PTE_PWT;            // PAT index 1
    return e;
}

// Walk any page table hierarchy (also the bootloader's) to translate an address.
bool translate_in(u64 root, uptr virt, u64* phys_out) {
    const u64* table = mm::phys_to_virt<u64>(root);
    for (int level = 4; level >= 1; level--) {
        u64 e = table[table_index(virt, level)];
        if (!(e & PTE_PRESENT))
            return false;
        if (level == 1 || (e & PTE_HUGE)) {
            u64 size = 1ull << (12 + 9 * (level - 1));
            // For huge pages this mask also strips the PAT bit (bit 12).
            u64 base = (e & PTE_ADDR_MASK) & ~(size - 1);
            *phys_out = base + (virt & (size - 1));
            return true;
        }
        table = mm::phys_to_virt<u64>(e & PTE_ADDR_MASK);
    }
    return false;
}

} // namespace

AddressSpace& AddressSpace::kernel() { return g_kernel_space; }

// Returns the entry for `virt` at `target_level` (1 = PT, 2 = PD, 3 = PDPT),
// creating intermediate tables if asked. nullptr if a huge page is in the way.
u64* AddressSpace::walk(uptr virt, int target_level, bool create) {
    u64* table = mm::phys_to_virt<u64>(m_pml4);
    for (int level = 4; level > target_level; level--) {
        u64& e = table[table_index(virt, level)];
        if (!(e & PTE_PRESENT)) {
            if (!create)
                return nullptr;
            u64 phys = alloc_table();
            if (!phys)
                return nullptr;
            // Intermediate levels are permissive; the leaf decides the real rights.
            e = phys | PTE_PRESENT | PTE_WRITABLE | (virt < mm::USER_END ? PTE_USER : 0);
        } else if (e & PTE_HUGE) {
            return nullptr;
        }
        table = mm::phys_to_virt<u64>(e & PTE_ADDR_MASK);
    }
    return &table[table_index(virt, target_level)];
}

bool AddressSpace::map(uptr virt, u64 phys, u32 flags, PageSize size) {
    u64 bytes = page_bytes(size);
    KASSERT(((virt | phys) & (bytes - 1)) == 0);
    if (size == PageSize::Size1G && !g_1g_pages)
        return false;

    LockGuard guard(m_lock);
    u64* entry = walk(virt, page_level(size), true);
    if (!entry || (*entry & PTE_PRESENT))
        return false;
    *entry = leaf_entry(phys, flags, size);
    return true;
}

bool AddressSpace::map_range(uptr virt, u64 phys, usize length, u32 flags) {
    KASSERT(((virt | phys | length) & (PAGE_SIZE - 1)) == 0);
    while (length) {
        PageSize size = PageSize::Size4K;
        if (g_1g_pages && ((virt | phys) & (mm::HUGE_PAGE_SIZE - 1)) == 0 && length >= mm::HUGE_PAGE_SIZE)
            size = PageSize::Size1G;
        else if (((virt | phys) & (mm::LARGE_PAGE_SIZE - 1)) == 0 && length >= mm::LARGE_PAGE_SIZE)
            size = PageSize::Size2M;

        if (!map(virt, phys, flags, size))
            return false;
        u64 bytes = page_bytes(size);
        virt += bytes;
        phys += bytes;
        length -= bytes;
    }
    return true;
}

u64 AddressSpace::unmap(uptr virt) {
    LockGuard guard(m_lock);
    u64* table = mm::phys_to_virt<u64>(m_pml4);
    for (int level = 4; level > 1; level--) {
        u64 e = table[table_index(virt, level)];
        if (!(e & PTE_PRESENT))
            return 0;
        if (e & PTE_HUGE)
            panic("vmm: unmap of %p inside a huge page is not supported", reinterpret_cast<void*>(virt));
        table = mm::phys_to_virt<u64>(e & PTE_ADDR_MASK);
    }
    u64& e = table[table_index(virt, 1)];
    if (!(e & PTE_PRESENT))
        return 0;
    u64 phys = e & PTE_ADDR_MASK;
    e = 0;
    arch::invlpg(virt); // other CPUs: caller follows up with vmm::shootdown()
    return phys;
}

u64 AddressSpace::replace(uptr virt, u64 phys, u32 flags) {
    LockGuard guard(m_lock);
    u64* table = mm::phys_to_virt<u64>(m_pml4);
    for (int level = 4; level > 1; level--) {
        u64 e = table[table_index(virt, level)];
        if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
            return 0;
        table = mm::phys_to_virt<u64>(e & PTE_ADDR_MASK);
    }
    u64& e = table[table_index(virt, 1)];
    if (!(e & PTE_PRESENT))
        return 0;
    u64 old = e & PTE_ADDR_MASK;
    __atomic_store_n(&e, leaf_entry(phys, flags, PageSize::Size4K), __ATOMIC_RELEASE);
    arch::invlpg(virt);
    return old;
}

AddressSpace* AddressSpace::create_user() {
    auto* as = new AddressSpace;
    as->m_pml4 = alloc_table();
    if (!as->m_pml4) {
        delete as;
        return nullptr;
    }
    u64* dst = mm::phys_to_virt<u64>(as->m_pml4);
    const u64* src = mm::phys_to_virt<u64>(g_kernel_space.m_pml4);
    for (int i = 256; i < 512; i++)
        dst[i] = src[i]; // kernel half: same PDPTs everywhere
    return as;
}

namespace {

void free_table_page(u64 phys) {
    pmm::frame(phys)->flags &= ~pmm::PF_PAGE_TABLE;
    pmm::free_page(phys);
    __atomic_sub_fetch(&g_table_pages, 1, __ATOMIC_RELAXED);
}

// Drop one reference to a mapped user page; free it on the last one.
void put_user_page(u64 phys) {
    pmm::PageFrame* f = pmm::frame(phys);
    if (!f || !(f->flags & pmm::PF_ALLOCATED))
        return; // not RAM we own (e.g. a device mapping)
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) == 0)
        pmm::free_page(phys);
}

} // namespace

void AddressSpace::destroy_user() {
    KASSERT(this != &g_kernel_space);
    u64* pml4 = mm::phys_to_virt<u64>(m_pml4);
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_PRESENT))
            continue;
        u64* pdpt = mm::phys_to_virt<u64>(pml4[i4] & PTE_ADDR_MASK);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE))
                continue;
            u64* pd = mm::phys_to_virt<u64>(pdpt[i3] & PTE_ADDR_MASK);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE))
                    continue;
                u64* pt = mm::phys_to_virt<u64>(pd[i2] & PTE_ADDR_MASK);
                for (int i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] & PTE_PRESENT)
                        put_user_page(pt[i1] & PTE_ADDR_MASK);
                }
                free_table_page(pd[i2] & PTE_ADDR_MASK);
            }
            free_table_page(pdpt[i3] & PTE_ADDR_MASK);
        }
        free_table_page(pml4[i4] & PTE_ADDR_MASK);
        pml4[i4] = 0;
    }
    free_table_page(m_pml4);
    m_pml4 = 0;
}

bool AddressSpace::protect(uptr virt, u32 flags) {
    LockGuard guard(m_lock);
    u64* table = mm::phys_to_virt<u64>(m_pml4);
    for (int level = 4; level > 1; level--) {
        u64 e = table[table_index(virt, level)];
        if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
            return false;
        table = mm::phys_to_virt<u64>(e & PTE_ADDR_MASK);
    }
    u64& e = table[table_index(virt, 1)];
    if (!(e & PTE_PRESENT))
        return false;
    // Keep the cache type (device mappings: write-combining / uncached).
    e = leaf_entry(e & PTE_ADDR_MASK, flags, PageSize::Size4K) | (e & (PTE_PWT | PTE_PCD | PTE_PAT_4K));
    arch::invlpg(virt);
    return true;
}

namespace {
void flush_local(uptr start, usize pages); // below
// Replace the huge page `e` (level 3: 1 GiB, level 2: 2 MiB) by a table of
// 512 entries mapping the same memory with the same rights and cache type.
bool split_huge(u64& e, int level) {
    u64 table = alloc_table();
    if (!table)
        return false;
    u64* t = mm::phys_to_virt<u64>(table);
    u64 child = 1ull << (12 + 9 * (level - 2)); // what each new entry maps
    u64 base = (e & PTE_ADDR_MASK) & ~((child << 9) - 1);
    u64 low = e & 0xFFF & ~PTE_HUGE;            // present, rw, user, PWT, PCD, accessed, dirty, global
    bool pat = e & PTE_PAT_LARGE;
    u64 nx = e & PTE_NX;
    for (int i = 0; i < 512; i++) {
        if (level == 3) // 2 MiB entries: still huge, PAT stays at bit 12
            t[i] = (base + i * child) | low | PTE_HUGE | (pat ? PTE_PAT_LARGE : 0) | nx;
        else            // 4 KiB entries: PAT moves to bit 7
            t[i] = (base + i * child) | low | (pat ? PTE_PAT_4K : 0) | nx;
    }
    // Intermediate levels are permissive (as walk() makes them); one store,
    // so other CPUs see either the old huge page or the new table.
    __atomic_store_n(&e, table | PTE_PRESENT | PTE_WRITABLE | (e & PTE_USER), __ATOMIC_RELEASE);
    return true;
}
} // namespace

bool AddressSpace::set_cache(uptr virt, usize pages, u32 cache) {
    LockGuard guard(m_lock);
    for (usize n = 0; n < pages; n++, virt += PAGE_SIZE) {
        u64* table = mm::phys_to_virt<u64>(m_pml4);
        for (int level = 4; level > 1; level--) {
            u64& e = table[table_index(virt, level)];
            if (!(e & PTE_PRESENT))
                return false;
            if ((e & PTE_HUGE) && !split_huge(e, level))
                return false;
            table = mm::phys_to_virt<u64>(e & PTE_ADDR_MASK);
        }
        u64& e = table[table_index(virt, 1)];
        if (!(e & PTE_PRESENT))
            return false;
        u64 v = e & ~(PTE_PWT | PTE_PCD | PTE_PAT_4K);
        if (cache & CACHE_WC)
            v |= PTE_PAT_4K | PTE_PWT; // PAT index 5
        else if (cache & CACHE_UC)
            v |= PTE_PCD | PTE_PWT;    // PAT index 3
        e = v;
    }
    return true;
}

bool set_kernel_cache(uptr virt, usize pages, u32 cache) {
    if (!pages)
        return true;
    virt &= ~(PAGE_SIZE - 1);
    bool ok = g_kernel_space.set_cache(virt, pages, cache);
    flush_local(virt, pages);
    shootdown(virt, pages);
    // Cache lines filled through the old write-back mapping must not linger
    // (the GPU wouldn't see data still sitting in them).
    // clflush reaches every CPU's caches (wbinvd only this one's).
    if (cache & (CACHE_WC | CACHE_UC)) {
        for (uptr p = virt; p < virt + pages * PAGE_SIZE; p += 64)
            asm volatile("clflush (%0)" ::"r"(p) : "memory");
        asm volatile("mfence" ::: "memory");
    }
    return ok;
}

u32 kernel_cache_type(uptr virt) {
    const u64* table = mm::phys_to_virt<u64>(g_kernel_space.root());
    for (int level = 4; level >= 1; level--) {
        u64 e = table[table_index(virt, level)];
        if (!(e & PTE_PRESENT))
            return ~0u;
        if (level == 1 || (e & PTE_HUGE)) {
            bool pat = level == 1 ? (e & PTE_PAT_4K) : (e & PTE_PAT_LARGE);
            unsigned index = (pat ? 4 : 0) | (e & PTE_PCD ? 2 : 0) | (e & PTE_PWT ? 1 : 0);
            static const u32 types[8] = {0, CACHE_WT, CACHE_UC, CACHE_UC, 0, CACHE_WC, CACHE_UC, CACHE_UC};
            return types[index];
        }
        table = mm::phys_to_virt<u64>(e & PTE_ADDR_MASK);
    }
    return ~0u;
}

bool set_direct_cache(u64 phys, usize pages, u32 cache) {
    return set_kernel_cache(mm::g_hhdm_offset + (phys & ~(PAGE_SIZE - 1)), pages, cache);
}

bool AddressSpace::is_writable(uptr virt) const {
    LockGuard guard(m_lock);
    const u64* table = mm::phys_to_virt<u64>(m_pml4);
    for (int level = 4; level >= 1; level--) {
        u64 e = table[table_index(virt, level)];
        if (!(e & PTE_PRESENT))
            return false;
        if (level == 1 || (e & PTE_HUGE))
            return e & PTE_WRITABLE;
        table = mm::phys_to_virt<u64>(e & PTE_ADDR_MASK);
    }
    return false;
}

bool AddressSpace::translate(uptr virt, u64* phys_out) const {
    LockGuard guard(m_lock);
    return translate_in(m_pml4, virt, phys_out);
}

void AddressSpace::activate() const { arch::write_cr3(m_pml4); }

namespace {

void map_kernel_section(const char* start, const char* end, u32 flags) {
    auto* ka = boot::kernel_address();
    uptr vstart = align_down(reinterpret_cast<uptr>(start), PAGE_SIZE);
    uptr vend = align_up(reinterpret_cast<uptr>(end), PAGE_SIZE);
    u64 phys = vstart - ka->virtual_base + ka->physical_base;

    // The kernel must be where the bootloader says it is; check against its page tables.
    u64 boot_phys;
    if (!translate_in(arch::read_cr3() & PTE_ADDR_MASK, vstart, &boot_phys) || boot_phys != phys)
        panic("vmm: kernel section at %p is not physically contiguous", reinterpret_cast<void*>(vstart));

    if (!g_kernel_space.map_range(vstart, phys, vend - vstart, flags | GLOBAL))
        panic("vmm: failed to map kernel section at %p", reinterpret_cast<void*>(vstart));
}

} // namespace

void init() {
    auto ext = arch::cpuid(0x80000001);
    g_nx = ext.edx & (1u << 20);
    g_1g_pages = ext.edx & (1u << 26);
    if (!boot::kernel_address())
        panic("vmm: bootloader did not report the kernel's address");

    AddressSpace& ks = g_kernel_space;
    ks.m_pml4 = alloc_table();
    KASSERT(ks.m_pml4 != 0);

    // Give the whole kernel half its PDPTs now. Every future address space
    // copies these 256 PML4 entries, so kernel mappings added later appear
    // everywhere automatically.
    u64* pml4 = mm::phys_to_virt<u64>(ks.m_pml4);
    for (int i = 256; i < 512; i++) {
        u64 pdpt = alloc_table();
        KASSERT(pdpt != 0);
        pml4[i] = pdpt | PTE_PRESENT | PTE_WRITABLE;
    }

    // Kernel image with real per-section permissions (W^X).
    map_kernel_section(__kernel_start, __text_start, WRITE);     // Limine requests
    map_kernel_section(__text_start, __text_end, EXEC);          // code
    map_kernel_section(__rodata_start, __rodata_end, 0);         // read-only data
    map_kernel_section(__data_start, __data_end, WRITE);         // data + bss

    // Direct map of physical memory, at the same offset Limine used so that
    // every pointer it gave us stays valid.
    auto* mmap = boot::memory_map();
    for (u64 i = 0; i < mmap->entry_count; i++) {
        auto* e = mmap->entries[i];
        u32 flags = WRITE | GLOBAL;
        switch (e->type) {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_NVS:
        case LIMINE_MEMMAP_RESERVED_MAPPED:
            break;
        case LIMINE_MEMMAP_FRAMEBUFFER:
            flags |= CACHE_WC;
            break;
        default:
            continue; // reserved / bad memory: never touched through the direct map
        }
        u64 start = align_down(e->base, PAGE_SIZE);
        u64 end = align_up(e->base + e->length, PAGE_SIZE);
        if (!ks.map_range(mm::g_hhdm_offset + start, start, end - start, flags))
            panic("vmm: failed to direct-map %#lx-%#lx", start, end);
    }

    // Program the PAT so the cache-type bits in our entries mean what we expect.
    asm volatile("wbinvd" ::: "memory");
    arch::wrmsr(MSR_PAT, PAT_VALUE);
    arch::write_cr4(arch::read_cr4() | (1 << 7)); // CR4.PGE: enable global pages

    ks.activate();
    interrupts::set_handler(interrupts::VEC_TLB_SHOOTDOWN, shootdown_handler);
}

namespace {

constexpr u32 MSR_EFER = 0xC0000080;
constexpr u64 EFER_NXE = 1 << 11;
constexpr u64 CR4_PGE = 1 << 7;

Spinlock g_shoot_lock;
uptr g_shoot_start = 0;
usize g_shoot_pages = 0;
u32 g_shoot_pending = 0;

void flush_local(uptr start, usize pages) {
    if (pages > 32) {
        // Cheaper to flush everything. Global pages survive a CR3 reload,
        // so toggle CR4.PGE instead.
        u64 cr4 = arch::read_cr4();
        arch::write_cr4(cr4 & ~CR4_PGE);
        arch::write_cr4(cr4);
    } else {
        for (usize i = 0; i < pages; i++)
            arch::invlpg(start + i * PAGE_SIZE);
    }
}

void shootdown_handler(interrupts::Frame*) {
    flush_local(__atomic_load_n(&g_shoot_start, __ATOMIC_ACQUIRE), g_shoot_pages);
    __atomic_sub_fetch(&g_shoot_pending, 1, __ATOMIC_RELEASE);
}

} // namespace

void init_ap() {
    if (g_nx)
        arch::wrmsr(MSR_EFER, arch::rdmsr(MSR_EFER) | EFER_NXE);
    asm volatile("wbinvd" ::: "memory");
    arch::wrmsr(MSR_PAT, PAT_VALUE);
    arch::write_cr4(arch::read_cr4() | CR4_PGE);
    g_kernel_space.activate();
}

void shootdown(uptr start, usize pages, const AddressSpace* user) {
    if (pages == 0 || percpu::online_count() <= 1)
        return;
    if (!arch::interrupts_enabled())
        panic("vmm::shootdown() with interrupts disabled would deadlock");

    // Our page-table changes must be visible before we look at what other
    // CPUs are running (they publish `current` before loading its CR3).
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    Cpu* me = this_cpu();
    auto wanted = [&](Cpu* c) {
        if (!c || c == me || !__atomic_load_n(&c->online, __ATOMIC_ACQUIRE))
            return false;
        if (!user)
            return true;
        sched::Thread* t = __atomic_load_n(&c->current, __ATOMIC_ACQUIRE);
        return t && t->aspace == user;
    };

    // Spinlock (not IrqSpinlock): while waiting for the lock we keep
    // servicing other CPUs' shootdown requests.
    g_shoot_lock.lock();
    u32 targets = 0;
    Cpu* chosen[256];
    for (u32 i = 0; i < percpu::count() && targets < 256; i++) {
        Cpu* c = percpu::get(i);
        if (wanted(c))
            chosen[targets++] = c;
    }
    if (targets) {
        g_shoot_pages = pages;
        __atomic_store_n(&g_shoot_start, start, __ATOMIC_RELEASE);
        __atomic_store_n(&g_shoot_pending, targets, __ATOMIC_RELEASE);
        for (u32 i = 0; i < targets; i++)
            apic::send_ipi(chosen[i]->lapic_id, interrupts::VEC_TLB_SHOOTDOWN);
        while (__atomic_load_n(&g_shoot_pending, __ATOMIC_ACQUIRE))
            arch::pause();
    }
    g_shoot_lock.unlock();
}

void release_user_page(u64 phys) { put_user_page(phys); }

bool nx_supported() { return g_nx; }
bool huge_pages_supported() { return g_1g_pages; }
u64 page_table_pages() { return __atomic_load_n(&g_table_pages, __ATOMIC_RELAXED); }

} // namespace vmm
