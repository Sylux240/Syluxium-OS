#include <arch/x86_64/protect.hpp>
#include <fs/inotify.hpp>
#include <fs/ramfs.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>

namespace ramfs {

using namespace vfs;

namespace {

void touch(Inode* n) { n->atime = n->mtime = n->ctime = vfs::now(); }

u8 dtype_of(u32 mode) {
    switch (mode & S_IFMT) {
    case S_IFDIR: return DT_DIR;
    case S_IFREG: return DT_REG;
    case S_IFLNK: return DT_LNK;
    case S_IFCHR: return DT_CHR;
    case S_IFBLK: return DT_BLK;
    case S_IFIFO: return DT_FIFO;
    case S_IFSOCK: return 12; // DT_SOCK
    default:      return DT_UNKNOWN;
    }
}

char* dup_name(const char* name, usize len) {
    auto* s = static_cast<char*>(kmalloc(len + 1));
    if (s) {
        memcpy(s, name, len);
        s[len] = 0;
    }
    return s;
}

} // namespace

// ---- Dir ----

Dir::Dir() {
    ino = next_ino();
    mode = S_IFDIR | 0755;
    nlink = 2;
    touch(this);
}

Dir::Child* Dir::find(const char* name, usize len) {
    for (Child* c = m_children; c; c = c->next) {
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0)
            return c;
    }
    return nullptr;
}

Inode* Dir::lookup(const char* name, usize len) {
    LockGuard guard(m_lock);
    Child* c = find(name, len);
    return c ? c->inode : nullptr;
}

long Dir::attach(const char* name, usize len, Inode* node) {
    LockGuard guard(m_lock);
    if (find(name, len))
        return -err::EXIST;
    auto* c = static_cast<Child*>(kmalloc(sizeof(Child)));
    char* copy = dup_name(name, len);
    if (!c || !copy) {
        kfree(c);
        kfree(copy);
        return -err::NOMEM;
    }
    *c = Child{copy, node, m_children};
    m_children = c;
    m_count++;
    if (is_dir(node->mode)) {
        node->parent = this;
        nlink++;
    }
    mtime = ctime = vfs::now();
    return 0;
}

vfs::Inode* Dir::replace(const char* name, usize len, Inode* node) {
    LockGuard guard(m_lock);
    Child* c = find(name, len);
    if (!c)
        return nullptr;
    Inode* old = c->inode;
    c->inode = node;
    if (is_dir(node->mode))
        node->parent = this;
    return old;
}

long Dir::create(const char* name, usize len, u32 new_mode, const char* target, Inode** out) {
    Inode* node;
    switch (new_mode & S_IFMT) {
    case S_IFDIR: node = new Dir; break;
    case S_IFLNK: node = new Symlink(target ? target : ""); break;
    case S_IFREG:
    case 0:       node = new RegularFile; break;
    case S_IFIFO: // named pipe: opening it connects to a pipe (pipe::fifo_for)
        node = new RegularFile;
        node->mode = S_IFIFO;
        break;
    default:      return -err::OPNOTSUPP; // device nodes are added by the kernel
    }
    node->mode = (node->mode & S_IFMT) | (new_mode & 07777);
    long r = attach(name, len, node);
    if (r < 0) {
        delete node;
        return r;
    }
    if (out)
        *out = node;
    return 0;
}

long Dir::unlink(const char* name, usize len, bool remove_dir) {
    LockGuard guard(m_lock);
    Child** link = &m_children;
    for (Child* c = m_children; c; link = &c->next, c = c->next) {
        if (strlen(c->name) != len || memcmp(c->name, name, len) != 0)
            continue;
        bool dir = is_dir(c->inode->mode);
        if (remove_dir && !dir)
            return -err::NOTDIR;
        if (!remove_dir && dir)
            return -err::ISDIR;
        if (dir && static_cast<Dir*>(c->inode)->m_count != 0)
            return -err::NOTEMPTY;
        *link = c->next;
        m_count--;
        if (dir)
            nlink--;
        // The inode lives on while open files or mappings use it; files
        // free themselves after the last of those goes (unlinked()).
        vfs::Inode* gone = c->inode;
        gone->nlink = gone->nlink ? gone->nlink - 1 : 0;
        kfree(c->name);
        kfree(c);
        if (gone->nlink == 0 && !dir)
            gone->unlinked();
        mtime = ctime = vfs::now();
        return 0;
    }
    return -err::NOENT;
}

