#include <core/log.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <proc/memory.hpp>
#include <time/clock.hpp>

namespace proc {

using mm::PAGE_SIZE;

u32 prot_to_map_flags(u32 prot) {
    if (prot == PROT_NONE)
        return 0; // kept mapped (data preserved) but supervisor-only: user access faults
    u32 f = vmm::USER;
    if (prot & PROT_WRITE) f |= vmm::WRITE;
    if (prot & PROT_EXEC) f |= vmm::EXEC;
    return f;
}

Memory::~Memory() { destroy(); }

void Memory::put() {
    if (__atomic_sub_fetch(&m_refs, 1, __ATOMIC_ACQ_REL) == 0)
        delete this;
}

namespace {
IrqSpinlock g_dev_lock;
IntrusiveList<Vma, &Vma::dev_node> g_dev_vmas; // every VMA_DEVICE mapping

void dev_track(Vma* v, Memory* owner) {
    // (device mappings, and mappings of RAM files for zap_file_page)
    if ((v->kind != VMA_DEVICE && !(v->file && v->file->shared_mappable())) || v->owner)
        return;
    v->owner = owner;
    LockGuard guard(g_dev_lock);
    g_dev_vmas.push_back(v);
}

void dev_untrack(Vma* v) {
    if (!v->owner)
        return;
    LockGuard guard(g_dev_lock);
    g_dev_vmas.remove(v);
    v->owner = nullptr;
}
} // namespace

Memory* Memory::fork() {
    auto* as = vmm::AddressSpace::create_user();
    if (!as)
        return nullptr;
    auto* child = new Memory(as);

    LockGuard guard(m_lock);
    child->m_brk_base = m_brk_base;
    child->m_brk = m_brk;
    child->m_compat = m_compat;
    for (Vma* v : m_vmas) {
        auto* c = new Vma;
        c->start = v->start;
        c->end = v->end;
        c->prot = v->prot;
        c->kind = v->kind;
        c->file = v->file;
        c->file_offset = v->file_offset;
        c->file_bytes = v->file_bytes;
        if (c->file) {
            c->file->mmap_ref(+1);
            if (c->kind == VMA_DEVICE)
                c->file->mmap_range_ref(c->file_offset, +1);
        }
        child->m_vmas.push_back(c); // same order as ours: already sorted
        dev_track(c, child);

        u32 flags = prot_to_map_flags(v->prot);
        if (v->kind == VMA_DEVICE) { // device memory stays shared with the child
            for (uptr page = v->start; page < v->end; page += PAGE_SIZE) {
                u64 phys;
                if (m_as->translate(page, &phys))
                    as->map(page, phys, flags | v->file->mmap_cache_flags(v->file_offset + (page - v->start)));
            }
            continue;
        }
        u32 shared_flags = flags & ~vmm::WRITE; // writes must fault so they can be copied
        for (uptr page = v->start; page < v->end; page += PAGE_SIZE) {
            u64 phys;
            if (!m_as->translate(page, &phys))
                continue;
            __atomic_add_fetch(&pmm::frame(phys)->refcount, 1, __ATOMIC_RELAXED);
            if (flags & vmm::WRITE)
                m_as->protect(page, shared_flags);
            as->map(page, phys, shared_flags);
            child->m_rss++;
        }
    }
    // Our other threads may still hold writable TLB entries for the pages we
    // just made copy-on-write: flush them everywhere this space is running.
    vmm::shootdown(0, ~usize(0) >> 1, m_as);
    child->arg_start = arg_start;
    child->start_stack = start_stack;
    child->arg_end = arg_end;
    child->env_start = env_start;
    child->env_end = env_end;
    return child;
}

bool Memory::copy_on_write(Vma* v, uptr page) {
    u64 old_phys;
    if (!m_as->translate(page, &old_phys))
        return false;
    pmm::PageFrame* f = pmm::frame(old_phys);
    u32 flags = prot_to_map_flags(v->prot);

    if (__atomic_load_n(&f->refcount, __ATOMIC_ACQUIRE) == 1) {
        // Last user of the page: just make it writable again (upgrading
        // permissions needs no shootdown: stale entries just fault again).
        return m_as->protect(page, flags);
    }
    u64 new_phys = pmm::alloc_page();
    if (!new_phys)
        return false;
    memcpy(mm::phys_to_virt(new_phys), mm::phys_to_virt(old_phys), PAGE_SIZE);
    // One PTE store: threads of this process reading the page on other CPUs
    // (the kernel's futex code among them, holding a spinlock) never find it
    // missing -- they see the old copy or the new one.
    bool ok = m_as->replace(page, new_phys, flags) != 0;
    vmm::shootdown(page, 1, m_as);
    vmm::release_user_page(ok ? old_phys : new_phys);
    return ok;
}

Vma* Memory::find(uptr addr) {
    for (Vma* v : m_vmas) {
        if (addr < v->start)
            return nullptr;
        if (addr < v->end)
            return v;
    }
    return nullptr;
}

Vma* Memory::find_heap() {
    for (Vma* v : m_vmas)
        if (v->kind == VMA_HEAP)
            return v;
    return nullptr;
}

bool Memory::overlaps(uptr start, uptr end) {
    for (Vma* v : m_vmas)
        if (v->start < end && start < v->end)
            return true;
    return false;
}


void zap_device_range(u64 offset, u64 len) {
    // Collect under the lock, unmap outside it (shootdowns wait for other CPUs).
    struct Hit { vmm::AddressSpace* as; uptr start, end; };
    constexpr usize MAX_HITS = 64;
    Hit hits[MAX_HITS];
    usize n = 0;
    {
        LockGuard guard(g_dev_lock);
        for (Vma* v = g_dev_vmas.front(); v && n < MAX_HITS; v = g_dev_vmas.next_of(v)) {
            if (v->kind != VMA_DEVICE)
                continue;  // (a RAM file's mapping)
            u64 vbeg = v->file_offset, vend = v->file_offset + (v->end - v->start);
            u64 beg = max(vbeg, offset), end = min(vend, offset + len);
            if (beg >= end || !v->owner->aspace())
                continue;
            hits[n++] = {v->owner->aspace(), v->start + (beg - vbeg), v->start + (end - vbeg)};
        }
    }
    for (usize i = 0; i < n; i++) {
        for (uptr page = hits[i].start; page < hits[i].end; page += PAGE_SIZE)
            hits[i].as->unmap(page);
        vmm::shootdown(hits[i].start, (hits[i].end - hits[i].start) / PAGE_SIZE, hits[i].as);
    }
}

void zap_file_page(vfs::Inode* file, u64 offset, u64 phys) {
    // Shared mappings of a RAM file are VMA_DEVICE (the file's pages, no
    // reference of their own); private ones map its pages read-only with a
    // reference until written to.
    // (in rounds of up to 64 mappings: every one must go, or it would keep
    // showing a page that is freed)
    struct Hit { vmm::AddressSpace* as; uptr va; bool counted; };
    constexpr usize MAX_HITS = 64;
    Hit hits[MAX_HITS];
    for (usize skip = 0;; skip += MAX_HITS) {
        usize n = 0, seen = 0;
        {
            LockGuard guard(g_dev_lock);
            for (Vma* v = g_dev_vmas.front(); v && n < MAX_HITS; v = g_dev_vmas.next_of(v)) {
                if (v->file != file || !v->owner->aspace())
                    continue;
                if (offset < v->file_offset || offset >= v->file_offset + (v->end - v->start))
                    continue;
                if (seen++ < skip)
                    continue;
                hits[n++] = {v->owner->aspace(), v->start + (offset - v->file_offset), v->kind != VMA_DEVICE};
            }
        }
        for (usize i = 0; i < n; i++) {
            u64 now;
            if (!hits[i].as->translate(hits[i].va, &now) || (now & ~(PAGE_SIZE - 1)) != phys)
                continue;  // not mapped, or a private copy
            if (hits[i].as->unmap(hits[i].va) == phys && hits[i].counted)
                vmm::release_user_page(phys);
            vmm::shootdown(hits[i].va, 1, hits[i].as);
        }
        if (n < MAX_HITS)
            break;
    }
}

void Memory::insert(Vma* v) {
    dev_track(v, this);
    Vma* pos = m_vmas.front();
    while (pos && pos->start < v->start)
        pos = m_vmas.next_of(pos);
    m_vmas.insert_before(v, pos);
}

// Highest free gap of `len` bytes below the mmap area's top.
uptr Memory::find_gap(usize len, uptr top, uptr bottom) {
    if (!top)
        top = mmap_top();
    for (Vma* v = m_vmas.back(); v; v = m_vmas.prev_of(v)) {
        if (v->start >= top)
            continue;
        if (v->end <= top && top - v->end >= len)
            return top - len;
        top = min(top, v->start);
    }
    return top >= bottom + len ? top - len : 0;
}

Vma* Memory::split(Vma* v, uptr at) {
    auto* n = new Vma;
    n->start = at;
    n->end = v->end;
    n->prot = v->prot;
    n->kind = v->kind;
    n->file = v->file;
    u64 delta = at - v->start;
    n->file_offset = v->file_offset + delta;
    if (n->file) {
        n->file->mmap_ref(+1);
        if (n->kind == VMA_DEVICE)
            n->file->mmap_range_ref(n->file_offset, +1);
    }
    n->file_bytes = v->file_bytes > delta ? v->file_bytes - delta : 0;
    v->file_bytes = min(v->file_bytes, delta);
    v->end = at;
    insert(n);
    return n;
}

void Memory::remove_range(uptr start, uptr end) {
    for (Vma* v = m_vmas.front(); v;) {
        Vma* next = m_vmas.next_of(v);
        if (v->end <= start || v->start >= end) {
            v = next;
            continue;
        }
        if (v->start < start) { // keep the part below
            v = split(v, start);
            next = m_vmas.next_of(v);
        }
        if (v->end > end) {     // keep the part above
            split(v, end);
            next = m_vmas.next_of(v);
        }
        unmap_pages(v->start, v->end, v->kind != VMA_DEVICE);
        m_vmas.remove(v);
        dev_untrack(v);
        if (v->file && v->kind == VMA_DEVICE)
            v->file->mmap_range_ref(v->file_offset, -1);
        if (v->file)
            v->file->mmap_ref(-1); // may free an unlinked file (its pages are already unmapped)
        delete v;
        v = next;
    }
}

long Memory::map(uptr addr, usize len, u32 prot, u32 flags, vfs::Inode* file, u64 offset, u64 file_bytes,
                 uptr* out) {
    if (len == 0)
        return -err::INVAL;
    len = align_up<usize>(len, PAGE_SIZE);
    if (offset & (PAGE_SIZE - 1))
        return -err::INVAL;

    LockGuard guard(m_lock);
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    if (fixed) {
        if ((addr & (PAGE_SIZE - 1)) || addr < USER_MIN || addr + len < addr)
            return -err::INVAL;
        if (addr + len > task_end())
            return m_compat ? -err::NOMEM : -err::INVAL; // Linux: ENOMEM beyond a 32-bit task's end
        if (flags & MAP_FIXED_NOREPLACE) {
            if (overlaps(addr, addr + len))
                return -err::EXIST;
        } else {
            remove_range(addr, addr + len);
        }
    } else {
        addr = align_down<uptr>(addr, PAGE_SIZE);
        if (!addr || addr < USER_MIN || addr + len > mmap_top() || overlaps(addr, addr + len)) {
            // MAP_32BIT (x86_64): in the second gigabyte, as Linux places it.
            // 64-bit ART keeps its Java heap there: object references are 32 bits.
            if ((flags & MAP_32BIT) && !m_compat)
                addr = find_gap(len, 0x80000000, 0x40000000);
            else
                addr = find_gap(len);
        }
        if (!addr)
            return -err::NOMEM;
    }

    auto* v = new Vma;
    v->start = addr;
    v->end = addr + len;
    v->prot = prot;
    v->file = (flags & MAP_ANONYMOUS) ? nullptr : file;
    v->file_offset = offset;
    v->file_bytes = v->file ? min<u64>(file_bytes, len) : 0;
    if (flags & MAP_GROWSDOWN)
        v->kind = VMA_STACK;
    // Shared pages: device memory (also Linux driver files that look regular,
    // like dma-bufs), or MAP_SHARED of a file whose pages can be shared
    // (ramfs/tmpfs, memfd, shm). Other MAP_SHARED files get a private copy.
    if (v->file && (!vfs::is_reg(v->file->mode) || v->file->linux_file(nullptr) ||
                    ((flags & MAP_SHARED) && v->file->shared_mappable())))
        v->kind = VMA_DEVICE;
    if (v->file) {
        v->file->mmap_ref(+1);
        if (v->kind == VMA_DEVICE)
            v->file->mmap_range_ref(v->file_offset, +1);
    }
    insert(v);

    if (flags & MAP_POPULATE)
        prepare_locked(addr, len, false);
    *out = addr;
    return 0;
}

bool Memory::shared_at(uptr addr) {
    LockGuard guard(m_lock);
    Vma* v = find(addr);
    return v && v->kind == VMA_DEVICE;
}

bool Memory::mapping_at(uptr addr, vfs::Inode** file, usize* len) {
    LockGuard guard(m_lock);
    for (Vma* v : m_vmas) {
        if (v->start == addr && v->file) {
            *file = v->file;
            // Adjacent pieces of the same mapping (split by mprotect) count too.
            uptr end = v->end;
            for (Vma* n = m_vmas.next_of(v); n && n->start == end && n->file == v->file; n = m_vmas.next_of(n))
                end = n->end;
            *len = end - addr;
            return true;
        }
    }
    return false;
}

long Memory::unmap(uptr addr, usize len) {
    if ((addr & (PAGE_SIZE - 1)) || len == 0)
        return -err::INVAL;
    len = align_up<usize>(len, PAGE_SIZE);
    LockGuard guard(m_lock);
    remove_range(addr, addr + len);
    return 0;
}

long Memory::remap(uptr old_addr, usize old_len, usize new_len, u32 flags, uptr new_addr, uptr* out) {
    constexpr u32 MREMAP_MAYMOVE = 1, MREMAP_FIXED = 2;
    if ((old_addr & (PAGE_SIZE - 1)) || (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED)) ||
        ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE)) || new_len == 0 || old_len == 0)
        return -err::INVAL;
    old_len = align_up<usize>(old_len, PAGE_SIZE);
    new_len = align_up<usize>(new_len, PAGE_SIZE);
    LockGuard guard(m_lock);
    Vma* v = find(old_addr);
    if (!v || old_addr + old_len > v->end)
        return -err::FAULT; // the old range must lie in one mapping
    bool fixed = flags & MREMAP_FIXED;
    if (!fixed) {
        if (new_len <= old_len) { // shrink
            remove_range(old_addr + new_len, old_addr + old_len);
            *out = old_addr;
            return 0;
        }
        // Grow in place when the space after the mapping is free.
        uptr new_end = old_addr + new_len;
        if (old_addr + old_len == v->end && new_end > v->end && new_end <= task_end() &&
            !overlaps(v->end, new_end)) {
            if (v->file && v->file_bytes >= v->end - v->start)
                v->file_bytes = new_end - v->start; // a whole-file mapping keeps showing the file
            v->end = new_end;
            *out = old_addr;
            return 0;
        }
        if (!(flags & MREMAP_MAYMOVE))
            return -err::NOMEM;
    }
    // Move. The destination first.
    uptr dest;
    if (fixed) {
        if ((new_addr & (PAGE_SIZE - 1)) || new_addr < USER_MIN || new_addr + new_len > task_end() ||
            (new_addr < old_addr + old_len && old_addr < new_addr + new_len))
            return -err::INVAL;
        remove_range(new_addr, new_addr + new_len);
        dest = new_addr;
        v = find(old_addr); // remove_range may have split neighbours
    } else {
        dest = find_gap(new_len);
        if (!dest)
            return -err::NOMEM;
    }
    if (new_len < old_len) {
        remove_range(old_addr + new_len, old_addr + old_len);
        old_len = new_len;
        v = find(old_addr);
    }
    // Exactly [old_addr, old_addr + old_len) as one mapping.
    if (v->start < old_addr)
        v = split(v, old_addr);
    if (v->end > old_addr + old_len)
        split(v, old_addr + old_len);
    // Move the present pages' entries; device pages fault in again from the driver.
    bool device = v->kind == VMA_DEVICE;
    u32 base_flags = prot_to_map_flags(v->prot);
    for (usize off = 0; off < old_len; off += PAGE_SIZE) {
        u64 phys;
        if (!m_as->translate(old_addr + off, &phys))
            continue;
        m_as->unmap(old_addr + off);
        if (device)
            continue;
        u32 f = base_flags;
        if ((f & vmm::WRITE) && pmm::frame(phys)->refcount > 1)
            f &= ~vmm::WRITE; // still shared copy-on-write
        m_as->map(dest + off, phys, f);
    }
    vmm::shootdown(old_addr, old_len / PAGE_SIZE, m_as);
    m_vmas.remove(v);
    dev_untrack(v);
    if (v->file && v->file_bytes >= old_len)
        v->file_bytes = new_len;
    v->start = dest;
    v->end = dest + new_len;
    insert(v);
    *out = dest;
    return 0;
}

