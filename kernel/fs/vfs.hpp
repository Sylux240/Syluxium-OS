#pragma once

#include <lib/errno.hpp>
#include <lib/spinlock.hpp>
#include <sched/sync.hpp>
#include <lib/types.hpp>

// Virtual File System: the filesystem-independent layer. Every file,
// directory, device and symlink is an Inode; filesystems (ramfs today,
// ext4/FAT/procfs later) provide Inode subclasses.
namespace vfs {

// File type bits of `mode` (same values as Linux's st_mode).
constexpr u32 S_IFMT   = 0170000;
constexpr u32 S_IFSOCK = 0140000;
constexpr u32 S_IFLNK  = 0120000;
constexpr u32 S_IFREG  = 0100000;
constexpr u32 S_IFBLK  = 0060000;
constexpr u32 S_IFDIR  = 0040000;
constexpr u32 S_IFCHR  = 0020000;
constexpr u32 S_IFIFO  = 0010000;

inline bool is_dir(u32 mode) { return (mode & S_IFMT) == S_IFDIR; }
inline bool is_reg(u32 mode) { return (mode & S_IFMT) == S_IFREG; }
inline bool is_lnk(u32 mode) { return (mode & S_IFMT) == S_IFLNK; }

struct Timespec {
    i64 sec = 0;
    i64 nsec = 0;
};

// Linux d_type values for getdents64.
constexpr u8 DT_UNKNOWN = 0, DT_FIFO = 1, DT_CHR = 2, DT_DIR = 4, DT_BLK = 6, DT_REG = 8, DT_LNK = 10;

struct DirEntry {
    u64 ino;
    u8 type;
    char name[256];
};

struct File;

Timespec now();

class Inode {
public:
    virtual ~Inode() = default;

    u64 ino = 0;
    u32 mode = 0;
    u32 nlink = 1;
    u8 anon_kind = 0; // anonymous-inode files (eventfd, epoll, memfd...): which kind, 0 = none
    u32 uid = 0;
    u32 gid = 0;
    u64 size = 0;
    u64 rdev = 0;  // device number for device nodes
    u64 dev = 1;   // which file system this inode lives on (st_dev); 1 = the RAM root
    Timespec atime, mtime, ctime;
    Inode* parent = nullptr; // directories: for ".."

    // ---- Data (regular files, devices) ----
    virtual isize read(u64 offset, void* buf, usize count) { return -err::INVAL; }
    virtual isize write(u64 offset, const void* buf, usize count) { return -err::INVAL; }
    virtual long truncate(u64 new_size) { return -err::INVAL; }
    virtual long ioctl(u64 request, u64 arg) { return -err::NOTTY; }
    // ioctl through an open file (sys_ioctl calls this). The default checks
    // the argument buffer (size from the ioctl number) and calls ioctl().
    // Devices that need the file (per-open state, O_NONBLOCK) or take plain
    // integers as arguments override it and access user memory with uaccess.
    virtual long ioctl_file(File* f, u64 request, u64 arg);
    // mmap(MAP_SHARED) of a device: the physical address of the page at file
    // offset `offset` (page aligned). The memory must stay valid for as long
    // as the inode exists; it's mapped as is (no copy-on-write, never freed
    // by the address space). -ENODEV: this inode can't be mapped.
    virtual long mmap_phys(u64 offset, u64* phys) { return -err::NODEV; }
    // Regular files whose pages can be mapped MAP_SHARED (ramfs/tmpfs): the
    // mapping uses mmap_phys() and writes are seen by every process.
    virtual bool shared_mappable() const { return false; }
    // A mapping (VMA) referring to this inode was created (+1) or removed (-1):
    // keeps unlinked-but-mapped files alive.
    virtual void mmap_ref(int delta) {}
    // memfd seals (fcntl F_ADD_SEALS / F_GET_SEALS): returns the seal set or -errno.
    virtual long seals(bool add, u32 bits) { return -err::INVAL; }
    // The last directory entry naming this inode was removed.
    virtual void unlinked() {}
    // Change access/modification times (utimensat). Disk file systems persist them.
    virtual long set_times(Timespec access, Timespec modify) {
        atime = access;
        mtime = modify;
        ctime = vfs::now();
        return 0;
    }
    virtual bool seekable() const { return true; }

