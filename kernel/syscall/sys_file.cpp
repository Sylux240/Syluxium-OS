// File and filesystem system calls.

#include <block/block.hpp>
#include <fs/ext2.hpp>
#include <core/log.hpp>
#include <fs/exfat.hpp>
#include <fs/inotify.hpp>
#include <fs/fat.hpp>
#include <fs/pipe.hpp>
#include <fs/ramfs.hpp>
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

void join_path(const char* cwd, const char* path, char* out, usize out_size);

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
    st->st_dev = n->dev;
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
    if (offset >= 0) {
        isize n = f->inode->write_file(f, static_cast<u64>(offset), reinterpret_cast<const void*>(user_buf), count);
        if (n > 0 && is_reg(f->inode->mode))
            inotify::file_event(f, inotify::IN_MODIFY);
        return n;
    }
    LockGuard guard(f->lock);
    if (f->flags & O_APPEND)
        f->offset = f->inode->size;
    isize n = f->inode->write_file(f, f->offset, reinterpret_cast<const void*>(user_buf), count);
    if (n > 0 && f->inode->seekable())
        f->offset += static_cast<u64>(n);
    if (n > 0 && is_reg(f->inode->mode))
        inotify::file_event(f, inotify::IN_MODIFY);
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
        if ((r = proc::permission(parent, proc::MAY_WRITE | proc::MAY_EXEC)) < 0)
            return r;
        if ((r = parent->create(name, len, S_IFREG | (mode & ~p->umask & 07777), nullptr, &inode)) < 0)
            return r;
        proc::init_owner(parent, inode);
        inotify::child_event(parent, inotify::IN_CREATE, name, len);
    } else if (r < 0) {
        return r;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -err::EXIST;
    } else if (!(flags & O_PATH)) { // an existing file: may we open it this way?
        int want = 0;
        u32 acc = flags & O_ACCMODE;
        if (acc == O_RDONLY || acc == O_RDWR)
            want |= proc::MAY_READ;
        if (acc == O_WRONLY || acc == O_RDWR || (flags & O_TRUNC))
            want |= proc::MAY_WRITE;
        if (long e = proc::permission(inode, want); e < 0)
            return e;
    }

    if ((flags & O_DIRECTORY) && !is_dir(inode->mode))
        return -err::NOTDIR;
    if (is_dir(inode->mode) && (flags & O_ACCMODE) != O_RDONLY)
        return -err::ISDIR;
    if (is_lnk(inode->mode) && !(flags & O_PATH))
        return -err::LOOP; // O_NOFOLLOW on a symlink
    if ((flags & O_TRUNC) && is_reg(inode->mode) && (flags & O_ACCMODE) != O_RDONLY && inode->size) {
        inode->truncate(0);
        inotify::self_event(inode, inotify::IN_MODIFY);
    }

    inode = inode->open_redirect();
    if ((inode->mode & S_IFMT) == S_IFIFO && !(flags & O_PATH)) { // a named pipe
        inode = pipe::fifo_for(inode);
        if (!inode)
            return -err::NOMEM;
    }
    File* f = open_inode(inode, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC) /* O_NOCTTY stays: terminals check it */);
    {
        char abs[512];
        const char* base_path = p->cwd_path;
        char dir_path[512];
        if (path.c_str()[0] != '/' && dirfd != AT_FDCWD) {
            FileRef dir(p, dirfd);
            if (dir && dir->path) {
                snprintf(dir_path, sizeof(dir_path), "%s", dir->path);
                base_path = dir_path;
            }
        }
        join_path(base_path, path.c_str(), abs, sizeof(abs));
        file_set_path(f, abs);
    }
    if (is_reg(inode->mode) || is_dir(inode->mode))
        inotify::file_event(f, inotify::IN_OPEN);
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
    return f->inode->ioctl_file(f.get(), a1, a2); // checks the argument buffer (see vfs::Inode)
}