long Memory::protect(uptr addr, usize len, u32 prot) {
    if (addr & (PAGE_SIZE - 1))
        return -err::INVAL;
    len = align_up<usize>(len, PAGE_SIZE);
    uptr end = addr + len;

    LockGuard guard(m_lock);
    // Linux fails with ENOMEM if part of the range is unmapped.
    for (uptr a = addr; a < end;) {
        Vma* v = find(a);
        if (!v)
            return -err::NOMEM;
        a = v->end;
    }
    for (Vma* v = m_vmas.front(); v; v = m_vmas.next_of(v)) {
        if (v->end <= addr || v->start >= end)
            continue;
        if (v->start < addr)
            v = split(v, addr);
        if (v->end > end)
            split(v, end);
        v->prot = prot;
        u32 flags = prot_to_map_flags(prot);
        for (uptr page = v->start; page < v->end; page += PAGE_SIZE) {
            u64 phys;
            if (!m_as->translate(page, &phys))
                continue; // not present: populated with the new protection later
            u32 page_flags = flags;
            // A page still shared copy-on-write (after fork) stays read-only:
            // the first write must copy it, not change the other process's.
            if ((flags & vmm::WRITE) && v->kind != VMA_DEVICE && pmm::frame(phys)->refcount > 1)
                page_flags &= ~vmm::WRITE;
            m_as->protect(page, page_flags);
        }
        vmm::shootdown(v->start, (v->end - v->start) / PAGE_SIZE, m_as);
    }
    merge_adjacent(addr, end);
    return 0;
}

