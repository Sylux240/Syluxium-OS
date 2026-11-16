// File and filesystem system calls.

#include <fs/pipe.hpp>
#include <fs/poll.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <time/clock.hpp>
#include <syscall/syscall.hpp>

namespace syscall {

namespace {

using namespace vfs;
using proc::Process;

constexpr int AT_FDCWD = -100;
constexpr u32 AT_SYMLINK_NOFOLLOW = 0x100, AT_REMOVEDIR = 0x200, AT_EMPTY_PATH = 0x1000;
constexpr int SEEK_SET = 0, SEEK_CUR = 1, SEEK_END = 2;
constexpr usize PATH_MAX = 4096;

// Linux struct stat (x86_64).
struct LinuxStat {
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
    u32 pad0;
    u64 st_rdev;
    i64 st_size;
    i64 st_blksize;
    i64 st_blocks;
    i64 st_atime_sec, st_atime_nsec;
    i64 st_mtime_sec, st_mtime_nsec;
    i64 st_ctime_sec, st_ctime_nsec;
    i64 unused[3];
};
static_assert(sizeof(LinuxStat) == 144);

struct IoVec {
    u64 base;
    u64 len;
};

// RAII holder for a file reference from the fd table.
class FileRef {
public:
    FileRef(Process* p, long fd) : m_file(fd >= 0 ? proc::fd_get(p, static_cast<int>(fd)) : nullptr) {}
    ~FileRef() {
        if (m_file)
            file_put(m_file);
    }
    File* operator->() const { return m_file; }
    File* get() const { return m_file; }
    explicit operator bool() const { return m_file != nullptr; }

private:
    File* m_file;
};

// Copy a user path into a kernel buffer (heap, PATH_MAX).
class UserPath {
public:
    explicit UserPath(u64 user_ptr) : m_buf(static_cast<char*>(kmalloc(PATH_MAX))) {
        m_error = m_buf ? uaccess::string_from(reinterpret_cast<const char*>(user_ptr), m_buf, PATH_MAX)
                        : -err::NOMEM;
        if (m_error > 0)
            m_error = 0;
    }
    ~UserPath() { kfree(m_buf); }
    long error() const { return m_error; }
    const char* c_str() const { return m_buf; }

private:
    char* m_buf;
    long m_error;
};

// Directory a *at() call is relative to.
long dir_for(Process* p, long dirfd, const char* path, Inode** out) {
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        *out = p->cwd;
        return 0;
    }
    FileRef f(p, dirfd);
    if (!f)
        return -err::BADF;
    if (!is_dir(f->inode->mode))
        return -err::NOTDIR;
    *out = f->inode;
    return 0;
}

void fill_stat(const Inode* n, LinuxStat* st) {
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = n->ino;
    st->st_nlink = n->nlink;
    st->st_mode = n->mode;
    st->st_uid = n->uid;
    st->st_gid = n->gid;
    st->st_rdev = n->rdev;
    st->st_size = static_cast<i64>(n->size);
    st->st_blksize = 4096;
    st->st_blocks = static_cast<i64>((n->size + 511) / 512);
    st->st_atime_sec = n->atime.sec; st->st_atime_nsec = n->atime.nsec;
    st->st_mtime_sec = n->mtime.sec; st->st_mtime_nsec = n->mtime.nsec;
    st->st_ctime_sec = n->ctime.sec; st->st_ctime_nsec = n->ctime.nsec;
}

bool can_read(const File* f) { return (f->flags & O_ACCMODE) != O_WRONLY; }
bool can_write(const File* f) { return (f->flags & O_ACCMODE) != O_RDONLY; }

// read/write at the file's offset (or a given one for pread/pwrite).
long do_read(File* f, u64 user_buf, u64 count, i64 offset = -1) {
    if (!can_read(f) || (f->flags & O_PATH))
        return -err::BADF;
    if (is_dir(f->inode->mode))
        return -err::ISDIR;
    if (long r = uaccess::check(reinterpret_cast<void*>(user_buf), count, true); r < 0)
        return r;
    if (offset >= 0)
        return f->inode->read_file(f, static_cast<u64>(offset), reinterpret_cast<void*>(user_buf), count);
    LockGuard guard(f->lock);
    isize n = f->inode->read_file(f, f->offset, reinterpret_cast<void*>(user_buf), count);
    if (n > 0 && f->inode->seekable())
        f->offset += static_cast<u64>(n);
    return n;
}

long do_write(File* f, u64 user_buf, u64 count, i64 offset = -1) {
    if (!can_write(f) || (f->flags & O_PATH))
        return -err::BADF;
    if (long r = uaccess::check(reinterpret_cast<void*>(user_buf), count, false); r < 0)
        return r;
    if (offset >= 0)
        return f->inode->write_file(f, static_cast<u64>(offset), reinterpret_cast<const void*>(user_buf), count);
    LockGuard guard(f->lock);
    if (f->flags & O_APPEND)
        f->offset = f->inode->size;
    isize n = f->inode->write_file(f, f->offset, reinterpret_cast<const void*>(user_buf), count);
    if (n > 0 && f->inode->seekable())
        f->offset += static_cast<u64>(n);
    return n;
}

long do_openat(long dirfd, u64 user_path, u32 flags, u32 mode) {
    Process* p = proc::current();
    UserPath path(user_path);
    if (path.error())
        return path.error();

    Inode* base;
    if (long r = dir_for(p, dirfd, path.c_str(), &base); r < 0)
        return r;

    Inode* inode = nullptr;
    long r = resolve(path.c_str(), base, !(flags & O_NOFOLLOW), &inode);
    if (r == -err::NOENT && (flags & O_CREAT)) {
        Inode* parent;
        const char* name;
        usize len;
        if ((r = resolve_parent(path.c_str(), base, &parent, &name, &len)) < 0)
            return r;
        if ((r = parent->create(name, len, S_IFREG | (mode & ~p->umask & 07777), nullptr, &inode)) < 0)
            return r;
    } else if (r < 0) {
        return r;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -err::EXIST;
    }

    if ((flags & O_DIRECTORY) && !is_dir(inode->mode))
        return -err::NOTDIR;
    if (is_dir(inode->mode) && (flags & O_ACCMODE) != O_RDONLY)
        return -err::ISDIR;
    if (is_lnk(inode->mode) && !(flags & O_PATH))
        return -err::LOOP; // O_NOFOLLOW on a symlink
    if ((flags & O_TRUNC) && is_reg(inode->mode) && (flags & O_ACCMODE) != O_RDONLY)
        inode->truncate(0);

    File* f = open_inode(inode, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_NOCTTY | O_CLOEXEC));
    long fd = proc::fd_install(p, f, 0, flags & O_CLOEXEC);
    if (fd < 0)
        file_put(f);
    return fd;
}