long sys_access(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    UserPath path(a0);
    if (path.error())
        return path.error();
    Inode* n;
    if (long r = resolve(path.c_str(), proc::current()->cwd, true, &n); r < 0)
        return r;
    return a1 ? proc::permission(n, static_cast<int>(a1 & 7), true) : 0; // checked with the real ids
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
    if (long r = resolve(path.c_str(), base, !(a3 & AT_SYMLINK_NOFOLLOW), &n); r < 0)
        return r;
    constexpr u64 AT_EACCESS = 0x200;
    return a2 ? proc::permission(n, static_cast<int>(a2 & 7), !(a3 & AT_EACCESS)) : 0;
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
    case 1033: // F_ADD_SEALS (memfd)
        return f->inode->seals(true, static_cast<u32>(a2));
    case 1034: // F_GET_SEALS
        return f->inode->seals(false, 0);
    case 5:  // F_GETLK: advisory locks always succeed (single-user system):
    case 36: { // F_OFD_GETLK  report "unlocked" (l_type = F_UNLCK)
        i16 unlocked = 2;
        return uaccess::put(reinterpret_cast<i16*>(a2), unlocked);
    }
    case 6: case 7: case 37: case 38: // F_SETLK, F_SETLKW, F_OFD_SETLK, F_OFD_SETLKW
        return 0;
    case 1031: // F_SETPIPE_SZ
    case 1032: // F_GETPIPE_SZ
        return 64 * 1024;
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
    if (long r = proc::permission(n, proc::MAY_EXEC); r < 0)
        return r;
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
    if (parent->lookup(name, len))
        return -err::EXIST; // "exists" wins over "no permission", as on Linux (mkdir -p relies on it)
    if (long r = proc::permission(parent, proc::MAY_WRITE | proc::MAY_EXEC); r < 0)
        return r;
    Inode* made = nullptr;
    long r = parent->create(name, len, S_IFDIR | (mode & ~p->umask & 07777), nullptr, &made);
    if (r == 0) {
        if (!made)
            made = parent->lookup(name, len);
        if (made)
            proc::init_owner(parent, made);
        inotify::child_event(parent, inotify::IN_CREATE | inotify::IN_ISDIR, name, len);
    }
    return r;
}

// mknod/mknodat: named pipes (FIFOs) and plain files. Device nodes are made
// by the kernel itself (devtmpfs-style), so only root may ask, and gets
// EPERM like an unprivileged container would.
long do_mknodat(long dirfd, u64 user_path, u32 mode) {
    Process* p = proc::current();
    u32 type = mode & S_IFMT;
    if (type == S_IFCHR || type == S_IFBLK)
        return -err::PERM;
    if (type != 0 && type != S_IFREG && type != S_IFIFO)
        return -err::INVAL; // sockets are made by bind()
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
    if (parent->lookup(name, len))
        return -err::EXIST; // "exists" wins over "no permission", as on Linux (mkdir -p relies on it)
    if (long r = proc::permission(parent, proc::MAY_WRITE | proc::MAY_EXEC); r < 0)
        return r;
    Inode* made = nullptr;
    long r = parent->create(name, len, (type ? type : S_IFREG) | (mode & ~p->umask & 07777), nullptr, &made);
    if (r == 0) {
        if (!made)
            made = parent->lookup(name, len);
        if (made)
            proc::init_owner(parent, made);
        inotify::child_event(parent, inotify::IN_CREATE, name, len);
    }
    return r;
}

long sys_mknod(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_mknodat(AT_FDCWD, a0, static_cast<u32>(a1)); }
long sys_mknodat(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_mknodat(static_cast<i32>(a0), a1, static_cast<u32>(a2)); }

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
    // Files report IN_DELETE_SELF when they're really destroyed (last link
    // and last user gone: inotify::inode_gone from the destructor); `victim`
    // may already be freed after unlink, so it's only used for directories.
    if (long r = proc::may_delete(parent, parent->lookup(name, len)); r < 0)
        return r;
    Inode* victim = (inotify::active() && dir) ? parent->lookup(name, len) : nullptr;
    long r = parent->unlink(name, len, dir);
    if (r == 0) {
        inotify::child_event(parent, inotify::IN_DELETE | (dir ? inotify::IN_ISDIR : 0), name, len);
        if (victim)
            inotify::inode_gone(victim); // directories: IN_DELETE_SELF + IN_IGNORED
    }
    return r;
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
    if (parent->lookup(name, len))
        return -err::EXIST; // "exists" wins over "no permission", as on Linux (mkdir -p relies on it)
    if (long r = proc::permission(parent, proc::MAY_WRITE | proc::MAY_EXEC); r < 0)
        return r;
    Inode* made = nullptr;
    long r = parent->create(name, len, S_IFLNK | 0777, target.c_str(), &made);
    if (r == 0) {
        if (!made)
            made = parent->lookup(name, len);
        if (made)
            proc::init_owner(parent, made);
        inotify::child_event(parent, inotify::IN_CREATE, name, len);
    }
    return r;
}