long Memory::discard(uptr addr, usize len) {
    if (addr & (PAGE_SIZE - 1))
        return -err::INVAL;
    len = align_up<usize>(len, PAGE_SIZE);
    uptr end = addr + len;
    if (end < addr)
        return -err::INVAL;
    LockGuard guard(m_lock);
    bool gap = false;
    uptr covered = addr;
    for (Vma* v = m_vmas.front(); v; v = m_vmas.next_of(v)) {
        if (v->end <= addr || v->start >= end)
            continue;
        uptr s = max(v->start, addr), e = min(v->end, end);
        if (s > covered)
            gap = true;
        covered = e;
        if (v->kind == VMA_DEVICE)
            continue; // shared memory keeps its contents
        // A private anonymous page only this mapping uses is zeroed where it
        // is: what the program sees is the same as a fresh zero page, without
        // freeing pages or TLB shootdowns (glibc does this for every cached
        // thread stack; unmapping each time stressed thread exit on a busy
        // machine). Pages shared copy-on-write and private file pages (which
        // must read back as the file's data) are dropped as Linux drops them.
        for (uptr page = s; page < e; page += PAGE_SIZE) {
            u64 phys;
            if (!m_as->translate(page, &phys))
                continue;
            pmm::PageFrame* fr = pmm::frame(phys);
            // Never touch kernel memory a driver mapped here (heap, page
            // tables, firmware areas): only ordinary pages are zeroed or freed.
            if (fr->flags & (pmm::PF_RESERVED | pmm::PF_SLAB | pmm::PF_HEAP_LARGE | pmm::PF_PAGE_TABLE))
                continue;
            if (!v->file && __atomic_load_n(&fr->refcount, __ATOMIC_ACQUIRE) == 1)
                memset(mm::phys_to_virt(phys), 0, PAGE_SIZE);
            else
                unmap_pages(page, page + PAGE_SIZE, true);
        }
    }
    return gap || covered < end ? -err::NOMEM : 0;
}