long do_fstatat(long dirfd, u64 user_path, u64 user_stat, u32 flags) {
    Process* p = proc::current();
    Inode* inode;
    UserPath path(user_path);
    if (path.error())
        return path.error();

    if (path.c_str()[0] == 0) {
        if (!(flags & AT_EMPTY_PATH))
            return -err::NOENT;
        if (dirfd == AT_FDCWD) {
            inode = p->cwd;
        } else {
            FileRef f(p, dirfd);
            if (!f)
                return -err::BADF;
            inode = f->inode;
        }
    } else {
        Inode* base;
        if (long r = dir_for(p, dirfd, path.c_str(), &base); r < 0)
            return r;
        if (long r = resolve(path.c_str(), base, !(flags & AT_SYMLINK_NOFOLLOW), &inode); r < 0)
            return r;
    }
    LinuxStat st;
    fill_stat(inode, &st);
    return uaccess::put(reinterpret_cast<LinuxStat*>(user_stat), st);
}

// ---- handlers ----

long sys_read(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    return f ? do_read(f.get(), a1, a2) : -err::BADF;
}

long sys_write(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    return f ? do_write(f.get(), a1, a2) : -err::BADF;
}

long sys_pread64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    if (!f)
        return -err::BADF;
    if (!f->inode->seekable())
        return -err::SPIPE;
    return do_read(f.get(), a1, a2, static_cast<i64>(a3));
}

long sys_pwrite64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    if (!f)
        return -err::BADF;
    if (!f->inode->seekable())
        return -err::SPIPE;
    return do_write(f.get(), a1, a2, static_cast<i64>(a3));
}

long vectored(u64 fd, u64 user_iov, u64 count, bool write) {
    FileRef f(proc::current(), static_cast<long>(fd));
    if (!f)
        return -err::BADF;
    if (count > 1024)
        return -err::INVAL;
    long total = 0;
    for (u64 i = 0; i < count; i++) {
        IoVec v;
        if (long r = uaccess::get(&v, reinterpret_cast<IoVec*>(user_iov) + i); r < 0)
            return total ? total : r;
        if (v.len == 0)
            continue;
        long n = write ? do_write(f.get(), v.base, v.len) : do_read(f.get(), v.base, v.len);
        if (n < 0)
            return total ? total : n;
        total += n;
        if (static_cast<u64>(n) < v.len)
            break; // short transfer
    }
    return total;
}

