// File and filesystem system calls.

#include <arch/x86_64/protect.hpp>
#include <block/block.hpp>
#include <fs/ext2.hpp>
#include <core/log.hpp>
#include <fs/exfat.hpp>
#include <fs/inotify.hpp>
#include <fs/fat.hpp>
#include <fs/fuse.hpp>
#include <fs/pipe.hpp>
#include <block/loop.hpp>
#include <fs/cgroupfs.hpp>
#include <fs/ramfs.hpp>
#include <fs/poll.hpp>
#include <fs/procfs.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <time/clock.hpp>
#include <syscall/syscall.hpp>

namespace linuxkpi {
vfs::Inode* binderfs_mount(); // kernel/linuxkpi/binder.cpp
}

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
    if (Process* p = proc::current(); p && p->userns) { // owners as the user namespace names them
        st->st_uid = proc::uid_to_ns(n->uid);
        st->st_gid = proc::gid_to_ns(n->gid);
    }
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

// The open file's lock guards its offset. Pipes, sockets and anonymous
// event files (eventfd, timerfd, ...) have none, and one thread or process
// commonly blocks reading one while another writes it (bubblewrap wakes its
// child through a shared eventfd): they go unlocked, as on Linux.
bool offset_locked(const File* f) {
    u32 type = f->inode->mode & S_IFMT;
    return f->inode->seekable() || !(f->inode->anon_kind || type == S_IFIFO || type == S_IFSOCK);
}