long Dir::rename(const char* old_name, usize old_len, Inode* new_dir_inode, const char* new_name, usize new_len) {
    if (!new_dir_inode || !new_dir_inode->is_ramfs_dir())
        return -err::XDEV;
    auto* target_dir = static_cast<Dir*>(new_dir_inode);
    Inode* moving = lookup(old_name, old_len);
    if (!moving)
        return -err::NOENT;
    if (Inode* existing = target_dir->lookup(new_name, new_len)) {
        if (existing == moving)
            return 0;
        bool existing_dir = is_dir(existing->mode);
        if (existing_dir != is_dir(moving->mode))
            return existing_dir ? -err::ISDIR : -err::NOTDIR;
        if (long r = target_dir->unlink(new_name, new_len, existing_dir); r < 0)
            return r;
    }
    if (long r = target_dir->attach(new_name, new_len, moving); r < 0)
        return r;
    // Detach from here without dropping the link count (it moved, it wasn't deleted).
    LockGuard guard(m_lock);
    Child** link = &m_children;
    for (Child* c = m_children; c; link = &c->next, c = c->next) {
        if (c->inode == moving && strlen(c->name) == old_len && memcmp(c->name, old_name, old_len) == 0) {
            *link = c->next;
            m_count--;
            if (is_dir(moving->mode))
                nlink--;
            kfree(c->name);
            kfree(c);
            break;
        }
    }
    return 0;
}

long Dir::readdir(u64 index, DirEntry* out) {
    if (index == 0 || index == 1) {
        out->ino = index == 0 ? ino : (parent ? parent->ino : ino);
        out->type = DT_DIR;
        memcpy(out->name, index == 0 ? "." : "..", index == 0 ? 2 : 3);
        return 1;
    }
    LockGuard guard(m_lock);
    u64 i = 2;
    for (Child* c = m_children; c; c = c->next, i++) {
        if (i == index) {
            out->ino = c->inode->ino;
            out->type = dtype_of(c->inode->mode);
            usize n = min<usize>(strlen(c->name), sizeof(out->name) - 1);
            memcpy(out->name, c->name, n);
            out->name[n] = 0;
            return 1;
        }
    }
    return 0;
}

// ---- RegularFile ----

using mm::PAGE_SIZE;

RegularFile::RegularFile() {
    ino = next_ino();
    mode = S_IFREG | 0644;
    touch(this);
}

RegularFile::~RegularFile() {
    inotify::inode_gone(this); // watches on it get IN_IGNORED (pointer is about to be reused)
    free_pages_from(0);
    kfree(m_pages);
}

u64 RegularFile::page(u64 index, bool allocate) {
    // The slot table grows outside the lock: a large one is vmalloc memory,
    // and freeing that needs a TLB shootdown, which can't wait with
    // interrupts off (a big memfd faulting in: Android's ART heap).
    for (;;) {
        u64 slots;
        {
            LockGuard guard(m_pages_lock);
            if (index < m_slots) {
                if (!m_pages[index] && allocate)
                    m_pages[index] = pmm::alloc_page(true);
                return m_pages[index];
            }
            if (!allocate)
                return 0;
            slots = max<u64>(index + 1, max<u64>(m_slots * 2, 16));
        }
        auto* grown = static_cast<u64*>(kzalloc(slots * sizeof(u64)));
        if (!grown)
            return 0;
        u64* old = nullptr;
        {
            LockGuard guard(m_pages_lock);
            if (m_slots < slots) { // (nobody grew it meanwhile)
                if (m_pages)
                    memcpy(grown, m_pages, m_slots * sizeof(u64));
                old = m_pages;
                m_pages = grown;
                m_slots = slots;
                grown = nullptr;
            }
        }
        kfree(grown);
        kfree(old);
    }
}

void RegularFile::free_pages_from(u64 first) {
    LockGuard guard(m_pages_lock);
    for (u64 i = first; i < m_slots; i++) {
        if (m_pages[i]) {
            pmm::free_page(m_pages[i]);
            m_pages[i] = 0;
        }
    }
}

isize RegularFile::read(u64 offset, void* buf, usize count) {
    LockGuard guard(m_io);
    if (offset >= size)
        return 0;
    usize n = min<u64>(count, size - offset);
    auto* out = static_cast<u8*>(buf);
    protect::UserAccess window; // buf is often the reader's (user) buffer
    for (usize done = 0; done < n;) {
        u64 pos = offset + done;
        usize in_page = pos % PAGE_SIZE;
        usize chunk = min<usize>(n - done, PAGE_SIZE - in_page);
        u64 phys = page(pos / PAGE_SIZE, false);
        if (phys)
            memcpy(out + done, mm::phys_to_virt<u8>(phys) + in_page, chunk);
        else
            memset(out + done, 0, chunk);
        done += chunk;
    }
    return static_cast<isize>(n);
}