long sys_readv(SYSCALL_ARGS) { SYSCALL_UNUSED; return vectored(a0, a1, a2, false); }
long sys_writev(SYSCALL_ARGS) { SYSCALL_UNUSED; return vectored(a0, a1, a2, true); }

long sys_open(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_openat(AT_FDCWD, a0, static_cast<u32>(a1), static_cast<u32>(a2)); }
long sys_openat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return do_openat(static_cast<long>(static_cast<i32>(a0)), a1, static_cast<u32>(a2), static_cast<u32>(a3));
}
long sys_creat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return do_openat(AT_FDCWD, a0, O_CREAT | O_WRONLY | O_TRUNC, static_cast<u32>(a1));
}

long sys_close(SYSCALL_ARGS) { SYSCALL_UNUSED; return proc::fd_close(proc::current(), static_cast<int>(a0)); }

long sys_stat(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_fstatat(AT_FDCWD, a0, a1, 0); }
long sys_lstat(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_fstatat(AT_FDCWD, a0, a1, AT_SYMLINK_NOFOLLOW); }
long sys_newfstatat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return do_fstatat(static_cast<i32>(a0), a1, a2, static_cast<u32>(a3));
}

long sys_fstat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    if (!f)
        return -err::BADF;
    LinuxStat st;
    fill_stat(f->inode, &st);
    return uaccess::put(reinterpret_cast<LinuxStat*>(a1), st);
}

long sys_lseek(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    if (!f)
        return -err::BADF;
    if (!f->inode->seekable())
        return -err::SPIPE;
    LockGuard guard(f->lock);
    i64 base;
    switch (static_cast<int>(a2)) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<i64>(f->offset); break;
    case SEEK_END: base = static_cast<i64>(f->inode->size); break;
    default: return -err::INVAL;
    }
    i64 pos = base + static_cast<i64>(a1);
    if (pos < 0)
        return -err::INVAL;
    f->offset = static_cast<u64>(pos);
    return pos;
}

long sys_ioctl(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    if (!f)
        return -err::BADF;
    // Linux encodes the argument size in bits 16-29 of most ioctl numbers;
    // older terminal ioctls don't, and use at most a termios (36 bytes).
    // Make sure the buffer is mapped and writable before a driver touches it.
    usize arg_size = (a1 >> 16) & 0x3FFF;
    if (arg_size == 0)
        arg_size = 36;
    if (a2 && uaccess::check(reinterpret_cast<void*>(a2), arg_size, true) < 0)
        return -err::FAULT;
    return f->inode->ioctl(a1, a2);
}

long sys_access(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    UserPath path(a0);
    if (path.error())
        return path.error();
    Inode* n;
    return resolve(path.c_str(), proc::current()->cwd, true, &n); // root may do anything
}

long sys_faccessat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* p = proc::current();
    UserPath path(a1);
    if (path.error())
        return path.error();
    Inode* base;
    if (long r = dir_for(p, static_cast<i32>(a0), path.c_str(), &base); r < 0)
        return r;
    Inode* n;
    return resolve(path.c_str(), base, true, &n);
}

long sys_dup(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* p = proc::current();
    File* f = proc::fd_get(p, static_cast<int>(a0));
    if (!f)
        return -err::BADF;
    long fd = proc::fd_install(p, f);
    if (fd < 0)
        file_put(f);
    return fd;
}

long sys_dup2(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return proc::fd_dup2(proc::current(), static_cast<int>(a0), static_cast<int>(a1), false);
}

long sys_dup3(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a0 == a1)
        return -err::INVAL;
    return proc::fd_dup2(proc::current(), static_cast<int>(a0), static_cast<int>(a1), a2 & O_CLOEXEC);
}