// Linux merges neighbouring mappings that became alike again (protection put
// back). Programs rely on it: bionic's atexit table toggles single pages
// read-only, then mremap()s the whole table, which must be one mapping.
// Only private anonymous memory is merged (no file offsets to line up).
void Memory::merge_adjacent(uptr start, uptr end) {
    auto mergeable = [](const Vma* a, const Vma* b) {
        return a->end == b->start && a->prot == b->prot && a->kind == VMA_NORMAL && b->kind == VMA_NORMAL &&
               !a->file && !b->file;
    };
    for (Vma* v = m_vmas.front(); v;) {
        Vma* n = m_vmas.next_of(v);
        if (!n || v->start > end)
            break;
        if (n->end >= start && mergeable(v, n)) {
            v->end = n->end;
            m_vmas.remove(n);
            dev_untrack(n);
            delete n;
            continue; // v may take its next neighbour too
        }
        v = n;
    }
}

long Memory::residency(uptr addr, usize len, u8* out) {
    if (addr & (PAGE_SIZE - 1))
        return -err::INVAL;
    uptr end = addr + align_up<usize>(len, PAGE_SIZE);
    if (end < addr || end > task_end())
        return -err::NOMEM;
    LockGuard guard(m_lock);
    for (uptr a = addr; a < end;) {
        Vma* v = find(a);
        if (!v)
            return -err::NOMEM;
        a = v->end;
    }
    for (uptr page = addr; page < end; page += PAGE_SIZE) {
        u64 phys;
        out[(page - addr) / PAGE_SIZE] = m_as->translate(page, &phys) ? 1 : 0;
    }
    return 0;
}

