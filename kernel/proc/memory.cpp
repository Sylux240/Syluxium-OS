#include <core/log.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <proc/memory.hpp>

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
        child->m_vmas.push_back(c); // same order as ours: already sorted

        u32 flags = prot_to_map_flags(v->prot);
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
    m_as->unmap(page);
    bool ok = m_as->map(page, new_phys, flags);
    vmm::shootdown(page, 1, m_as);
    vmm::release_user_page(old_phys);
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

void Memory::insert(Vma* v) {
    Vma* pos = m_vmas.front();
    while (pos && pos->start < v->start)
        pos = m_vmas.next_of(pos);
    m_vmas.insert_before(v, pos);
}

// Highest free gap of `len` bytes below the mmap area's top.
uptr Memory::find_gap(usize len) {
    uptr top = mmap_top();
    for (Vma* v = m_vmas.back(); v; v = m_vmas.prev_of(v)) {
        if (v->start >= top)
            continue;
        if (v->end <= top && top - v->end >= len)
            return top - len;
        top = min(top, v->start);
    }
    return top >= USER_MIN + len ? top - len : 0;
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
        unmap_pages(v->start, v->end);
        m_vmas.remove(v);
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
        if (!addr || addr < USER_MIN || addr + len > mmap_top() || overlaps(addr, addr + len))
            addr = find_gap(len);
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
    insert(v);

    if (flags & MAP_POPULATE)
        prepare_locked(addr, len, false);
    *out = addr;
    return 0;
}

long Memory::unmap(uptr addr, usize len) {
    if ((addr & (PAGE_SIZE - 1)) || len == 0)
        return -err::INVAL;
    len = align_up<usize>(len, PAGE_SIZE);
    LockGuard guard(m_lock);
    remove_range(addr, addr + len);
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
        for (uptr page = v->start; page < v->end; page += PAGE_SIZE)
            m_as->protect(page, flags); // only affects pages already present
        vmm::shootdown(v->start, (v->end - v->start) / PAGE_SIZE, m_as);
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
    u64 phys = pmm::alloc_page(true);
    if (!phys)
        return false;
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

void Memory::unmap_pages(uptr start, uptr end) {
    constexpr usize BATCH = 64;
    u64 phys[BATCH];
    for (uptr chunk = start; chunk < end;) {
        usize n = 0;
        uptr chunk_start = chunk;
        for (; n < BATCH && chunk < end; chunk += PAGE_SIZE)
            phys[n++] = m_as->unmap(chunk);
        // No CPU may still reach these pages through its TLB once we free them.
        vmm::shootdown(chunk_start, n, m_as);
        for (usize i = 0; i < n; i++) {
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

bool Memory::fault(uptr addr, bool write) {
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
        if (write && (v->prot & PROT_WRITE))
            return copy_on_write(v, page);
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
    while (Vma* v = m_vmas.pop_front())
        delete v;
    if (m_as) {
        m_as->destroy_user(); // frees every user page and page table
        delete m_as;
        m_as = nullptr;
    }
}

} // namespace proc