long sys_fcntl(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr int F_DUPFD = 0, F_GETFD = 1, F_SETFD = 2, F_GETFL = 3, F_SETFL = 4, F_DUPFD_CLOEXEC = 1030;
    constexpr int FD_CLOEXEC = 1;
    Process* p = proc::current();
    int fd = static_cast<int>(a0);
    FileRef f(p, fd);
    if (!f)
        return -err::BADF;
    switch (static_cast<int>(a1)) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        File* nf = proc::fd_get(p, fd);
        long r = proc::fd_install(p, nf, static_cast<int>(a2), static_cast<int>(a1) == F_DUPFD_CLOEXEC);
        if (r < 0)
            file_put(nf);
        return r;
    }
    case F_GETFD:
        return p->cloexec[fd] ? FD_CLOEXEC : 0;
    case F_SETFD:
        p->cloexec[fd] = a2 & FD_CLOEXEC;
        return 0;
    case F_GETFL:
        return f->flags;
    case F_SETFL: {
        constexpr u32 changeable = O_APPEND | O_NONBLOCK;
        f->flags = (f->flags & ~changeable) | (static_cast<u32>(a2) & changeable);
        return 0;
    }
    default:
        return -err::INVAL;
    }
}

long sys_getcwd(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* p = proc::current();
    usize len = strlen(p->cwd_path) + 1;
    if (a1 < len)
        return -err::RANGE;
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(a0), p->cwd_path, len); r < 0)
        return r;
    return static_cast<long>(len);
}

// Normalize `path` (relative to `cwd`) into an absolute path without . / ..
void join_path(const char* cwd, const char* path, char* out, usize out_size) {
    char tmp[1024];
    if (path[0] == '/')
        snprintf(tmp, sizeof(tmp), "%s", path);
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);

    usize len = 0;
    out[0] = 0;
    const char* p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* s = p;
        while (*p && *p != '/') p++;
        usize n = p - s;
        if (n == 1 && s[0] == '.')
            continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--;
            out[len] = 0;
            continue;
        }
        if (len + n + 2 >= out_size)
            break;
        out[len++] = '/';
        memcpy(out + len, s, n);
        len += n;
        out[len] = 0;
    }
    if (len == 0) {
        out[0] = '/';
        out[1] = 0;
    }
}

long sys_chdir(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* p = proc::current();
    UserPath path(a0);
    if (path.error())
        return path.error();
    Inode* n;
    if (long r = resolve(path.c_str(), p->cwd, true, &n); r < 0)
        return r;
    if (!is_dir(n->mode))
        return -err::NOTDIR;
    char abs[512];
    join_path(p->cwd_path, path.c_str(), abs, sizeof(abs));
    p->cwd = n;
    snprintf(p->cwd_path, sizeof(p->cwd_path), "%s", abs);
    return 0;
}

long do_mkdirat(long dirfd, u64 user_path, u32 mode) {
    Process* p = proc::current();
    UserPath path(user_path);
    if (path.error())
        return path.error();
    Inode* base;
    if (long r = dir_for(p, dirfd, path.c_str(), &base); r < 0)
        return r;
    Inode* parent;
    const char* name;
    usize len;
    if (long r = resolve_parent(path.c_str(), base, &parent, &name, &len); r < 0)
        return r;
    return parent->create(name, len, S_IFDIR | (mode & ~p->umask & 07777), nullptr, nullptr);
}

long sys_mkdir(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_mkdirat(AT_FDCWD, a0, static_cast<u32>(a1)); }
long sys_mkdirat(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_mkdirat(static_cast<i32>(a0), a1, static_cast<u32>(a2)); }

long do_unlinkat(long dirfd, u64 user_path, bool dir) {
    Process* p = proc::current();
    UserPath path(user_path);
    if (path.error())
        return path.error();
    Inode* base;
    if (long r = dir_for(p, dirfd, path.c_str(), &base); r < 0)
        return r;
    Inode* parent;
    const char* name;
    usize len;
    if (long r = resolve_parent(path.c_str(), base, &parent, &name, &len); r < 0)
        return r;
    return parent->unlink(name, len, dir);
}

long sys_unlink(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_unlinkat(AT_FDCWD, a0, false); }
long sys_rmdir(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_unlinkat(AT_FDCWD, a0, true); }
long sys_unlinkat(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_unlinkat(static_cast<i32>(a0), a1, a2 & AT_REMOVEDIR); }

long do_symlinkat(u64 user_target, long dirfd, u64 user_path) {
    Process* p = proc::current();
    UserPath target(user_target);
    UserPath path(user_path);
    if (target.error()) return target.error();
    if (path.error()) return path.error();
    Inode* base;
    if (long r = dir_for(p, dirfd, path.c_str(), &base); r < 0)
        return r;
    Inode* parent;
    const char* name;
    usize len;
    if (long r = resolve_parent(path.c_str(), base, &parent, &name, &len); r < 0)
        return r;
    return parent->create(name, len, S_IFLNK | 0777, target.c_str(), nullptr);
}