long Memory::brk(uptr new_end) {
    LockGuard guard(m_lock);
    if (new_end < m_brk_base)
        return static_cast<long>(m_brk);

    uptr old_top = align_up<uptr>(m_brk, PAGE_SIZE);
    uptr new_top = align_up<uptr>(new_end, PAGE_SIZE);
    Vma* heap = find_heap();

    if (new_top > old_top) {
        if (overlaps(old_top, new_top))
            return static_cast<long>(m_brk); // would collide with another mapping
        if (heap) {
            heap->end = new_top;
        } else {
            heap = new Vma;
            heap->start = align_up<uptr>(m_brk_base, PAGE_SIZE);
            heap->end = new_top;
            heap->prot = PROT_READ | PROT_WRITE;
            heap->kind = VMA_HEAP;
            if (heap->start < heap->end)
                insert(heap);
            else
                delete heap;
        }
    } else if (new_top < old_top && heap) {
        unmap_pages(new_top, old_top);
        heap->end = new_top;
        if (heap->end <= heap->start) {
            m_vmas.remove(heap);
            delete heap;
        }
    }
    m_brk = new_end;
    return static_cast<long>(m_brk);
}

bool Memory::populate(Vma* v, uptr page) {
    if (v->kind == VMA_DEVICE) {
        u64 dev_phys;
        if (v->file->mmap_phys(v->file_offset + (page - v->start), &dev_phys) < 0)
            return false;
        return m_as->map(page, dev_phys,
                         prot_to_map_flags(v->prot) | v->file->mmap_cache_flags(v->file_offset + (page - v->start)));
    }
    // A whole page of a RAM-backed file (the live system's programs and
    // libraries): map the file's own page read-only and share it between
    // every process that maps it; the first write copies it (copy-on-write).
    // Copying it per process would cost each Firefox process 156 MiB of libxul.
    if (v->file) {
        u64 delta = page - v->start;
        u64 file_phys;
        bool whole = delta + PAGE_SIZE <= v->file_bytes && ((v->file_offset + delta) & (PAGE_SIZE - 1)) == 0;
        if (whole && (v->file->shared_mappable() ? v->file->mmap_phys(v->file_offset + delta, &file_phys)
                                                 : v->file->cache_page(v->file_offset + delta, &file_phys)) == 0) {
            __atomic_add_fetch(&pmm::frame(file_phys)->refcount, 1, __ATOMIC_ACQ_REL); // the mapping's reference
            if (!m_as->map(page, file_phys, prot_to_map_flags(v->prot) & ~vmm::WRITE)) {
                vmm::release_user_page(file_phys);
                return false;
            }
            __atomic_add_fetch(&m_rss, 1, __ATOMIC_RELAXED);
            return true;
        }
    }
    u64 phys = pmm::alloc_page(true);
    if (!phys) {
        // Out of memory: the program gets SIGSEGV. Say why (once a second at most).
        static u64 last_ns = 0;
        u64 now = clock::now_ns();
        if (now - last_ns > 1'000'000'000ull) {
            last_ns = now;
            klog::warn("out of memory: a page fault at %#lx could not be served (%lu pages free)", page,
                       pmm::stats().free_pages);
        }
        return false;
    }
    if (v->file) {
        u64 delta = page - v->start;
        if (delta < v->file_bytes) {
            usize n = min<u64>(PAGE_SIZE, v->file_bytes - delta);
            isize r = v->file->read(v->file_offset + delta, mm::phys_to_virt(phys), n);
            if (r < 0) {
                pmm::free_page(phys);
                return false;
            }
        }
    }
    if (!m_as->map(page, phys, prot_to_map_flags(v->prot))) {
        pmm::free_page(phys);
        return false;
    }
    __atomic_add_fetch(&m_rss, 1, __ATOMIC_RELAXED);
    return true;
}