// link/linkat: hard links (same tmpfs only; X servers use them for lock files).
long do_linkat(long olddirfd, u64 old_user, long newdirfd, u64 new_user, u32 flags) {
    constexpr u32 AT_SYMLINK_FOLLOW = 0x400;
    Process* p = proc::current();
    UserPath oldp(old_user), newp(new_user);
    if (oldp.error()) return oldp.error();
    if (newp.error()) return newp.error();
    Inode *obase, *nbase;
    if (long r = dir_for(p, olddirfd, oldp.c_str(), &obase); r < 0) return r;
    if (long r = dir_for(p, newdirfd, newp.c_str(), &nbase); r < 0) return r;
    Inode* target;
    if (long r = resolve(oldp.c_str(), obase, flags & AT_SYMLINK_FOLLOW, &target); r < 0)
        return r;
    if (is_dir(target->mode))
        return -err::PERM;
    Inode* dir;
    const char* name;
    usize len;
    if (long r = resolve_parent(newp.c_str(), nbase, &dir, &name, &len); r < 0)
        return r;
    if (long r = proc::permission(dir, proc::MAY_WRITE | proc::MAY_EXEC); r < 0)
        return r;
    // protected_hardlinks: only to files we own or could read and write.
    if (!proc::owns(target) && proc::permission(target, proc::MAY_READ | proc::MAY_WRITE) < 0)
        return -err::PERM;
    if (dir->lookup(name, len))
        return -err::EXIST;
    if (target->dev != dir->dev)
        return -err::XDEV;
    if (long r = dir->link(name, len, target); r < 0)
        return r;
    inotify::child_event(dir, inotify::IN_CREATE, name, len);
    return 0;
}

long sys_link(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_linkat(AT_FDCWD, a0, AT_FDCWD, a1, 0); }
long sys_linkat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return do_linkat(static_cast<i32>(a0), a1, static_cast<i32>(a2), a3, static_cast<u32>(a4));
}

// Extended attributes: none are stored (tmpfs/ext2 without xattrs).
long sys_getxattr(SYSCALL_ARGS) { SYSCALL_UNUSED; return -err::NODATA; }
long sys_listxattr(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; }
long sys_setxattr(SYSCALL_ARGS) { SYSCALL_UNUSED; return -err::OPNOTSUPP; }
long sys_removexattr(SYSCALL_ARGS) { SYSCALL_UNUSED; return -err::NODATA; }
long sys_fadvise64(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; } // advice only

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
    long r = f->inode->truncate(a1);
    if (r == 0)
        inotify::file_event(f.get(), inotify::IN_MODIFY);
    return r;
}

long sys_truncate(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    UserPath path(a0);
    if (path.error())
        return path.error();
    Inode* n;
    if (long r = resolve(path.c_str(), proc::current()->cwd, true, &n); r < 0)
        return r;
    if (long r = proc::permission(n, proc::MAY_WRITE); r < 0)
        return r;
    long r = n->truncate(a1);
    if (r == 0)
        inotify::self_event(n, inotify::IN_MODIFY);
    return r;
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
    char name[48];
    snprintf(name, sizeof(name), "pipe:[%lu]", rd->inode->ino);
    file_set_path(rd, name);
    file_set_path(wr, name);
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
    if (a3) { // temporary signal mask while waiting (restored on the way back to user mode)
        if (a4 != 8)
            return -err::INVAL;
        u64 mask;
        if (long r = uaccess::get(&mask, reinterpret_cast<u64*>(a3)); r < 0)
            return r;
        sched::Thread* t = sched::current();
        t->saved_sigmask = t->sigmask;
        t->restore_sigmask = true;
        t->sigmask = mask & ~((1ull << 8) | (1ull << 18)); // SIGKILL/SIGSTOP stay unblockable
    }
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
    if (a5) { // struct { const sigset_t* ss; size_t ss_len; }: temporary signal mask
        u64 arg[2];
        if (long r = uaccess::copy_from(arg, reinterpret_cast<void*>(a5), sizeof(arg)); r < 0)
            return r;
        if (arg[0]) {
            if (arg[1] != 8)
                return -err::INVAL;
            u64 mask;
            if (long r = uaccess::get(&mask, reinterpret_cast<u64*>(arg[0])); r < 0)
                return r;
            sched::Thread* t = sched::current();
            t->saved_sigmask = t->sigmask;
            t->restore_sigmask = true;
            t->sigmask = mask & ~((1ull << 8) | (1ull << 18));
        }
    }
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
    if (!proc::owns(n))
        return -err::PERM;
    u32 m = static_cast<u32>(mode) & 07777;
    if (proc::cred().fsuid != 0 && !proc::in_group(n->gid))
        m &= ~S_ISGID; // can't make a file setgid to a group we're not in
    long r = n->set_owner(m, n->uid, n->gid);
    if (r == 0)
        inotify::self_event(n, inotify::IN_ATTRIB);
    return r;
}