long sys_symlink(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_symlinkat(a0, AT_FDCWD, a1); }
long sys_symlinkat(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_symlinkat(a0, static_cast<i32>(a1), a2); }

long do_readlinkat(long dirfd, u64 user_path, u64 user_buf, u64 size) {
    Process* p = proc::current();
    UserPath path(user_path);
    if (path.error())
        return path.error();
    if (strcmp(path.c_str(), "/proc/self/exe") == 0) {
        // Until procfs exists: the one /proc link nearly every runtime asks for.
        usize n = min<usize>(strlen(p->exe_path), size);
        if (long r = uaccess::copy_to(reinterpret_cast<void*>(user_buf), p->exe_path, n); r < 0)
            return r;
        return static_cast<long>(n);
    }
    Inode* base;
    if (long r = dir_for(p, dirfd, path.c_str(), &base); r < 0)
        return r;
    Inode* n;
    if (long r = resolve(path.c_str(), base, false, &n); r < 0)
        return r;
    if (!is_lnk(n->mode))
        return -err::INVAL;
    char buf[256];
    isize len = n->readlink(buf, min<u64>(size, sizeof(buf)));
    if (len < 0)
        return len;
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(user_buf), buf, static_cast<usize>(len)); r < 0)
        return r;
    return len;
}

long sys_readlink(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_readlinkat(AT_FDCWD, a0, a1, a2); }
long sys_readlinkat(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_readlinkat(static_cast<i32>(a0), a1, a2, a3); }

long sys_getdents64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    if (!f)
        return -err::BADF;
    if (!is_dir(f->inode->mode))
        return -err::NOTDIR;
    if (long r = uaccess::check(reinterpret_cast<void*>(a1), a2, true); r < 0)
        return r;

    auto* out = reinterpret_cast<u8*>(a1);
    usize used = 0;
    LockGuard guard(f->lock);
    DirEntry e;
    while (true) {
        long r = f->inode->readdir(f->offset, &e);
        if (r <= 0)
            break;
        usize name_len = strlen(e.name);
        usize reclen = align_up<usize>(19 + name_len + 1, 8); // ino(8) off(8) reclen(2) type(1) name
        if (used + reclen > a2) {
            if (used == 0)
                return -err::INVAL; // buffer too small for even one entry
            break;
        }
        u8* rec = out + used;
        u64 next_off = f->offset + 1;
        u16 rl = static_cast<u16>(reclen);
        memcpy(rec, &e.ino, 8);
        memcpy(rec + 8, &next_off, 8);
        memcpy(rec + 16, &rl, 2);
        rec[18] = e.type;
        memcpy(rec + 19, e.name, name_len + 1);
        memset(rec + 19 + name_len + 1, 0, reclen - (19 + name_len + 1));
        used += reclen;
        f->offset++;
    }
    return static_cast<long>(used);
}

long sys_ftruncate(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    if (!f)
        return -err::BADF;
    if (!can_write(f.get()))
        return -err::INVAL;
    return f->inode->truncate(a1);
}

long sys_truncate(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    UserPath path(a0);
    if (path.error())
        return path.error();
    Inode* n;
    if (long r = resolve(path.c_str(), proc::current()->cwd, true, &n); r < 0)
        return r;
    return n->truncate(a1);
}

long sys_umask(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* p = proc::current();
    u32 old = p->umask;
    p->umask = static_cast<u32>(a0) & 0777;
    return old;
}

long sys_pipe2(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a1 & ~(O_CLOEXEC | O_NONBLOCK))
        return -err::INVAL;
    Process* p = proc::current();
    if (long r = uaccess::check(reinterpret_cast<void*>(a0), 2 * sizeof(i32), true); r < 0)
        return r;
    File *rd, *wr;
    if (long r = pipe::create(static_cast<u32>(a1), &rd, &wr); r < 0)
        return r;
    bool cloexec = a1 & O_CLOEXEC;
    long fd0 = proc::fd_install(p, rd, 0, cloexec);
    if (fd0 < 0) {
        file_put(rd);
        file_put(wr);
        return fd0;
    }
    long fd1 = proc::fd_install(p, wr, 0, cloexec);
    if (fd1 < 0) {
        proc::fd_close(p, static_cast<int>(fd0));
        file_put(wr);
        return fd1;
    }
    i32 fds[2] = {static_cast<i32>(fd0), static_cast<i32>(fd1)};
    return uaccess::copy_to(reinterpret_cast<void*>(a0), fds, sizeof(fds));
}