isize RegularFile::write(u64 offset, const void* buf, usize count) {
    LockGuard guard(m_io);
    if (m_seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))
        return -err::PERM;
    if ((m_seals & F_SEAL_GROW) && offset + count > size)
        return -err::PERM;
    auto* in = static_cast<const u8*>(buf);
    protect::UserAccess window; // buf is often the writer's (user) buffer
    for (usize done = 0; done < count;) {
        u64 pos = offset + done;
        usize in_page = pos % PAGE_SIZE;
        usize chunk = min<usize>(count - done, PAGE_SIZE - in_page);
        u64 phys = page(pos / PAGE_SIZE, true);
        if (!phys)
            return done ? static_cast<isize>(done) : -err::NOSPC;
        memcpy(mm::phys_to_virt<u8>(phys) + in_page, in + done, chunk);
        done += chunk;
    }
    size = max<u64>(size, offset + count);
    mtime = ctime = vfs::now();
    return static_cast<isize>(count);
}

long RegularFile::truncate(u64 new_size) {
    LockGuard guard(m_io);
    if ((m_seals & F_SEAL_SHRINK) && new_size < size)
        return -err::PERM;
    if ((m_seals & F_SEAL_GROW) && new_size > size)
        return -err::PERM;
    if (new_size < size) {
        // Zero the tail of the last kept page, so a later grow reads zeros.
        if (u64 in_page = new_size % PAGE_SIZE) {
            u64 phys = page(new_size / PAGE_SIZE, false);
            if (phys)
                memset(mm::phys_to_virt<u8>(phys) + in_page, 0, PAGE_SIZE - in_page);
        }
        // Pages still mapped somewhere stay until the file goes away.
        if (!__atomic_load_n(&m_maps, __ATOMIC_ACQUIRE))
            free_pages_from(align_up<u64>(new_size, PAGE_SIZE) / PAGE_SIZE);
    }
    size = new_size;
    mtime = ctime = vfs::now();
    return 0;
}

long RegularFile::mmap_phys(u64 offset, u64* phys) {
    if (offset >= align_up<u64>(size, PAGE_SIZE))
        return -err::FAULT; // beyond the end of the file (Linux sends SIGBUS)
    u64 p = page(offset / PAGE_SIZE, true);
    if (!p)
        return -err::NOMEM;
    *phys = p;
    return 0;
}

void RegularFile::mmap_ref(int delta) {
    if (__atomic_add_fetch(&m_maps, static_cast<u32>(delta), __ATOMIC_ACQ_REL) == 0)
        maybe_free();
}

void RegularFile::on_open(vfs::File*) { __atomic_add_fetch(&m_opens, 1, __ATOMIC_ACQ_REL); }

void RegularFile::on_release(vfs::File*) {
    if (__atomic_sub_fetch(&m_opens, 1, __ATOMIC_ACQ_REL) == 0)
        maybe_free();
}

long RegularFile::seals(bool add, u32 bits) {
    LockGuard guard(m_io);
    if (!add)
        return m_seals;
    if (m_seals & F_SEAL_SEAL)
        return -err::PERM;
    if ((bits & F_SEAL_WRITE) && __atomic_load_n(&m_maps, __ATOMIC_ACQUIRE))
        return -err::BUSY; // Linux: writable shared mappings exist (we can't tell them apart)
    m_seals |= bits & (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE);
    return 0;
}

void RegularFile::maybe_free() {
    if (nlink == 0 && __atomic_load_n(&m_opens, __ATOMIC_ACQUIRE) == 0 &&
        __atomic_load_n(&m_maps, __ATOMIC_ACQUIRE) == 0)
        delete this;
}

// ---- Symlink ----

Symlink::Symlink(const char* target) {
    ino = next_ino();
    mode = S_IFLNK | 0777;
    m_target = dup_name(target, strlen(target));
    size = strlen(target);
    touch(this);
}

Symlink::~Symlink() { kfree(m_target); }

isize Symlink::readlink(char* buf, usize bufsize) {
    usize n = min<usize>(size, bufsize);
    memcpy(buf, m_target, n);
    return static_cast<isize>(n);
}

} // namespace ramfs
