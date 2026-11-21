#pragma once

#include <lib/spinlock.hpp>
#include <lib/types.hpp>

// Virtual memory manager: page tables and address spaces.
namespace vmm {

// Mapping flags. Pages are always readable; everything else is opt-in
// (in particular, memory is non-executable unless EXEC is given).
enum MapFlags : u32 {
    WRITE    = 1 << 0,
    EXEC     = 1 << 1,
    USER     = 1 << 2,
    GLOBAL   = 1 << 3,
    CACHE_WC = 1 << 4, // write-combining: framebuffers
    CACHE_UC = 1 << 5, // uncached: device registers (MMIO)
    CACHE_WT = 1 << 6, // write-through
};

enum class PageSize { Size4K, Size2M, Size1G };

class AddressSpace {
public:
    // The kernel's own address space (also the template for user ones).
    static AddressSpace& kernel();

    // New address space for a process: empty user half, shared kernel half.
    static AddressSpace* create_user();

    // Free every user-half page table and every page mapped there
    // (dropping one reference per mapping). Must not be the active one.
    void destroy_user();

    // Change the flags of an existing 4 KiB mapping. Returns false if unmapped.
    bool protect(uptr virt, u32 flags);

    // Map one page. Fails if something is already mapped there.
    bool map(uptr virt, u64 phys, u32 flags, PageSize size = PageSize::Size4K);

    // Map a page-aligned range, using 2 MiB / 1 GiB pages where alignment allows.
    bool map_range(uptr virt, u64 phys, usize length, u32 flags);

    // Unmap one 4 KiB page; returns the physical address it pointed to (0 if none).
    u64 unmap(uptr virt);
    // Point a present 4 KiB page at another frame in one store (never
    // not-present in between: copy-on-write while other threads read the
    // page). Returns the old frame, 0 if the page wasn't mapped. Local TLB
    // only; the caller shoots down other CPUs.
    u64 replace(uptr virt, u64 phys, u32 flags);

    // Whether an existing 4 KiB mapping is writable.
    bool is_writable(uptr virt) const;

    // Look up the physical address behind a virtual one.
    bool translate(uptr virt, u64* phys_out) const;

    void activate() const;
    u64 root() const { return m_pml4; }

private:
    friend void init();

    u64* walk(uptr virt, int target_level, bool create);

    u64 m_pml4 = 0; // physical address of the top-level table
    mutable IrqSpinlock m_lock;
};

// Build the kernel page tables and switch to them.
void init();

// Per-CPU setup on an application processor (PAT, global pages, NX, CR3).
void init_ap();

// Make other CPUs drop stale TLB entries after an unmap/protect. For kernel
// mappings (user == nullptr) every CPU is told; for a user address space
// only CPUs currently running it (others reload CR3, which flushes, before
// they use it again). Must be called with interrupts enabled.
void shootdown(uptr start, usize pages, const AddressSpace* user = nullptr);

// Drop one reference to a page that was mapped into user space; frees it
// with the last reference.
void release_user_page(u64 phys);

bool nx_supported();
bool huge_pages_supported(); // 1 GiB pages
u64 page_table_pages();      // pages currently used for page tables

} // namespace vmm