long sys_pipe(SYSCALL_ARGS) { return sys_pipe2(a0, 0, a2, a3, a4, a5, frame); }

// ---- poll / select ----

struct PollFd {
    i32 fd;
    i16 events;
    i16 revents;
};

constexpr u64 WAIT_FOREVER = ~0ull;

// Core of poll/ppoll: fills revents, returns the number of ready entries.
long do_poll(PollFd* fds, u64 nfds, u64 timeout_ns) {
    Process* p = proc::current();
    u64 deadline = timeout_ns == WAIT_FOREVER ? WAIT_FOREVER : clock::now_ns() + timeout_ns;
    for (;;) {
        u64 seq = poll::sequence();
        long ready = 0;
        for (u64 i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            FileRef f(p, fds[i].fd);
            if (!f) {
                fds[i].revents = poll::POLLNVAL;
            } else {
                u16 mask = f->inode->poll(f.get());
                if (mask & poll::POLLIN) mask |= poll::POLLRDNORM;
                if (mask & poll::POLLOUT) mask |= poll::POLLWRNORM;
                fds[i].revents = static_cast<i16>(mask & (fds[i].events | poll::POLLERR | poll::POLLHUP));
            }
            if (fds[i].revents)
                ready++;
        }
        if (ready || timeout_ns == 0)
            return ready;
        if (signal::interrupted())
            return -err::INTR;
        u64 now = clock::now_ns();
        if (deadline != WAIT_FOREVER && now >= deadline)
            return 0;
        poll::wait_for_change(seq, deadline == WAIT_FOREVER ? WAIT_FOREVER : deadline - now);
    }
}

long poll_user(u64 user_fds, u64 nfds, u64 timeout_ns) {
    if (nfds > static_cast<u64>(proc::MAX_FDS))
        return -err::INVAL;
    auto* fds = static_cast<PollFd*>(kmalloc(max<u64>(nfds, 1) * sizeof(PollFd)));
    if (!fds)
        return -err::NOMEM;
    long r = uaccess::copy_from(fds, reinterpret_cast<void*>(user_fds), nfds * sizeof(PollFd));
    if (r == 0) {
        r = do_poll(fds, nfds, timeout_ns);
        if (r >= 0) {
            long e = uaccess::copy_to(reinterpret_cast<void*>(user_fds), fds, nfds * sizeof(PollFd));
            if (e < 0)
                r = e;
        }
    }
    kfree(fds);
    return r;
}

long sys_poll(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    i32 ms = static_cast<i32>(a2);
    return poll_user(a0, a1, ms < 0 ? WAIT_FOREVER : static_cast<u64>(ms) * 1'000'000);
}

struct KTimespec {
    i64 sec, nsec;
};

long sys_ppoll(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u64 timeout = WAIT_FOREVER;
    if (a2) {
        KTimespec ts;
        if (long r = uaccess::get(&ts, reinterpret_cast<const KTimespec*>(a2)); r < 0)
            return r;
        timeout = static_cast<u64>(ts.sec) * 1'000'000'000 + static_cast<u64>(ts.nsec);
    }
    // TODO: apply the temporary signal mask (a3) while waiting.
    return poll_user(a0, a1, timeout);
}

