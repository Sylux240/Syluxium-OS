#include <fs/ramfs.hpp>
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

long Dir::create(const char* name, usize len, u32 new_mode, const char* target, Inode** out) {
    Inode* node;
    switch (new_mode & S_IFMT) {
    case S_IFDIR: node = new Dir; break;
    case S_IFLNK: node = new Symlink(target ? target : ""); break;
    case S_IFREG:
    case 0:       node = new RegularFile; break;
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
        // The inode itself is kept: open files may still use it.
        // TODO: free it once both nlink and open references reach zero.
        c->inode->nlink = c->inode->nlink ? c->inode->nlink - 1 : 0;
        kfree(c->name);
        kfree(c);
        mtime = ctime = vfs::now();
        return 0;
    }
    return -err::NOENT;
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

RegularFile::RegularFile() {
    ino = next_ino();
    mode = S_IFREG | 0644;
    touch(this);
}

RegularFile::~RegularFile() { kfree(m_data); }

long RegularFile::reserve(u64 capacity) {
    if (capacity <= m_capacity)
        return 0;
    u64 new_cap = max<u64>(capacity, m_capacity * 2);
    new_cap = max<u64>(new_cap, 256);
    auto* p = static_cast<u8*>(krealloc(m_data, new_cap));
    if (!p)
        return -err::NOSPC;
    memset(p + m_capacity, 0, new_cap - m_capacity);
    m_data = p;
    m_capacity = new_cap;
    return 0;
}

isize RegularFile::read(u64 offset, void* buf, usize count) {
    LockGuard guard(m_lock);
    if (offset >= size)
        return 0;
    usize n = min<u64>(count, size - offset);
    memcpy(buf, m_data + offset, n);
    return static_cast<isize>(n);
}

isize RegularFile::write(u64 offset, const void* buf, usize count) {
    LockGuard guard(m_lock);
    if (long r = reserve(offset + count); r < 0)
        return r;
    memcpy(m_data + offset, buf, count);
    size = max<u64>(size, offset + count);
    mtime = ctime = vfs::now();
    return static_cast<isize>(count);
}

long RegularFile::truncate(u64 new_size) {
    LockGuard guard(m_lock);
    if (new_size > size) {
        if (long r = reserve(new_size); r < 0)
            return r;
        memset(m_data + size, 0, new_size - size);
    }
    size = new_size;
    mtime = ctime = vfs::now();
    return 0;
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
