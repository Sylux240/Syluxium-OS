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
    // Free every user page and page table. Releases that don't fit (pages
    // the kernel owns, or with no references left) are reported and not
    // freed; their user addresses go to `bad` (up to bad_max), the count back.
    usize destroy_user(uptr* bad = nullptr, usize bad_max = 0);

    // Change the flags of an existing 4 KiB mapping. Returns false if unmapped.
    bool protect(uptr virt, u32 flags);

    // Map one page. Fails if something is already mapped there.
    bool map(uptr virt, u64 phys, u32 flags, PageSize size = PageSize::Size4K);

    // Map a page-aligned range, using 2 MiB / 1 GiB pages where alignment allows.
    bool map_range(uptr virt, u64 phys, usize length, u32 flags);

    // Unmap one 4 KiB page; returns the physical address it pointed to (0 if none).
    u64 unmap(uptr virt);
    // Unmap only if `virt` still maps `phys` (atomically, under the table
    // lock): another process's page, which its owner may be replacing or
    // unmapping at the same time. True if this call removed it.
    bool unmap_if(uptr virt, u64 phys);
    // Point a present 4 KiB page at another frame in one store (never
    // not-present in between: copy-on-write while other threads read the
    // page). Returns the old frame, 0 if the page wasn't mapped. Local TLB
    // only; the caller shoots down other CPUs.
    u64 replace(uptr virt, u64 phys, u32 flags);

    // Change the cache type (0 = write-back, CACHE_WC, CACHE_UC) of present
    // mappings, splitting 1 GiB / 2 MiB pages that cover the range into
    // 4 KiB ones. Local TLB only: set_direct_cache() does the rest.
    // Returns false if part of the range isn't mapped.
    bool set_cache(uptr virt, usize pages, u32 cache, bool* split = nullptr);

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

// The cache type of kernel memory (Linux's set_memory_wc/uc/wb and
// set_pages_array_*): a GPU reads write-combined buffers without snooping the
// CPU caches, so the kernel's own mapping must not be write-back either (two
// mappings with different types are undefined, and stale cache lines would
// reach the GPU). Every CPU's TLB is flushed, and when leaving write-back the
// range's cache lines are written back. Interrupts must be enabled.
bool set_kernel_cache(uptr virt, usize pages, u32 cache);
bool set_direct_cache(u64 phys, usize pages, u32 cache); // the direct map of these frames
// The cache type of a kernel mapping: 0 (write-back), CACHE_WC, CACHE_UC,
// CACHE_WT, or ~0u if unmapped (self-tests).
u32 kernel_cache_type(uptr virt);

// Translate through the page tables this CPU uses right now (CR3), not the
// kernel's own: the kernel half of every address space must agree (checks).
bool translate_current(uptr virt, u64* phys_out);

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