// select/pselect6 on top of do_poll.
long do_select(u64 nfds, u64 user_read, u64 user_write, u64 user_except, u64 timeout_ns) {
    if (nfds > static_cast<u64>(proc::MAX_FDS))
        return -err::INVAL;
    constexpr usize WORDS = proc::MAX_FDS / 64;
    u64 rd[WORDS] = {}, wr[WORDS] = {}, ex[WORDS] = {};
    usize bytes = (nfds + 7) / 8;
    if ((user_read && uaccess::copy_from(rd, reinterpret_cast<void*>(user_read), bytes) < 0) ||
        (user_write && uaccess::copy_from(wr, reinterpret_cast<void*>(user_write), bytes) < 0) ||
        (user_except && uaccess::copy_from(ex, reinterpret_cast<void*>(user_except), bytes) < 0))
        return -err::FAULT;

    auto* fds = static_cast<PollFd*>(kmalloc(max<u64>(nfds, 1) * sizeof(PollFd)));
    if (!fds)
        return -err::NOMEM;
    u64 n = 0;
    for (u64 fd = 0; fd < nfds; fd++) {
        u64 w = fd / 64, b = 1ull << (fd % 64);
        i16 events = 0;
        if (rd[w] & b) events |= poll::POLLIN;
        if (wr[w] & b) events |= poll::POLLOUT;
        if (ex[w] & b) events |= poll::POLLPRI;
        if (events)
            fds[n++] = PollFd{static_cast<i32>(fd), events, 0};
    }
    long r = do_poll(fds, n, timeout_ns);
    if (r >= 0) {
        memset(rd, 0, sizeof(rd));
        memset(wr, 0, sizeof(wr));
        memset(ex, 0, sizeof(ex));
        long count = 0;
        for (u64 i = 0; i < n; i++) {
            if (fds[i].revents & poll::POLLNVAL) {
                r = -err::BADF;
                break;
            }
            u64 w = fds[i].fd / 64, b = 1ull << (fds[i].fd % 64);
            if ((fds[i].events & poll::POLLIN) && (fds[i].revents & (poll::POLLIN | poll::POLLHUP | poll::POLLERR))) { rd[w] |= b; count++; }
            if ((fds[i].events & poll::POLLOUT) && (fds[i].revents & (poll::POLLOUT | poll::POLLERR))) { wr[w] |= b; count++; }
            if ((fds[i].events & poll::POLLPRI) && (fds[i].revents & poll::POLLPRI)) { ex[w] |= b; count++; }
        }
        if (r >= 0) {
            r = count;
            if ((user_read && uaccess::copy_to(reinterpret_cast<void*>(user_read), rd, bytes) < 0) ||
                (user_write && uaccess::copy_to(reinterpret_cast<void*>(user_write), wr, bytes) < 0) ||
                (user_except && uaccess::copy_to(reinterpret_cast<void*>(user_except), ex, bytes) < 0))
                r = -err::FAULT;
        }
    }
    kfree(fds);
    return r;
}

long sys_select(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u64 timeout = WAIT_FOREVER;
    if (a4) {
        struct { i64 sec, usec; } tv;
        if (long r = uaccess::copy_from(&tv, reinterpret_cast<void*>(a4), sizeof(tv)); r < 0)
            return r;
        timeout = static_cast<u64>(tv.sec) * 1'000'000'000 + static_cast<u64>(tv.usec) * 1000;
    }
    return do_select(a0, a1, a2, a3, timeout);
}

long sys_pselect6(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u64 timeout = WAIT_FOREVER;
    if (a4) {
        KTimespec ts;
        if (long r = uaccess::get(&ts, reinterpret_cast<const KTimespec*>(a4)); r < 0)
            return r;
        timeout = static_cast<u64>(ts.sec) * 1'000'000'000 + static_cast<u64>(ts.nsec);
    }
    // TODO: apply the temporary signal mask (a5) while waiting.
    return do_select(a0, a1, a2, a3, timeout);
}

// sendfile(out_fd, in_fd, offset*, count): copy between files in the kernel.
long sys_sendfile(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* p = proc::current();
    FileRef out(p, static_cast<long>(a0));
    FileRef in(p, static_cast<long>(a1));
    if (!out || !in)
        return -err::BADF;
    if (!can_read(in.get()) || !can_write(out.get()))
        return -err::BADF;

    i64 offset = -1;
    if (a2 && uaccess::get(&offset, reinterpret_cast<const i64*>(a2)) < 0)
        return -err::FAULT;

    constexpr usize CHUNK = 64 * KiB;
    auto* buf = static_cast<u8*>(kmalloc(CHUNK));
    if (!buf)
        return -err::NOMEM;
    u64 total = 0;
    long result = 0;
    while (total < a3) {
        usize want = min<u64>(CHUNK, a3 - total);
        isize n;
        if (offset >= 0) {
            n = in->inode->read_file(in.get(), static_cast<u64>(offset), buf, want);
        } else {
            LockGuard guard(in->lock);
            n = in->inode->read_file(in.get(), in->offset, buf, want);
            if (n > 0 && in->inode->seekable())
                in->offset += static_cast<u64>(n);
        }
        if (n <= 0) {
            result = n;
            break;
        }
        isize w;
        {
            LockGuard guard(out->lock);
            if (out->flags & O_APPEND)
                out->offset = out->inode->size;
            w = out->inode->write_file(out.get(), out->offset, buf, static_cast<usize>(n));
            if (w > 0 && out->inode->seekable())
                out->offset += static_cast<u64>(w);
        }
        if (w < 0) {
            result = w;
            break;
        }
        if (offset >= 0)
            offset += w;
        total += static_cast<u64>(w);
        if (w < n)
            break;
    }
    kfree(buf);
    if (a2 && offset >= 0)
        uaccess::put(reinterpret_cast<i64*>(a2), offset);
    return total ? static_cast<long>(total) : result;
}