// chown: only root gives files away; an owner may change the group to one
// of their own groups. -1 leaves an id alone. Changing ownership clears
// setuid (and group-executable setgid) bits, as on Linux.
long chown_inode(Inode* n, u32 uid, u32 gid) {
    const proc::Cred& c = proc::cred();
    u32 new_uid = uid == 0xFFFFFFFF ? n->uid : uid;
    u32 new_gid = gid == 0xFFFFFFFF ? n->gid : gid;
    if (c.euid != 0) {
        if (new_uid != n->uid || c.fsuid != n->uid)
            return -err::PERM;
        if (new_gid != n->gid && !proc::in_group(new_gid))
            return -err::PERM;
    }
    u32 mode = n->mode & 07777;
    if (is_reg(n->mode) && (uid != 0xFFFFFFFF || gid != 0xFFFFFFFF)) {
        mode &= ~S_ISUID;
        if (mode & 0010)
            mode &= ~S_ISGID;
    }
    long r = n->set_owner(mode, new_uid, new_gid);
    if (r == 0)
        inotify::self_event(n, inotify::IN_ATTRIB);
    return r;
}

long do_fchownat(long dirfd, u64 user_path, u32 uid, u32 gid, u32 flags) {
    Process* p = proc::current();
    Inode* n;
    UserPath path(user_path);
    if (path.error())
        return path.error();
    if (path.c_str()[0] == 0 && (flags & AT_EMPTY_PATH)) {
        FileRef f(p, dirfd);
        if (!f)
            return -err::BADF;
        n = f->inode;
    } else {
        Inode* base;
        if (long r = dir_for(p, dirfd, path.c_str(), &base); r < 0)
            return r;
        if (long r = resolve(path.c_str(), base, !(flags & AT_SYMLINK_NOFOLLOW), &n); r < 0)
            return r;
    }
    return chown_inode(n, uid, gid);
}