void Memory::unmap_pages(uptr start, uptr end, bool release) {
    constexpr usize BATCH = 64;
    u64 phys[BATCH];
    for (uptr chunk = start; chunk < end;) {
        usize n = 0;
        uptr chunk_start = chunk;
        for (; n < BATCH && chunk < end; chunk += PAGE_SIZE)
            phys[n++] = m_as->unmap(chunk);
        // No CPU may still reach these pages through its TLB once we free them.
        vmm::shootdown(chunk_start, n, m_as);
        for (usize i = 0; i < n && release; i++) {
            if (phys[i]) {
                vmm::release_user_page(phys[i]);
                __atomic_sub_fetch(&m_rss, 1, __ATOMIC_RELAXED);
            }
        }
    }
}

usize Memory::read_remote(uptr addr, void* out, usize len) {
    LockGuard guard(m_lock);
    auto* dst = static_cast<u8*>(out);
    usize done = 0;
    while (done < len) {
        u64 phys;
        if (!m_as || !m_as->translate(addr + done, &phys))
            break;
        usize chunk = min<usize>(len - done, PAGE_SIZE - ((addr + done) & (PAGE_SIZE - 1)));
        memcpy(dst + done, mm::phys_to_virt(phys), chunk);
        done += chunk;
    }
    return done;
}