// ---- permissions ----

long chmod_inode(Inode* n, u64 mode) {
    n->mode = (n->mode & S_IFMT) | (static_cast<u32>(mode) & 07777);
    n->ctime = vfs::now();
    return 0;
}

long sys_chmod(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    UserPath path(a0);
    if (path.error())
        return path.error();
    Inode* n;
    if (long r = resolve(path.c_str(), proc::current()->cwd, true, &n); r < 0)
        return r;
    return chmod_inode(n, a1);
}

long sys_fchmod(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    return f ? chmod_inode(f->inode, a1) : -err::BADF;
}

long sys_fchmodat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* p = proc::current();
    UserPath path(a1);
    if (path.error())
        return path.error();
    Inode* base;
    if (long r = dir_for(p, static_cast<i32>(a0), path.c_str(), &base); r < 0)
        return r;
    Inode* n;
    if (long r = resolve(path.c_str(), base, true, &n); r < 0)
        return r;
    return chmod_inode(n, a2);
}

// Networking arrives in phase 6; programs probing for sockets get clean errors.
long sys_socket(SYSCALL_ARGS) { SYSCALL_UNUSED; return -97; /* EAFNOSUPPORT */ }
long sys_not_socket(SYSCALL_ARGS) { SYSCALL_UNUSED; return -88; /* ENOTSOCK */ }

// Ownership: accepted (single root user for now).
long sys_ok(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; }

} // namespace

void register_file() {
    set(0, sys_read, "read");
    set(1, sys_write, "write");
    set(2, sys_open, "open");
    set(3, sys_close, "close");
    set(4, sys_stat, "stat");
    set(5, sys_fstat, "fstat");
    set(6, sys_lstat, "lstat");
    set(8, sys_lseek, "lseek");
    set(16, sys_ioctl, "ioctl");
    set(17, sys_pread64, "pread64");
    set(18, sys_pwrite64, "pwrite64");
    set(19, sys_readv, "readv");
    set(20, sys_writev, "writev");
    set(21, sys_access, "access");
    set(22, sys_pipe, "pipe");
    set(293, sys_pipe2, "pipe2");
    set(32, sys_dup, "dup");
    set(33, sys_dup2, "dup2");
    set(72, sys_fcntl, "fcntl");
    set(74, sys_ok, "fsync");
    set(75, sys_ok, "fdatasync");
    set(76, sys_truncate, "truncate");
    set(77, sys_ftruncate, "ftruncate");
    set(79, sys_getcwd, "getcwd");
    set(80, sys_chdir, "chdir");
    set(83, sys_mkdir, "mkdir");
    set(84, sys_rmdir, "rmdir");
    set(85, sys_creat, "creat");
    set(87, sys_unlink, "unlink");
    set(88, sys_symlink, "symlink");
    set(89, sys_readlink, "readlink");
    set(7, sys_poll, "poll");
    set(23, sys_select, "select");
    set(40, sys_sendfile, "sendfile");
    set(41, sys_socket, "socket");
    set(42, sys_not_socket, "connect");
    set(51, sys_not_socket, "getsockname");
    set(52, sys_not_socket, "getpeername");
    set(270, sys_pselect6, "pselect6");
    set(271, sys_ppoll, "ppoll");
    set(90, sys_chmod, "chmod");
    set(91, sys_fchmod, "fchmod");
    set(92, sys_ok, "chown");
    set(93, sys_ok, "fchown");
    set(94, sys_ok, "lchown");
    set(95, sys_umask, "umask");
    set(162, sys_ok, "sync");
    set(217, sys_getdents64, "getdents64");
    set(257, sys_openat, "openat");
    set(258, sys_mkdirat, "mkdirat");
    set(260, sys_ok, "fchownat");
    set(262, sys_newfstatat, "newfstatat");
    set(263, sys_unlinkat, "unlinkat");
    set(266, sys_symlinkat, "symlinkat");
    set(267, sys_readlinkat, "readlinkat");
    set(268, sys_fchmodat, "fchmodat");
    set(269, sys_faccessat, "faccessat");
    set(292, sys_dup3, "dup3");
    set(439, sys_faccessat, "faccessat2");
}

} // namespace syscall