long sys_chown(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_fchownat(AT_FDCWD, a0, static_cast<u32>(a1), static_cast<u32>(a2), 0); }
long sys_lchown(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return do_fchownat(AT_FDCWD, a0, static_cast<u32>(a1), static_cast<u32>(a2), AT_SYMLINK_NOFOLLOW);
}
long sys_fchownat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return do_fchownat(static_cast<i32>(a0), a1, static_cast<u32>(a2), static_cast<u32>(a3), static_cast<u32>(a4));
}
long sys_fchown(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    return f ? chown_inode(f->inode, static_cast<u32>(a1), static_cast<u32>(a2)) : -err::BADF;
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

// fchmodat2(dirfd, path, mode, flags): fchmodat with flags (Linux 6.6+;
// coreutils uses it). AT_SYMLINK_NOFOLLOW on a symlink: links have no mode.
long sys_fchmodat2(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* p = proc::current();
    UserPath path(a1);
    if (path.error())
        return path.error();
    Inode* n;
    u32 flags = static_cast<u32>(a3);
    if (path.c_str()[0] == 0 && (flags & AT_EMPTY_PATH)) {
        FileRef f(p, static_cast<i32>(a0));
        if (!f)
            return -err::BADF;
        n = f->inode;
    } else {
        Inode* base;
        if (long r = dir_for(p, static_cast<i32>(a0), path.c_str(), &base); r < 0)
            return r;
        if (long r = resolve(path.c_str(), base, !(flags & AT_SYMLINK_NOFOLLOW), &n); r < 0)
            return r;
        if (is_lnk(n->mode))
            return -err::OPNOTSUPP;
    }
    return chmod_inode(n, a2);
}

// Networking arrives in phase 6; programs probing for sockets get clean errors.
long sys_socket(SYSCALL_ARGS) { SYSCALL_UNUSED; return -97; /* EAFNOSUPPORT */ }
long sys_not_socket(SYSCALL_ARGS) { SYSCALL_UNUSED; return -88; /* ENOTSOCK */ }

// ---- statfs ----

struct LinuxStatfs {
    i64 f_type, f_bsize;
    u64 f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    i32 f_fsid[2];
    i64 f_namelen, f_frsize, f_flags;
    i64 f_spare[4];
};
static_assert(sizeof(LinuxStatfs) == 120);

// Any inode can report the numbers for the file system it lives on.
long fill_statfs(Inode* n, u64 user_buf) {
    Inode::FsStats st{};
    if (long r = n->statfs(&st); r < 0)
        return r;
    LinuxStatfs out{};
    out.f_type = static_cast<i64>(st.type);
    out.f_bsize = out.f_frsize = static_cast<i64>(st.block_size);
    out.f_blocks = st.blocks;
    out.f_bfree = out.f_bavail = st.blocks_free;
    out.f_files = st.files;
    out.f_ffree = st.files_free;
    out.f_namelen = 255;
    return uaccess::put(reinterpret_cast<LinuxStatfs*>(user_buf), out);
}

long sys_statfs(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    UserPath path(a0);
    if (path.error())
        return path.error();
    Inode* n;
    if (long r = resolve(path.c_str(), proc::current()->cwd, true, &n); r < 0)
        return r;
    return fill_statfs(n, a1);
}

long sys_fstatfs(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    return f ? fill_statfs(f->inode, a1) : -err::BADF;
}

// ---- timestamps ----

long do_utimensat(long dirfd, u64 user_path, u64 user_times, u32 flags) {
    constexpr i64 UTIME_NOW = (1l << 30) - 1, UTIME_OMIT = (1l << 30) - 2;
    Process* p = proc::current();
    Inode* inode;
    if (user_path) {
        UserPath path(user_path);
        if (path.error())
            return path.error();
        Inode* base;
        if (long r = dir_for(p, dirfd, path.c_str(), &base); r < 0)
            return r;
        if (long r = resolve(path.c_str(), base, !(flags & AT_SYMLINK_NOFOLLOW), &inode); r < 0)
            return r;
    } else { // futimens(fd, ...)
        FileRef f(p, dirfd);
        if (!f)
            return -err::BADF;
        inode = f->inode;
    }
    Timespec now = vfs::now(), access = now, modify = now;
    bool explicit_times = false;
    if (user_times) {
        struct { i64 sec, nsec; } ts[2];
        if (long r = uaccess::copy_from(ts, reinterpret_cast<void*>(user_times), sizeof(ts)); r < 0)
            return r;
        access = ts[0].nsec == UTIME_NOW ? now : ts[0].nsec == UTIME_OMIT ? inode->atime : Timespec{ts[0].sec, ts[0].nsec};
        modify = ts[1].nsec == UTIME_NOW ? now : ts[1].nsec == UTIME_OMIT ? inode->mtime : Timespec{ts[1].sec, ts[1].nsec};
        explicit_times = (ts[0].nsec != UTIME_NOW && ts[0].nsec != UTIME_OMIT) ||
                         (ts[1].nsec != UTIME_NOW && ts[1].nsec != UTIME_OMIT);
    }
    // Setting "now" needs write access; setting other times needs ownership.
    if (!proc::owns(inode) && (explicit_times || proc::permission(inode, proc::MAY_WRITE) < 0))
        return explicit_times ? -err::PERM : -err::ACCES;
    long r = inode->set_times(access, modify);
    if (r == 0)
        inotify::self_event(inode, inotify::IN_ATTRIB);
    return r;
}

long sys_utimensat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return do_utimensat(static_cast<i32>(a0), a1, a2, static_cast<u32>(a3));
}

long sys_utimes(SYSCALL_ARGS) { // utimes(path, timeval[2])
    SYSCALL_UNUSED;
    if (!a1)
        return do_utimensat(AT_FDCWD, a0, 0, 0);
    struct { i64 sec, usec; } tv[2];
    if (long r = uaccess::copy_from(tv, reinterpret_cast<void*>(a1), sizeof(tv)); r < 0)
        return r;
    UserPath path(a0);
    if (path.error())
        return path.error();
    Inode* inode;
    if (long r = resolve(path.c_str(), proc::current()->cwd, true, &inode); r < 0)
        return r;
    if (!proc::owns(inode))
        return -err::PERM;
    return inode->set_times(Timespec{tv[0].sec, tv[0].usec * 1000}, Timespec{tv[1].sec, tv[1].usec * 1000});
}