u64 Memory::virtual_size() {
    LockGuard guard(m_lock);
    u64 total = 0;
    for (Vma* v : m_vmas)
        total += v->end - v->start;
    return total;
}

void Memory::for_each_vma(void (*fn)(const Vma&, void*), void* ctx) {
    LockGuard guard(m_lock);
    for (Vma* v : m_vmas)
        fn(*v, ctx);
}

bool Memory::fault(uptr addr, bool write, bool was_present) {
    LockGuard guard(m_lock);
    Vma* v = find(addr);
    if (!v || v->prot == PROT_NONE)
        return false;
    if (write && !(v->prot & PROT_WRITE))
        return false;
    uptr page = align_down<uptr>(addr, PAGE_SIZE);
    u64 phys;
    if (m_as->translate(page, &phys)) {
        // Present: a write to a page shared copy-on-write after fork().
        if (write && (v->prot & PROT_WRITE) && v->kind != VMA_DEVICE)
            return copy_on_write(v, page);
        // Not present when the CPU looked, present now: another thread of
        // this process faulted the same page in meanwhile. Just retry.
        if (!was_present)
            return true;
        return false; // a real protection violation
    }
    return populate(v, page);
}

long Memory::prepare_locked(uptr addr, usize len, bool write) {
    if (len == 0)
        return 0;
    if (addr + len < addr || addr + len > mm::USER_END)
        return -err::FAULT;
    for (uptr page = align_down<uptr>(addr, PAGE_SIZE); page < addr + len; page += PAGE_SIZE) {
        Vma* v = find(page);
        if (!v || v->prot == PROT_NONE || (write && !(v->prot & PROT_WRITE)))
            return -err::FAULT;
        u64 phys;
        if (!m_as->translate(page, &phys)) {
            if (!populate(v, page))
                return -err::FAULT;
        } else if (v->kind == VMA_DEVICE) {
            continue; // shared device memory: never copied
        } else if (write && pmm::frame(phys)->refcount > 1) {
            // Kernel is about to write here on the process's behalf: break the sharing first.
            if (!copy_on_write(v, page))
                return -err::FAULT;
        } else if (write && !m_as->is_writable(page)) {
            if (!copy_on_write(v, page)) // refcount 1 but still read-only after a fork
                return -err::FAULT;
        }
    }
    return 0;
}

