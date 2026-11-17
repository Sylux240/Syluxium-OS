#pragma once

#include <fs/vfs.hpp>
#include <lib/list.hpp>
#include <mm/layout.hpp>
#include <mm/vmm.hpp>
#include <sched/sync.hpp>

// A process's user address space: the list of mapped regions (VMAs) and the
// page tables behind them. Pages are allocated lazily, on first touch, by
// the page-fault handler (demand paging).
namespace proc {

// mmap() protection and flag bits (Linux values).
constexpr u32 PROT_NONE = 0, PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4;
constexpr u32 MAP_SHARED = 0x01, MAP_PRIVATE = 0x02, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20,
              MAP_GROWSDOWN = 0x100, MAP_DENYWRITE = 0x800, MAP_EXECUTABLE = 0x1000,
              MAP_NORESERVE = 0x4000, MAP_POPULATE = 0x8000, MAP_STACK = 0x20000,
              MAP_FIXED_NOREPLACE = 0x100000;

constexpr uptr USER_MIN      = 0x10000;            // keep NULL-ish addresses unmapped
constexpr uptr MMAP_TOP      = 0x7ff000000000;     // mmap() allocates downwards from here
constexpr uptr STACK_TOP     = 0x7ffffffff000;
constexpr usize STACK_SIZE   = 8 * MiB;

// 32-bit (ia32) programs can only address the low 4 GiB. Layout like Linux:
// a vsyscall page at the very top, the stack below it, mmap() below that.
constexpr uptr COMPAT_TASK_END   = 0x100000000;     // exclusive
constexpr uptr COMPAT_VSYSCALL   = 0xFFFFE000;      // int $0x80; ret + sigreturn trampolines
constexpr uptr COMPAT_STACK_TOP  = 0xFFFFD000;
constexpr uptr COMPAT_MMAP_TOP   = 0xF7000000;      // leaves 128+ MiB for the stack

// VMA_DEVICE: a MAP_SHARED mapping of device memory (Inode::mmap_phys). Its
// pages belong to the driver: never copied on write, never freed by us.
enum VmaKind : u8 { VMA_NORMAL, VMA_HEAP, VMA_STACK, VMA_DEVICE };

struct Vma {
    uptr start = 0, end = 0;      // [start, end), page aligned
    u32 prot = 0;
    VmaKind kind = VMA_NORMAL;
    vfs::Inode* file = nullptr;   // file-backed (private copy) or anonymous
    u64 file_offset = 0;          // file position of `start`
    u64 file_bytes = 0;           // bytes of file data from `start`; the rest is zero-filled
    ListNode node;
};

class Memory {
public:
    explicit Memory(vmm::AddressSpace* as) : m_as(as) {}
    ~Memory();

    // Shared by vfork()/CLONE_VM children; freed with the last reference.
    void get() { __atomic_add_fetch(&m_refs, 1, __ATOMIC_RELAXED); }
    void put(); // must not be the active address space when the last ref goes

    // Copy-on-write duplicate for fork(): both sides share every page
    // read-only; the first write to a page gives the writer its own copy.
    Memory* fork();

    vmm::AddressSpace* aspace() const { return m_as; }

    // mmap. For file mappings `file_bytes` limits how much file data is
    // shown (ELF segments); pass ~0 for "to the end of the mapping".
    long map(uptr addr, usize len, u32 prot, u32 flags, vfs::Inode* file, u64 offset, u64 file_bytes,
             uptr* out);
    long unmap(uptr addr, usize len);
    long protect(uptr addr, usize len, u32 prot);
    long brk(uptr new_end);  // returns the (possibly unchanged) break
    void set_brk_base(uptr base) { m_brk_base = m_brk = base; }

    // 32-bit address space: every mapping stays below 4 GiB.
    void set_compat() { m_compat = true; }
    bool compat() const { return m_compat; }
    uptr mmap_top() const { return m_compat ? COMPAT_MMAP_TOP : MMAP_TOP; }
    uptr task_end() const { return m_compat ? COMPAT_TASK_END : mm::USER_END; }

    // Resolve a page fault. False means the access is invalid (segfault).
    // was_present: the CPU saw a present page (a protection fault, error code P=1).
    bool fault(uptr addr, bool write, bool was_present = false);

    // Make sure [addr, addr+len) is mapped with the needed access, faulting
    // pages in as required. 0 or -EFAULT.
    long prepare(uptr addr, usize len, bool write);

    // Write into this address space even when it isn't the active one.
    long write_remote(uptr addr, const void* data, usize len);

    // Read from this (maybe inactive) address space; stops at unmapped pages.
    usize read_remote(uptr addr, void* out, usize len);

    // Unmap everything and free the page tables.
    void destroy();

    // Accounting / introspection (for /proc).
    u64 rss_pages() const { return __atomic_load_n(&m_rss, __ATOMIC_RELAXED); }
    u64 virtual_size();
    // Visit every mapping (with the lock held; fn must not sleep).
    void for_each_vma(void (*fn)(const Vma& v, void* ctx), void* ctx);

    // Where exec put the argument and environment strings (/proc/<pid>/cmdline, environ).
    uptr arg_start = 0, arg_end = 0, env_start = 0, env_end = 0;

private:
    Vma* find(uptr addr);
    Vma* find_heap();
    bool overlaps(uptr start, uptr end);
    uptr find_gap(usize len);
    Vma* split(Vma* v, uptr at);
    void remove_range(uptr start, uptr end);
    void insert(Vma* v);
    bool populate(Vma* v, uptr page);
    bool copy_on_write(Vma* v, uptr page);
    long prepare_locked(uptr addr, usize len, bool write);
    void unmap_pages(uptr start, uptr end, bool release = true); // unmap, shoot down, then free (if release)

    vmm::AddressSpace* m_as;
    IntrusiveList<Vma, &Vma::node> m_vmas; // sorted by address
    sched::Mutex m_lock;
    uptr m_brk_base = 0, m_brk = 0;
    u32 m_refs = 1;
    u64 m_rss = 0; // resident pages
    bool m_compat = false;
};

u32 prot_to_map_flags(u32 prot);

} // namespace proc