// ---- rename ----

long do_renameat(long olddirfd, u64 old_user, long newdirfd, u64 new_user, u32 flags) {
    constexpr u32 RENAME_NOREPLACE = 1;
    if (flags & ~RENAME_NOREPLACE)
        return -err::INVAL; // RENAME_EXCHANGE / WHITEOUT: not yet
    Process* p = proc::current();
    UserPath oldp(old_user), newp(new_user);
    if (oldp.error()) return oldp.error();
    if (newp.error()) return newp.error();
    Inode *obase, *nbase;
    if (long r = dir_for(p, olddirfd, oldp.c_str(), &obase); r < 0) return r;
    if (long r = dir_for(p, newdirfd, newp.c_str(), &nbase); r < 0) return r;
    Inode *odir, *ndir;
    const char *oname, *nname;
    usize olen, nlen;
    if (long r = resolve_parent(oldp.c_str(), obase, &odir, &oname, &olen); r < 0) return r;
    if (long r = resolve_parent(newp.c_str(), nbase, &ndir, &nname, &nlen); r < 0) return r;
    Inode* existing = ndir->lookup(nname, nlen);
    if ((flags & RENAME_NOREPLACE) && existing)
        return -err::EXIST;
    if (long r = proc::may_delete(odir, odir->lookup(oname, olen)); r < 0)
        return r;
    if (long r = existing ? proc::may_delete(ndir, existing) : proc::permission(ndir, proc::MAY_WRITE | proc::MAY_EXEC); r < 0)
        return r;
    Inode* moved = inotify::active() ? odir->lookup(oname, olen) : nullptr;
    long r = odir->rename(oname, olen, ndir, nname, nlen);
    if (r == 0 && moved) {
        u32 isdir = is_dir(moved->mode) ? inotify::IN_ISDIR : 0;
        u32 cookie = inotify::next_cookie();
        inotify::child_event(odir, inotify::IN_MOVED_FROM | isdir, oname, olen, cookie);
        inotify::child_event(ndir, inotify::IN_MOVED_TO | isdir, nname, nlen, cookie);
        inotify::self_event(moved, inotify::IN_MOVE_SELF);
    }
    return r;
}

long sys_rename(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_renameat(AT_FDCWD, a0, AT_FDCWD, a1, 0); }
long sys_renameat(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_renameat(static_cast<i32>(a0), a1, static_cast<i32>(a2), a3, 0); }
long sys_renameat2(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return do_renameat(static_cast<i32>(a0), a1, static_cast<i32>(a2), a3, static_cast<u32>(a4));
}

// ---- sync / mount ----

long sys_sync(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    block::sync_all();
    return 0;
}

long sys_mount(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (!proc::capable())
        return -err::PERM;
    UserPath source(a0), target(a1), type(a2);
    if (target.error()) return target.error();
    if (type.error()) return type.error();
    char abs[512];
    join_path(proc::current()->cwd_path, target.c_str(), abs, sizeof(abs));

    const char* t = type.c_str();
    // No type (or "auto"): detect it from the device.
    if ((!t[0] || strcmp(t, "auto") == 0) && !source.error()) {
        const char* src = source.c_str();
        if (strncmp(src, "/dev/", 5) == 0)
            src += 5;
        if (block::Device* dev = block::find(src))
            t = exfat::probe(dev) ? "exfat" : fat::probe(dev) ? "vfat" : ext2::probe(dev) ? "ext4" : t;
    }
    if (strcmp(t, "exfat") == 0) {
        if (source.error()) return source.error();
        const char* src = source.c_str();
        if (strncmp(src, "/dev/", 5) == 0)
            src += 5;
        block::Device* dev = block::find(src);
        if (!dev)
            return -err::NODEV;
        Inode* root;
        if (long r = exfat::mount(dev, &root); r < 0)
            return r;
        return vfs::mount_at(abs, root, source.c_str(), "exfat");
    }
    if (strcmp(t, "vfat") == 0 || strcmp(t, "fat") == 0 || strcmp(t, "msdos") == 0) {
        if (source.error()) return source.error();
        const char* src = source.c_str();
        if (strncmp(src, "/dev/", 5) == 0)
            src += 5;
        block::Device* dev = block::find(src);
        if (!dev)
            return -err::NODEV;
        Inode* root;
        if (long r = fat::mount(dev, &root); r < 0)
            return r;
        return vfs::mount_at(abs, root, source.c_str(), "vfat");
    }
    if (strcmp(t, "ext2") == 0 || strcmp(t, "ext3") == 0 || strcmp(t, "ext4") == 0) {
        if (source.error()) return source.error();
        const char* src = source.c_str();
        if (strncmp(src, "/dev/", 5) == 0)
            src += 5;
        block::Device* dev = block::find(src);
        if (!dev)
            return -err::NODEV;
        Inode* root;
        bool ro;
        const char* kind;
        if (long r = ext2::mount(dev, &root, &ro, &kind); r < 0)
            return r;
        return vfs::mount_at(abs, root, source.c_str(), kind);
    }
    if (strcmp(t, "tmpfs") == 0 || strcmp(t, "ramfs") == 0) {
        auto* root = new ramfs::Dir;
        root->mode = S_IFDIR | 01777;
        return vfs::mount_at(abs, root, "tmpfs", "tmpfs");
    }
    return -err::NODEV; // unknown file system type
}