long Memory::prepare(uptr addr, usize len, bool write) {
    LockGuard guard(m_lock);
    return prepare_locked(addr, len, write);
}

long Memory::write_remote(uptr addr, const void* data, usize len) {
    LockGuard guard(m_lock);
    if (long r = prepare_locked(addr, len, true); r < 0)
        return r;
    auto* src = static_cast<const u8*>(data);
    while (len) {
        u64 phys;
        m_as->translate(addr, &phys);
        usize chunk = min<usize>(len, PAGE_SIZE - (addr & (PAGE_SIZE - 1)));
        memcpy(mm::phys_to_virt(phys), src, chunk);
        addr += chunk;
        src += chunk;
        len -= chunk;
    }
    return 0;
}

void Memory::destroy() {
    LockGuard guard(m_lock);
    while (Vma* v = m_vmas.pop_front()) {
        dev_untrack(v);
        if (v->kind == VMA_DEVICE && m_as)
            unmap_pages(v->start, v->end, false); // destroy_user() must not free shared/driver pages
        if (v->file && v->kind == VMA_DEVICE)
            v->file->mmap_range_ref(v->file_offset, -1);
        if (v->file)
            v->file->mmap_ref(-1);
        delete v;
    }
    if (m_as) {
        m_as->destroy_user(); // frees every user page and page table
        delete m_as;
        m_as = nullptr;
    }
}

} // namespace proc