// read/write at the file's offset (or a given one for pread/pwrite).
long do_read(File* f, u64 user_buf, u64 count, i64 offset = -1) {
    if (!can_read(f) || (f->flags & O_PATH))
        return -err::BADF;
    if (is_dir(f->inode->mode))
        return -err::ISDIR;
    if (long r = uaccess::check(reinterpret_cast<void*>(user_buf), count, true); r < 0)
        return r;
    // File systems and drivers fill the user buffer directly: a SMAP window
    // for the duration of the read (and of the write below).
    protect::UserAccess window;
    if (offset >= 0 || !offset_locked(f))
        return f->inode->read_file(f, offset >= 0 ? static_cast<u64>(offset) : 0, reinterpret_cast<void*>(user_buf), count);
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
    protect::UserAccess window;
    if (offset >= 0) {
        isize n = f->inode->write_file(f, static_cast<u64>(offset), reinterpret_cast<const void*>(user_buf), count);
        if (n > 0 && is_reg(f->inode->mode))
            inotify::file_event(f, inotify::IN_MODIFY);
        return n;
    }
    if (!offset_locked(f))
        return f->inode->write_file(f, 0, reinterpret_cast<const void*>(user_buf), count);
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

long do_openat_path(long dirfd, const char* kpath, u32 flags, u32 mode) {
    Process* p = proc::current();

    Inode* base;
    if (long r = dir_for(p, dirfd, kpath, &base); r < 0)
        return r;

    Inode* inode = nullptr;
    long r = resolve(kpath, base, !(flags & O_NOFOLLOW), &inode);
    if (r == -err::NOENT && (flags & O_CREAT)) {
        Inode* parent;
        const char* name;
        usize len;
        if ((r = resolve_parent(kpath, base, &parent, &name, &len)) < 0)
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
    if (is_lnk(inode->mode) && !(flags & O_PATH) && !inode->magic_link())
        return -err::LOOP; // O_NOFOLLOW on a symlink
    if ((flags & O_TRUNC) && is_reg(inode->mode) && (flags & O_ACCMODE) != O_RDONLY && inode->size) {
        inode->truncate(0);
        inotify::self_event(inode, inotify::IN_MODIFY);
    }

    inode = inode->open_redirect();
    if ((inode->mode & S_IFMT) == S_IFSOCK && !(flags & O_PATH))
        return -err::NXIO; // sockets are not opened, as on Linux (reached through /proc/self/fd/N)
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
        if (kpath[0] != '/' && dirfd != AT_FDCWD) {
            FileRef dir(p, dirfd);
            if (dir && dir->path) {
                snprintf(dir_path, sizeof(dir_path), "%s", dir->path);
                base_path = dir_path;
            }
        }
        join_path(base_path, kpath, abs, sizeof(abs));
        file_set_path(f, abs);
    }
    if (is_reg(inode->mode) || is_dir(inode->mode))
        inotify::file_event(f, inotify::IN_OPEN);
    long fd = proc::fd_install(p, f, 0, flags & O_CLOEXEC);
    if (fd < 0)
        file_put(f);
    return fd;
}

long do_openat(long dirfd, u64 user_path, u32 flags, u32 mode) {
    UserPath path(user_path);
    if (path.error())
        return path.error();
    return do_openat_path(dirfd, path.c_str(), flags, mode);
}

// The inode fstatat/statx look at: path relative to dirfd, or the fd itself
// (AT_EMPTY_PATH with an empty path).
long stat_target(long dirfd, u64 user_path, u32 flags, Inode** out) {
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
    if (long r = inode->revalidate(); r < 0)
        return r;
    *out = inode;
    return 0;
}

long do_fstatat(long dirfd, u64 user_path, u64 user_stat, u32 flags) {
    Inode* inode;
    if (long r = stat_target(dirfd, user_path, flags, &inode); r < 0)
        return r;
    LinuxStat st;
    fill_stat(inode, &st);
    return uaccess::put(reinterpret_cast<LinuxStat*>(user_stat), st);
}

// statx(2): the same information in struct statx (glibc's stat() uses it
// when available; Wine asks for it directly).
struct StatxTimestamp {
    i64 tv_sec;
    u32 tv_nsec;
    i32 reserved;
};

struct Statx {
    u32 stx_mask, stx_blksize;
    u64 stx_attributes;
    u32 stx_nlink, stx_uid, stx_gid;
    u16 stx_mode, pad0;
    u64 stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    StatxTimestamp stx_atime, stx_btime, stx_ctime, stx_mtime;
    u32 stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    u64 spare[14];
};
static_assert(sizeof(Statx) == 256);

long sys_statx(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u32 STATX_BASIC_STATS = 0x7ff; // type, mode, nlink, uid, gid, times, ino, size, blocks
    constexpr u32 AT_STATX_SYNC_TYPE = 0x6000, AT_NO_AUTOMOUNT = 0x800;
    u32 flags = static_cast<u32>(a2);
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_STATX_SYNC_TYPE))
        return -err::INVAL;
    if ((flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE)
        return -err::INVAL;
    Inode* inode;
    if (long r = stat_target(static_cast<i32>(a0), a1, flags, &inode); r < 0)
        return r;
    LinuxStat st;
    fill_stat(inode, &st);
    // Linux's dev_t encoding: 12-bit major, 20-bit minor.
    auto major = [](u64 d) { return static_cast<u32>(((d >> 8) & 0xfff) | ((d >> 32) & ~0xfffull)); };
    auto minor = [](u64 d) { return static_cast<u32>((d & 0xff) | ((d >> 12) & ~0xffull)); };
    Statx x{};
    x.stx_mask = STATX_BASIC_STATS;
    x.stx_blksize = static_cast<u32>(st.st_blksize);
    x.stx_nlink = static_cast<u32>(st.st_nlink);
    x.stx_uid = st.st_uid;
    x.stx_gid = st.st_gid;
    x.stx_mode = static_cast<u16>(st.st_mode);
    x.stx_ino = st.st_ino;
    x.stx_size = static_cast<u64>(st.st_size);
    x.stx_blocks = static_cast<u64>(st.st_blocks);
    x.stx_atime = {st.st_atime_sec, static_cast<u32>(st.st_atime_nsec), 0};
    x.stx_mtime = {st.st_mtime_sec, static_cast<u32>(st.st_mtime_nsec), 0};
    x.stx_ctime = {st.st_ctime_sec, static_cast<u32>(st.st_ctime_nsec), 0};
    x.stx_rdev_major = major(st.st_rdev);
    x.stx_rdev_minor = minor(st.st_rdev);
    x.stx_dev_major = major(st.st_dev);
    x.stx_dev_minor = minor(st.st_dev);
    return uaccess::put(reinterpret_cast<Statx*>(a4), x);
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

// readv/writev on a socket: one message, as Linux does. Per-iovec writes
// would turn each piece into its own datagram (liblog writes a header, a
// priority, a tag and a message with one writev; logd drops a lone header).
long vectored_socket(File* f, const KIoVec* iov, u64 count, bool write) {
    if (write ? !can_write(f) : !can_read(f))
        return -err::BADF;
    usize total = 0;
    for (u64 i = 0; i < count; i++) {
        if (iov[i].len > 0x7ffff000 - total)
            return -err::INVAL;
        total += iov[i].len;
    }
    constexpr usize MAX_MSG = 256 * 1024; // larger ones go in pieces, like a stream
    usize n = min(total, MAX_MSG);
    u8* k = static_cast<u8*>(kmalloc(n ? n : 1));
    if (!k)
        return -err::NOMEM;
    long r = 0;
    if (write) {
        usize pos = 0;
        for (u64 i = 0; i < count && pos < n && r >= 0; i++) {
            usize part = min<usize>(iov[i].len, n - pos);
            if (part)
                r = uaccess::copy_from(k + pos, reinterpret_cast<const void*>(iov[i].base), part);
            pos += part;
        }
        if (r >= 0)
            r = f->inode->write_file(f, 0, k, n);
    } else {
        r = f->inode->read_file(f, 0, k, n);
        usize pos = 0;
        for (u64 i = 0; r > 0 && i < count && pos < static_cast<usize>(r); i++) {
            usize part = min<usize>(iov[i].len, static_cast<usize>(r) - pos);
            if (long e = part ? uaccess::copy_to(reinterpret_cast<void*>(iov[i].base), k + pos, part) : 0; e < 0) {
                r = e;
                break;
            }
            pos += part;
        }
    }
    kfree(k);
    return r;
}

long vectored(u64 fd, u64 user_iov, u64 count, bool write) {
    FileRef f(proc::current(), static_cast<long>(fd));
    if (!f)
        return -err::BADF;
    if (count > 1024)
        return -err::INVAL;
    if ((f->inode->mode & S_IFMT) == S_IFSOCK) {
        auto* iov = static_cast<KIoVec*>(kmalloc(sizeof(KIoVec) * (count ? count : 1)));
        if (!iov)
            return -err::NOMEM;
        long r = count ? uaccess::copy_from(iov, reinterpret_cast<const void*>(user_iov), sizeof(KIoVec) * count) : 0;
        if (r >= 0)
            r = vectored_socket(f.get(), iov, count, write);
        kfree(iov);
        return r;
    }
    if (write && f->inode->whole_writes()) {
        // Gathered into one buffer and written at once.
        IoVec vecs[64];
        if (count > 64)
            return -err::INVAL;
        if (long r = uaccess::copy_from(vecs, reinterpret_cast<const void*>(user_iov), count * sizeof(IoVec)); r < 0)
            return r;
        u64 len = 0;
        for (u64 i = 0; i < count; i++)
            len += vecs[i].len;
        if (len > 16 * MiB)
            return -err::INVAL;
        auto* buf = static_cast<u8*>(kmalloc(len ? len : 1));
        if (!buf)
            return -err::NOMEM;
        u64 at = 0;
        for (u64 i = 0; i < count; i++) {
            if (vecs[i].len && uaccess::copy_from(buf + at, reinterpret_cast<const void*>(vecs[i].base), vecs[i].len) < 0) {
                kfree(buf);
                return -err::FAULT;
            }
            at += vecs[i].len;
        }
        long n = can_write(f.get()) ? f->inode->write_file(f.get(), 0, buf, len) : -err::BADF;
        kfree(buf);
        return n;
    }
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
// openat2(dirfd, path, how, size): openat with its flags in a struct. The
// RESOLVE_* restrictions (no symlinks, stay beneath dirfd, ...) aren't
// implemented: asking for any answers ENOSYS (callers fall back to openat)
// rather than opening something the caller meant to forbid.
// openat2's RESOLVE_* restrictions, checked by walking the path one
// component at a time before the ordinary open (bubblewrap opens every
// bind-mount source this way): NO_XDEV (no mount crossings), NO_MAGICLINKS
// (/proc/<pid>/fd/N and friends), NO_SYMLINKS, BENEATH (never above the
// start directory: no absolute paths, no ".." past it) and IN_ROOT (the
// start directory acts as "/").
long check_resolve(Inode* base, const char* path, u64 res, u32 open_flags) {
    constexpr u64 NO_XDEV = 1, NO_MAGICLINKS = 2, NO_SYMLINKS = 4, BENEATH = 8, IN_ROOT = 0x10;
    bool scoped = res & (BENEATH | IN_ROOT);
    if (path[0] == '/' && (res & BENEATH))
        return -err::XDEV;
    Inode* stack[64];
    int depth = 0;
    Inode* cur = base;
    const char* p = path;
    for (;;) {
        while (*p == '/')
            p++;
        if (!*p)
            return 0;
        const char* start = p;
        while (*p && *p != '/')
            p++;
        usize len = static_cast<usize>(p - start);
        const char* rest = p;
        while (*rest == '/')
            rest++;
        bool last = !*rest;
        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (!scoped)
                return 0; // ".." without a scope: nothing left to restrict but symlinks; let the open walk it
            if (depth == 0) {
                if (res & BENEATH)
                    return -err::XDEV;
                continue; // IN_ROOT: ".." at the root stays there
            }
            cur = stack[--depth];
            continue;
        }
        char name[256];
        if (len >= sizeof(name))
            return -err::NAMETOOLONG;
        memcpy(name, start, len);
        name[len] = 0;
        Inode* next;
        if (long r = resolve(name, cur, false, &next); r < 0)
            return last ? 0 : r; // a missing last component is the open's business (O_CREAT)
        if ((res & NO_XDEV) && next->dev != base->dev)
            return -err::XDEV;
        if (is_lnk(next->mode) && !(last && (open_flags & O_NOFOLLOW))) {
            bool magic = next->jump_target() != nullptr || next->magic_link();
            if (magic && (res & NO_MAGICLINKS))
                return -err::LOOP;
            if (magic && scoped)
                return -err::XDEV;
            if (res & NO_SYMLINKS)
                return -err::LOOP;
            if (scoped) {
                char text[256];
                isize n = next->readlink(text, sizeof(text) - 1);
                if (n > 0 && text[0] == '/' && (res & BENEATH))
                    return -err::XDEV;
            }
            if (long r = resolve(name, cur, true, &next); r < 0)
                return last ? 0 : r;
            if ((res & NO_XDEV) && next->dev != base->dev)
                return -err::XDEV;
        }
        if (!last) {
            if (depth == 64)
                return -err::LOOP;
            stack[depth++] = cur;
        }
        cur = next;
    }
}

long sys_openat2(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    struct OpenHow {
        u64 flags, mode, resolve;
    } how{};
    if (a3 < sizeof(how) || a3 > 4096)
        return -err::INVAL;
    if (long r = uaccess::copy_from(&how, reinterpret_cast<const void*>(a2), sizeof(how)); r < 0)
        return r;
    if (how.flags >> 32 || how.mode & ~07777ull || (!(how.flags & (O_CREAT | 020000000 /* O_TMPFILE */)) && how.mode))
        return -err::INVAL;
    if (how.resolve & ~0x3Full)
        return -err::INVAL;
    if (how.resolve & ~0x20ull) { // RESOLVE_CACHED alone needs nothing (everything is "cached" here)
        UserPath path(a1);
        if (path.error())
            return path.error();
        const char* kpath = path.c_str();
        char scoped[PATH_MAX];
        if (how.resolve & 0x10) { // RESOLVE_IN_ROOT: dirfd is "/": "/x" and "../x" stay below it
            usize n = 0;
            for (const char* q = kpath; *q;) {
                while (*q == '/')
                    q++;
                const char* s0 = q;
                while (*q && *q != '/')
                    q++;
                usize len = static_cast<usize>(q - s0);
                if (len == 0 || (len == 1 && s0[0] == '.'))
                    continue;
                if (len == 2 && s0[0] == '.' && s0[1] == '.') {
                    while (n > 0 && scoped[n - 1] != '/')
                        n--;
                    if (n > 0)
                        n--;
                    continue;
                }
                if (n + len + 2 >= sizeof(scoped))
                    return -err::NAMETOOLONG;
                if (n)
                    scoped[n++] = '/';
                memcpy(scoped + n, s0, len);
                n += len;
            }
            if (n == 0)
                scoped[n++] = '.';
            scoped[n] = 0;
            kpath = scoped;
        }
        Inode* base;
        if (long r = dir_for(proc::current(), static_cast<i32>(a0), kpath, &base); r < 0)
            return r;
        if (long r = check_resolve(base, kpath, how.resolve, static_cast<u32>(how.flags)); r < 0)
            return r;
        return do_openat_path(static_cast<i32>(a0), kpath, static_cast<u32>(how.flags), static_cast<u32>(how.mode));
    }
    return do_openat(static_cast<i32>(a0), a1, static_cast<u32>(how.flags), static_cast<u32>(how.mode));
}

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
    if (long r = f->inode->revalidate(); r < 0)
        return r;
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
    Process* p = proc::current();
    FileRef f(p, static_cast<long>(a0));
    if (!f)
        return -err::BADF;
    // Pipes: FIONBIO (Python sets non-blocking mode with it, not fcntl).
    // Sockets and terminals keep their own FIONBIO handling for now: making
    // it take effect everywhere stalled the desktop session's start.
    bool pipe = (f->inode->mode & vfs::S_IFMT) == vfs::S_IFIFO;
    switch (static_cast<u32>(a1)) {
    case 0x5421: { // FIONBIO
        if (!pipe)
            break;
        i32 on;
        if (long r = uaccess::get(&on, reinterpret_cast<const i32*>(a2)); r < 0)
            return r;
        f->flags = on ? (f->flags | O_NONBLOCK) : (f->flags & ~static_cast<u32>(O_NONBLOCK));
        return 0;
    }
    case 0x5451: // FIOCLEX
    case 0x5450: // FIONCLEX
        p->cloexec[static_cast<int>(a0)] = static_cast<u32>(a1) == 0x5451;
        return 0;
    }
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

// fchdir: into the directory an fd refers to (wineserver: its config dir).
long sys_fchdir(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* p = proc::current();
    FileRef f(p, static_cast<long>(a0));
    if (!f)
        return -err::BADF;
    if (!is_dir(f->inode->mode))
        return -err::NOTDIR;
    if (long r = proc::permission(f->inode, proc::MAY_EXEC); r < 0)
        return r;
    p->cwd = f->inode;
    // The path it was opened by (always absolute); getcwd reports it.
    snprintf(p->cwd_path, sizeof(p->cwd_path), "%s", f->path ? f->path : "/");
    return 0;
}

// name_to_handle_at: none of our file systems can export file handles,
// which is what Linux answers for such file systems (Wine asks, then falls
// back to paths).
long sys_name_to_handle_at(SYSCALL_ARGS) { SYSCALL_UNUSED; return -err::OPNOTSUPP; }

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
// The device node in the system's /dev (any depth) with this type and number.
Inode* find_device_node(Inode* dir, u32 type, u64 rdev, int depth) {
    vfs::DirEntry e;
    for (u64 i = 0; dir->readdir(i, &e) > 0; i++) {
        usize len = strlen(e.name);
        if ((len == 1 && e.name[0] == '.') || (len == 2 && e.name[0] == '.' && e.name[1] == '.'))
            continue;
        Inode* n = dir->lookup(e.name, len);
        if (!n)
            continue;
        if ((n->mode & S_IFMT) == type && n->rdev == rdev)
            return n;
        if (depth < 3 && vfs::is_dir(n->mode))
            if (Inode* f = find_device_node(n, type, rdev, depth + 1))
                return f;
    }
    return nullptr;
}

// A device node made by mknod (root; containers build their own /dev): the
// same device as the system's node with that number, under the new name.
long mknod_device(Inode* parent, const char* name, usize len, u32 type, u64 linux_dev) {
    if (!proc::capable())
        return -err::PERM;
    // Linux's dev_t: major in bits 8-19 (and 32+), minor in bits 0-7 and 20-31.
    u32 major = static_cast<u32>(((linux_dev >> 8) & 0xFFF) | ((linux_dev >> 32) & ~0xFFFu));
    u32 minor = static_cast<u32>((linux_dev & 0xFF) | ((linux_dev >> 12) & ~0xFFu));
    u64 rdev = (major << 8) | (minor & 0xFF) | (static_cast<u64>(minor & ~0xFFu) << 12);
    Inode* dev;
    if (vfs::resolve_global("/dev", &dev) < 0)
        return -err::NODEV;
    Inode* node = find_device_node(dev, type, rdev, 0);
    if (!node)
        return -err::NODEV; // no such device here
    if (!parent->is_ramfs_dir())
        return -err::PERM; // tmpfs (where containers make their /dev) only
    if (long r = static_cast<ramfs::Dir*>(parent)->attach(name, len, node); r < 0)
        return r;
    node->nlink++;
    return 0;
}

long do_mknodat(long dirfd, u64 user_path, u32 mode, u64 linux_dev) {
    Process* p = proc::current();
    u32 type = mode & S_IFMT;
    if (type == S_IFCHR || type == S_IFBLK) {
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
            return -err::EXIST;
        return mknod_device(parent, name, len, type, linux_dev);
    }
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

long sys_mknod(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_mknodat(AT_FDCWD, a0, static_cast<u32>(a1), a2); }
long sys_mknodat(SYSCALL_ARGS) { SYSCALL_UNUSED; return do_mknodat(static_cast<i32>(a0), a1, static_cast<u32>(a2), a3); }

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
long sys_readahead(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; } // a hint: the cache reads on demand

// fallocate(fd, mode, offset, len): mode 0 makes sure the range exists
// (the file grows; blocks are allocated as it's written); KEEP_SIZE (1)
// is a pure reservation hint; PUNCH_HOLE|KEEP_SIZE (3) frees the range
// (RAM files: tmpfs, memfd). Other modes aren't supported.
long sys_fallocate(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef f(proc::current(), static_cast<long>(a0));
    if (!f)
        return -err::BADF;
    if (!can_write(f.get()))
        return -err::BADF;
    if (!is_reg(f->inode->mode))
        return -err::NODEV;
    i64 offset = static_cast<i64>(a2), len = static_cast<i64>(a3);
    if (offset < 0 || len <= 0)
        return -err::INVAL;
    if (a1 == 1)
        return 0;
    if (a1 == 3)
        return f->inode->punch_hole(static_cast<u64>(offset), static_cast<u64>(len));
    if (a1 != 0)
        return -err::OPNOTSUPP;
    u64 end = static_cast<u64>(offset + len);
    if (end > f->inode->size)
        return f->inode->truncate(end);
    return 0;
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
        protect::UserAccess window; // rec: in the caller's (user) buffer
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

// copy_file_range(fd_in, off_in*, fd_out, off_out*, len, flags): copy between
// two regular files in the kernel (cp and install use it). A NULL offset
// pointer means the file's own position, which then advances.
long sys_copy_file_range(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a5 != 0)
        return -err::INVAL;
    Process* p = proc::current();
    FileRef in(p, static_cast<long>(a0));
    FileRef out(p, static_cast<long>(a2));
    if (!in || !out)
        return -err::BADF;
    if (!can_read(in.get()) || !can_write(out.get()) || (out->flags & O_APPEND))
        return -err::BADF;
    if (!vfs::is_reg(in->inode->mode) || !vfs::is_reg(out->inode->mode))
        return vfs::is_dir(in->inode->mode) || vfs::is_dir(out->inode->mode) ? -err::ISDIR : -err::INVAL;
    i64 off_in = -1, off_out = -1;
    if (a1 && uaccess::get(&off_in, reinterpret_cast<const i64*>(a1)) < 0)
        return -err::FAULT;
    if (a3 && uaccess::get(&off_out, reinterpret_cast<const i64*>(a3)) < 0)
        return -err::FAULT;
    if ((a1 && off_in < 0) || (a3 && off_out < 0))
        return -err::INVAL;
    u64 rpos = a1 ? static_cast<u64>(off_in) : in->offset;
    u64 wpos = a3 ? static_cast<u64>(off_out) : out->offset;
    u64 len = a4;
    // The same file: the ranges may not overlap.
    if (in->inode == out->inode && rpos < wpos + len && wpos < rpos + len)
        return -err::INVAL;

    constexpr usize CHUNK = 64 * KiB;
    auto* buf = static_cast<u8*>(kmalloc(CHUNK));
    if (!buf)
        return -err::NOMEM;
    u64 total = 0;
    long result = 0;
    while (total < len) {
        usize want = min<u64>(CHUNK, len - total);
        isize n = in->inode->read_file(in.get(), rpos, buf, want);
        if (n <= 0) {
            result = n;
            break;
        }
        isize w = out->inode->write_file(out.get(), wpos, buf, static_cast<usize>(n));
        if (w <= 0) {
            result = w;
            break;
        }
        rpos += static_cast<u64>(w);
        wpos += static_cast<u64>(w);
        total += static_cast<u64>(w);
        if (w < n)
            break;
    }
    kfree(buf);
    if (a1)
        uaccess::put(reinterpret_cast<i64*>(a1), static_cast<i64>(rpos));
    else {
        LockGuard guard(in->lock);
        in->offset = rpos;
    }
    if (a3)
        uaccess::put(reinterpret_cast<i64*>(a3), static_cast<i64>(wpos));
    else {
        LockGuard guard(out->lock);
        out->offset = wpos;
    }
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
    if (sysfs::contains(n))
        st.type = 0x62656572; // SYSFS_MAGIC
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
    long r = odir->rename(oname, olen, vfs::unwrap(ndir), nname, nlen); // (bind-mount wrappers: the real directory)
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

// sync_file_range(fd, offset, nbytes, flags): only a hint to start writing a
// range back early (dpkg gives it for every file it unpacks); the data reaches
// the disk with the next fsync/sync, so nothing needs doing here.
long sys_sync_file_range(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a3 & ~7ul || static_cast<i64>(a1) < 0 || static_cast<i64>(a2) < 0)
        return -err::INVAL;
    return 0;
}

long sys_mount(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    // Root, or a user namespace's owner in its own mount namespace (bind
    // mounts, tmpfs, proc, ... there; disks and FUSE stay root's).
    bool host = proc::capable();
    if (!host && !proc::mounts_capable())
        return -err::PERM;
    UserPath source(a0), target(a1), type(a2);
    if (target.error()) return target.error();
    char abs[512];
    join_path(proc::current()->cwd_path, target.c_str(), abs, sizeof(abs));
    { char c[512]; if (vfs::canonical_path(abs, "/", c, sizeof(c)) == 0) snprintf(abs, sizeof(abs), "%s", c); } // (/proc/self/fd/N paths: the real one)

    // Mount flags (containers): bind mounts, moves, remounts and propagation
    // changes. Read-only and propagation are accepted, not enforced: there is
    // one mount table (no mount namespaces yet).
    constexpr u64 MS_RDONLY = 1, MS_REMOUNT = 32, MS_BIND = 4096, MS_MOVE = 8192, MS_UNBINDABLE = 1 << 17,
                  MS_PRIVATE = 1 << 18, MS_SLAVE = 1 << 19, MS_SHARED = 1 << 20;
    u64 mflags = a3;
    (void)MS_RDONLY;
    if (mflags & MS_REMOUNT) {
        Inode* n;
        return vfs::resolve(abs, nullptr, true, &n);
    }
    if (mflags & MS_BIND) {
        if (source.error())
            return source.error();
        char src_abs[512];
        join_path(proc::current()->cwd_path, source.c_str(), src_abs, sizeof(src_abs));
    { char c[512]; if (vfs::canonical_path(src_abs, "/", c, sizeof(c)) == 0) snprintf(src_abs, sizeof(src_abs), "%s", c); } // (/proc/self/fd/N paths: the real one)
        Inode* src;
        if (long r = vfs::resolve(src_abs, nullptr, true, &src); r < 0)
            return r;
        // Directories come through a wrapper (vfs::bind_source): the mounts
        // below src show through it (MS_REC), and mounts made below the bind
        // stay there. (Recorded copies of the submounts, vfs::bind_replicate,
        // cover the same real directories and won over mounts made inside the
        // bind: bubblewrap's private /tmp landed on the system's.)
        return vfs::mount_at(abs, vfs::bind_source(src), src_abs, "bind");
    }
    if (mflags & MS_MOVE) {
        if (source.error())
            return source.error();
        char src_abs[512];
        join_path(proc::current()->cwd_path, source.c_str(), src_abs, sizeof(src_abs));
    { char c[512]; if (vfs::canonical_path(src_abs, "/", c, sizeof(c)) == 0) snprintf(src_abs, sizeof(src_abs), "%s", c); } // (/proc/self/fd/N paths: the real one)
        return vfs::move_mount(src_abs, abs);
    }
    if (mflags & (MS_UNBINDABLE | MS_PRIVATE | MS_SLAVE | MS_SHARED)) {
        Inode* n;
        return vfs::resolve(abs, nullptr, true, &n);
    }

    if (type.error()) return type.error(); // the flag-only operations above have no type

    // vfat/exfat options: uid=, gid=, umask=, fmask=, dmask= (octal masks).
    fat::MountOptions fat_opts;
    fat_opts.uid = proc::current()->cred.euid;
    fat_opts.gid = proc::current()->cred.egid;
    if (a4) {
        UserPath data(a4);
        if (!data.error()) {
            const char* o = data.c_str();
            while (*o) {
                auto num = [](const char* q, int base) {
                    u32 v = 0;
                    while ((*q >= '0' && *q <= '9') && (base == 10 || *q < '8'))
                        v = v * base + static_cast<u32>(*q++ - '0');
                    return v;
                };
                if (strncmp(o, "uid=", 4) == 0) fat_opts.uid = num(o + 4, 10);
                else if (strncmp(o, "gid=", 4) == 0) fat_opts.gid = num(o + 4, 10);
                else if (strncmp(o, "umask=", 6) == 0) fat_opts.fmask = fat_opts.dmask = num(o + 6, 8);
                else if (strncmp(o, "fmask=", 6) == 0) fat_opts.fmask = num(o + 6, 8);
                else if (strncmp(o, "dmask=", 6) == 0) fat_opts.dmask = num(o + 6, 8);
                while (*o && *o != ',')
                    o++;
                if (*o == ',')
                    o++;
            }
        }
    }

    const char* t = type.c_str();
    if (!host) {
        static const char* const ns_types[] = {"tmpfs", "ramfs", "proc", "sysfs", "devpts", "mqueue", "cgroup2"};
        bool ok = false;
        for (const char* n : ns_types)
            ok |= strcmp(t, n) == 0;
        if (!ok)
            return -err::PERM;
    }
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
        if (long r = exfat::mount(dev, &root, fat_opts); r < 0)
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
        if (long r = fat::mount(dev, &root, fat_opts); r < 0)
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
    // FUSE (fusermount3: "fuse.<subtype>", source = the server's name).
    if (strcmp(t, "fuse") == 0 || strncmp(t, "fuse.", 5) == 0 || strcmp(t, "fuseblk") == 0) {
        UserPath data(a4);
        if (!a4 || data.error())
            return -err::INVAL;
        Inode* root;
        char shown[80];
        if (long r = fuse::mount(data.c_str(), &root, shown, sizeof(shown)); r < 0)
            return r;
        if (long r = vfs::mount_at(abs, root, source.error() ? t : source.c_str(), t, shown); r < 0) {
            fuse::unmounted(root);
            return r;
        }
        return 0;
    }
    if (strcmp(t, "tmpfs") == 0 || strcmp(t, "ramfs") == 0) {
        auto* root = new ramfs::Dir;
        root->mode = S_IFDIR | 01777;
        return vfs::mount_at(abs, root, "tmpfs", "tmpfs");
    }
    if (strcmp(t, "binder") == 0) // binderfs: Android binder devices (kernel/linuxkpi/binder.cpp)
        return vfs::mount_at(abs, linuxkpi::binderfs_mount(), "binder", "binder");
    if (strcmp(t, "cgroup2") == 0 || strcmp(t, "cgroup") == 0)
        return vfs::mount_at(abs, cgroupfs::mount(), "cgroup2", "cgroup2");
    if (strcmp(t, "mqueue") == 0) { // POSIX message queues: an empty directory for now
        auto* root = new ramfs::Dir;
        root->mode = S_IFDIR | 01777;
        return vfs::mount_at(abs, root, "mqueue", "mqueue");
    }
    // The system's own instances, mounted again (inside containers).
    struct { const char* type; const char* path; } shared[] = {
        {"proc", "/proc"}, {"sysfs", "/sys"}, {"devtmpfs", "/dev"}, {"devpts", "/dev/pts"},
    };
    for (auto& s : shared) {
        if (strcmp(t, s.type) != 0)
            continue;
        Inode* root;
        if (long r = vfs::resolve_global(s.path, &root); r < 0)
            return r;
        return vfs::mount_at(abs, root, s.type, s.type);
    }
    return -err::NODEV; // unknown file system type
}

long sys_umount2(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (!proc::mounts_capable())
        return -err::PERM;
    UserPath target(a0);
    if (target.error())
        return target.error();
    char abs[512];
    join_path(proc::current()->cwd_path, target.c_str(), abs, sizeof(abs));
    { char c[512]; if (vfs::canonical_path(abs, "/", c, sizeof(c)) == 0) snprintf(abs, sizeof(abs), "%s", c); }
    block::sync_all();
    char source[64];
    vfs::mount_source(abs, source, sizeof(source));
    Inode* mounted = nullptr;
    vfs::resolve(abs, nullptr, true, &mounted);
    long r = vfs::unmount_at(target.c_str(), proc::current()->cwd, abs);
    if (r == 0) {
        loop::unmounted(source); // mount -o loop's device goes with it
        fuse::unmounted(mounted); // a FUSE server's loop ends
    }
    return r;
}

// chroot(path): the caller's "/" (and its children's) becomes `path`.
long sys_chroot(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (!proc::ns_capable()) // root, or capabilities in its user namespace (sandboxes)
        return -err::PERM;
    UserPath path(a0);
    if (path.error())
        return path.error();
    Process* p = proc::current();
    char abs[512];
    join_path(p->cwd_path, path.c_str(), abs, sizeof(abs));
    Inode* dir;
    if (long r = vfs::resolve(abs, nullptr, true, &dir); r < 0)
        return r;
    if (!vfs::is_dir(dir->mode))
        return -err::NOTDIR;
    // The working directory keeps its place; its path is now seen from the new root.
    usize n = strlen(abs);
    if (strcmp(abs, "/") != 0 && strncmp(p->cwd_path, abs, n) == 0 && (p->cwd_path[n] == '/' || !p->cwd_path[n])) {
        char rest[512];
        snprintf(rest, sizeof(rest), "%s", p->cwd_path[n] ? p->cwd_path + n : "/");
        snprintf(p->cwd_path, sizeof(p->cwd_path), "%s", rest);
    }
    p->root = dir;
    // Shared file system context (clone(CLONE_FS)): the parent's root moves too.
    if (p->fs_shared_with) {
        LockGuard guard(proc::table_lock());
        if (Process* peer = proc::find(p->fs_shared_with); peer && peer->state == proc::State::Running)
            peer->root = dir;
    }
    return 0;
}

// pivot_root(new_root, put_old): the caller's "/" becomes new_root; the old
// root stays reachable at put_old (under the new root) until it's unmounted.
// Only the caller and its children move (as after unshare(CLONE_NEWNS)): the
// rest of the system keeps its root.
long sys_pivot_root(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (!proc::mounts_capable())
        return -err::PERM;
    UserPath new_root(a0), put_old(a1);
    if (new_root.error())
        return new_root.error();
    if (put_old.error())
        return put_old.error();
    Process* p = proc::current();
    char nr_abs[512], po_abs[512];
    join_path(p->cwd_path, new_root.c_str(), nr_abs, sizeof(nr_abs));
    join_path(p->cwd_path, put_old.c_str(), po_abs, sizeof(po_abs));
    Inode *nr, *po;
    if (long r = vfs::resolve(nr_abs, nullptr, true, &nr); r < 0)
        return r;
    if (long r = vfs::resolve(po_abs, nullptr, true, &po); r < 0)
        return r;
    if (!vfs::is_dir(nr->mode) || !vfs::is_dir(po->mode))
        return -err::NOTDIR;
    Inode* old_root = p->root ? p->root : vfs::root();
    if (nr == old_root)
        return -err::BUSY;
    // put_old as seen from the new root.
    usize n = strlen(nr_abs);
    char po_new[512];
    if (strncmp(po_abs, nr_abs, n) == 0 && (po_abs[n] == '/' || !po_abs[n] || n == 1))
        snprintf(po_new, sizeof(po_new), "%s", n == 1 ? po_abs : (po_abs[n] ? po_abs + n : "/"));
    else
        return -err::INVAL; // put_old must be under new_root
    p->root = nr;
    if (p->mntns)
        vfs::pivot_mounts(nr_abs, po_new);
    if (po != nr) // pivot_root(".", ".") stacks the old root on the new one; nothing to show then
        if (long r = vfs::mount_at(po_new, old_root, "rootfs", "rootfs"); r < 0) {
            p->root = old_root == vfs::root() ? nullptr : old_root;
            return r;
        }
    if (p->cwd == nr)
        snprintf(p->cwd_path, sizeof(p->cwd_path), "/");
    return 0;
}

// Ownership: accepted (single root user for now).
long sys_ok(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; }

} // namespace

// flock: advisory whole-file locks. Accepted for open descriptors (single
// writers in practice: Xorg, Mesa's shader cache, dpkg lock their files).
long sys_flock(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u32 LOCK_SH = 1, LOCK_EX = 2, LOCK_UN = 8, LOCK_NB = 4;
    FileRef f(proc::current(), static_cast<int>(a0));
    if (!f)
        return -err::BADF;
    u32 op = static_cast<u32>(a1) & ~LOCK_NB;
    if (op != LOCK_SH && op != LOCK_EX && op != LOCK_UN)
        return -err::INVAL;
    return 0;
}

// kcmp: whether two processes share a resource (Mesa/Xorg ask whether two
// descriptors are the same open file). 0 = same, 1/2 = ordered different.
long sys_kcmp(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr int KCMP_FILE = 0, KCMP_VM = 1, KCMP_EPOLL_TFD = 7;
    int type = static_cast<int>(a2);
    if (type < KCMP_FILE || type >= KCMP_EPOLL_TFD)
        return -err::INVAL;
    LockGuard guard(proc::table_lock());
    Process* p1 = proc::find_visible(static_cast<int>(a0), proc::current());
    Process* p2 = proc::find_visible(static_cast<int>(a1), proc::current());
    if (!p1 || !p2)
        return -err::SRCH;
    Process* self = proc::current();
    if (self->cred.euid != 0 && (p1->cred.ruid != self->cred.ruid || p2->cred.ruid != self->cred.ruid))
        return -err::PERM;
    const void* x;
    const void* y;
    if (type == KCMP_FILE) {
        u64 i1 = a3, i2 = a4;
        if (i1 >= proc::MAX_FDS || i2 >= proc::MAX_FDS || !p1->fds[i1] || !p2->fds[i2])
            return -err::BADF;
        x = p1->fds[i1];
        y = p2->fds[i2];
    } else if (type == KCMP_VM) {
        x = p1->mem;
        y = p2->mem;
    } else { // files, fs, sighand, io, sysvsem: shared exactly when it's one process
        x = p1;
        y = p2;
    }
    return x == y ? 0 : reinterpret_cast<uptr>(x) < reinterpret_cast<uptr>(y) ? 1 : 2;
}

long socket_vectored(u64 fd, const KIoVec* iov, u64 count, bool write, bool* is_socket) {
    *is_socket = false;
    FileRef f(proc::current(), static_cast<long>(fd));
    if (!f || (f->inode->mode & S_IFMT) != S_IFSOCK)
        return 0;
    *is_socket = true;
    return vectored_socket(f.get(), iov, count, write);
}

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
    set(73, sys_flock, "flock");
    set(312, sys_kcmp, "kcmp");
    set(74, sys_sync, "fsync");
    set(75, sys_sync, "fdatasync");
    set(165, sys_mount, "mount");
    set(166, sys_umount2, "umount2");
    set(155, sys_pivot_root, "pivot_root");
    set(161, sys_chroot, "chroot");
    set(306, sys_sync, "syncfs");
    set(277, sys_sync_file_range, "sync_file_range");
    set(76, sys_truncate, "truncate");
    set(77, sys_ftruncate, "ftruncate");
    set(79, sys_getcwd, "getcwd");
    set(80, sys_chdir, "chdir");
    set(81, sys_fchdir, "fchdir");
    set(303, sys_name_to_handle_at, "name_to_handle_at");
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
    set(187, sys_readahead, "readahead");
    set(285, sys_fallocate, "fallocate");
    set(88, sys_symlink, "symlink");
    set(265, sys_linkat, "linkat");
    set(89, sys_readlink, "readlink");
    set(7, sys_poll, "poll");
    set(23, sys_select, "select");
    set(40, sys_sendfile, "sendfile");
    set(326, sys_copy_file_range, "copy_file_range");
    set(437, sys_openat2, "openat2");
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
    set(332, sys_statx, "statx");
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