long sys_umount2(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (!proc::capable())
        return -err::PERM;
    UserPath target(a0);
    if (target.error())
        return target.error();
    char abs[512];
    join_path(proc::current()->cwd_path, target.c_str(), abs, sizeof(abs));
    block::sync_all();
    return vfs::unmount(abs);
}

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
    set(74, sys_sync, "fsync");
    set(75, sys_sync, "fdatasync");
    set(165, sys_mount, "mount");
    set(166, sys_umount2, "umount2");
    set(306, sys_sync, "syncfs");
    set(76, sys_truncate, "truncate");
    set(77, sys_ftruncate, "ftruncate");
    set(79, sys_getcwd, "getcwd");
    set(80, sys_chdir, "chdir");
    set(133, sys_mknod, "mknod");
    set(259, sys_mknodat, "mknodat");
    set(82, sys_rename, "rename");
    set(137, sys_statfs, "statfs");
    set(138, sys_fstatfs, "fstatfs");
    set(235, sys_utimes, "utimes");
    set(280, sys_utimensat, "utimensat");
    set(264, sys_renameat, "renameat");
    set(316, sys_renameat2, "renameat2");
    set(83, sys_mkdir, "mkdir");
    set(84, sys_rmdir, "rmdir");
    set(85, sys_creat, "creat");
    set(87, sys_unlink, "unlink");
    set(86, sys_link, "link");
    set(188, sys_setxattr, "setxattr");
    set(189, sys_setxattr, "lsetxattr");
    set(190, sys_setxattr, "fsetxattr");
    set(191, sys_getxattr, "getxattr");
    set(192, sys_getxattr, "lgetxattr");
    set(193, sys_getxattr, "fgetxattr");
    set(194, sys_listxattr, "listxattr");
    set(195, sys_listxattr, "llistxattr");
    set(196, sys_listxattr, "flistxattr");
    set(197, sys_removexattr, "removexattr");
    set(198, sys_removexattr, "lremovexattr");
    set(199, sys_removexattr, "fremovexattr");
    set(221, sys_fadvise64, "fadvise64");
    set(88, sys_symlink, "symlink");
    set(265, sys_linkat, "linkat");
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
    set(92, sys_chown, "chown");
    set(93, sys_fchown, "fchown");
    set(94, sys_lchown, "lchown");
    set(95, sys_umask, "umask");
    set(162, sys_sync, "sync");
    set(217, sys_getdents64, "getdents64");
    set(257, sys_openat, "openat");
    set(258, sys_mkdirat, "mkdirat");
    set(260, sys_fchownat, "fchownat");
    set(262, sys_newfstatat, "newfstatat");
    set(263, sys_unlinkat, "unlinkat");
    set(266, sys_symlinkat, "symlinkat");
    set(267, sys_readlinkat, "readlinkat");
    set(268, sys_fchmodat, "fchmodat");
    set(452, sys_fchmodat2, "fchmodat2");
    set(269, sys_faccessat, "faccessat");
    set(292, sys_dup3, "dup3");
    set(439, sys_faccessat, "faccessat2");
}

} // namespace syscall