    // Reads/writes through an open file. Most inodes only care about the
    // offset; pipes and sockets also need the file (non-blocking mode, ends).
    virtual isize read_file(File* f, u64 offset, void* buf, usize count) { return read(offset, buf, count); }
    virtual isize write_file(File* f, u64 offset, const void* buf, usize count) {
        return write(offset, buf, count);
    }
    // Readiness for poll/select (POLLIN/POLLOUT/POLLHUP/POLLERR bits).
    // Plain files and most devices never block, so they're always ready.
    virtual u16 poll(File* f) { return 0x001 | 0x004; }

    // An open file description referring to this inode was created / closed for good.
    virtual void on_open(File* f) {}
    virtual void on_release(File* f) {}

    // ---- Directories ----
    virtual Inode* lookup(const char* name, usize len) { return nullptr; }
    // Create a file, directory or symlink (type from `mode`). `target` is
    // the link text for symlinks.
    virtual long create(const char* name, usize len, u32 new_mode, const char* target, Inode** out) {
        return -err::NOTDIR;
    }
    virtual long unlink(const char* name, usize len, bool remove_dir) { return -err::NOTDIR; }
    // Fill entry #index (".", ".." included). Returns 1, or 0 at the end.
    virtual long readdir(u64 index, DirEntry* out) { return -err::NOTDIR; }
    // Move entry `old_name` of this directory to `new_name` in `new_dir`
    // (same file system), replacing an existing target like POSIX rename.
    virtual long rename(const char* old_name, usize old_len, Inode* new_dir, const char* new_name, usize new_len) {
        return -err::PERM;
    }

    // File system usage for statfs/df (filled in for the root of a mount).
    struct FsStats {
        u64 type;          // Linux f_type magic
        u64 block_size;
        u64 blocks, blocks_free;
        u64 files, files_free;
    };
    virtual long statfs(FsStats* out);

    // True for directories of the RAM file system (rename between them is possible).
    virtual bool is_ramfs_dir() const { return false; }

    // ---- Symlinks ----
    virtual isize readlink(char* buf, usize bufsize) { return -err::INVAL; }
};

// An open file description (shared by dup'ed and fork'ed descriptors).
struct File {
    Inode* inode = nullptr;
    u64 offset = 0;
    u32 flags = 0;    // O_* access mode and status flags
    u32 refcount = 1;
    sched::Mutex lock; // protects offset; a mutex because reads may sleep (terminals, pipes)
    char* path = nullptr; // absolute path it was opened by (for /proc/<pid>/fd), heap-allocated
    void* private_data = nullptr; // per-open state owned by the inode (input clients, ...)
};

// Remember the path a file was opened by.
void file_set_path(File* f, const char* path);

// Linux open() flags.
constexpr u32 O_ACCMODE = 3, O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2;
constexpr u32 O_CREAT = 0100, O_EXCL = 0200, O_NOCTTY = 0400, O_TRUNC = 01000, O_APPEND = 02000,
              O_NONBLOCK = 04000, O_DIRECTORY = 0200000, O_NOFOLLOW = 0400000,
              O_CLOEXEC = 02000000, O_PATH = 010000000;

void init();                 // create the root ramfs and /dev
Inode* root();

// Path resolution relative to `cwd` (absolute paths ignore it).
// follow_last: follow a symlink in the final component.
long resolve(const char* path, Inode* cwd, bool follow_last, Inode** out);

// Resolve everything but the last component; returns the parent directory
// and points `name`/`len` at the final component inside `path`.
long resolve_parent(const char* path, Inode* cwd, Inode** parent, const char** name, usize* len);

// Convenience helpers for kernel code.
long mkdir_p(const char* path, u32 mode = 0755);
long create_file(const char* path, const void* data, usize size, u32 mode);
long add_node(const char* path, Inode* node); // place a ready-made inode (devices)
long remove_node(const char* path);            // unplugged device: drop its /dev entry (open files keep the inode)

File* open_inode(Inode* inode, u32 flags);
void file_get(File* f);
void file_put(File* f);
// Called just before a file's last reference goes away (epoll uses it to
// drop the file from every set watching it). Set once at boot.
extern void (*file_release_hook)(File* f);

u64 next_ino();
Timespec now();
u64 next_fs_dev(); // a fresh st_dev for a newly mounted file system

// ---- Mounts ----
// Attach a file system's root over the directory at `path` (which must live
// in the RAM root file system). source/type are what /proc/mounts shows.
long mount_at(const char* path, Inode* fs_root, const char* source, const char* type);
long unmount(const char* path);
void for_each_mount(void (*fn)(const char* source, const char* path, const char* type, void* ctx), void* ctx);

} // namespace vfs
