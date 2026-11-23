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
constexpr u32 S_ISUID  = 04000, S_ISGID = 02000, S_ISVTX = 01000; // setuid, setgid, sticky

// Groups that own device nodes (Debian's numbers; "input" is Syluxium's fixed
// choice, matched by /etc/group). Desktop users are members.
constexpr u32 GID_TTY = 5, GID_DISK = 6, GID_AUDIO = 29, GID_VIDEO = 44, GID_INPUT = 105;
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
    // fallocate(FALLOC_FL_PUNCH_HOLE): the range reads as zeros and its memory is freed.
    virtual long punch_hole(u64 offset, u64 len) { return -err::OPNOTSUPP; }
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
    // Before a device mapping is made: drivers that set up per-mapping state
    // (LinuxKPI: f_op->mmap) do it here and may replace the file offset
    // the mapping (and so mmap_phys) will use.
    virtual long mmap_prepare(File* f, u64 offset, u64 len, bool writable, u64* new_offset) {
        *new_offset = offset;
        return 0;
    }
    // Drivers that need the mapping's user address when they set it up
    // (binder hands out pointers into its buffer): mmap() picks the address
    // first and calls mmap_prepare_at() with it.
    virtual bool mmap_wants_address(File* f) { return false; }
    virtual long mmap_prepare_at(File* f, u64 offset, u64 len, bool writable, uptr addr, u64* new_offset) {
        return mmap_prepare(f, offset, len, writable, new_offset);
    }
    // Regular files whose pages can be mapped MAP_SHARED (ramfs/tmpfs): the
    // mapping uses mmap_phys() and writes are seen by every process.
    virtual bool shared_mappable() const { return false; }
    // LinuxKPI: the Linux struct file behind this open file (bridged files), else null.
    virtual void* linux_file(File* f) { return nullptr; }
    // Read-only page cache for private mappings (programs, libraries on
    // disk): the page holding file offset `offset` (page aligned), shared by
    // every process that maps it; the caller takes its own reference. Writes
    // to the file drop the cache (new mappings see new data).
    virtual long cache_page(u64 offset, u64* phys) { return -err::NODEV; }
    // A mapping (VMA) referring to this inode was created (+1) or removed (-1):
    // keeps unlinked-but-mapped files alive.
    virtual void mmap_ref(int delta) {}
    // Device mappings set up by mmap_prepare(): a mapping using file offset
    // `offset` (the start of what mmap_prepare returned, or a later part of
    // it) was created (+1) or removed (-1). mmap_prepare's own reference is
    // dropped by mmap() once the mapping exists. The last one lets the
    // driver tear its per-mapping state down (LinuxKPI: vm_ops->close).
    virtual void mmap_range_ref(u64 offset, int delta) {}
    // Cache attributes for the device page at `offset`: vmm::CACHE_WC / CACHE_UC or 0.
    virtual u32 mmap_cache_flags(u64 offset) { return 0; }
    // memfd seals (fcntl F_ADD_SEALS / F_GET_SEALS): returns the seal set or -errno.
    virtual long seals(bool add, u32 bits) { return -err::INVAL; }
    // The last directory entry naming this inode was removed.
    virtual void unlinked() {}
    // open() of this node opens another inode instead (/dev/tty -> the
    // caller's controlling terminal).
    virtual Inode* open_redirect() { return this; }
    // Add a hard link `name` -> `target` in this directory (same file system).
    virtual long link(const char* name, usize len, Inode* target) { return -err::XDEV; }
    // Change mode (permission bits, type unchanged) and ownership (chmod,
    // chown, new files). Disk file systems persist them.
    virtual long set_owner(u32 new_mode, u32 new_uid, u32 new_gid) {
        mode = (mode & S_IFMT) | (new_mode & 07777);
        uid = new_uid;
        gid = new_gid;
        ctime = vfs::now();
        return 0;
    }
    // Change access/modification times (utimensat). Disk file systems persist them.
    virtual long set_times(Timespec access, Timespec modify) {
        atime = access;
        mtime = modify;
        ctime = vfs::now();
        return 0;
    }
    virtual bool seekable() const { return true; }
    // Before stat/statx report the fields above: file systems whose
    // attributes live elsewhere (FUSE servers) bring them up to date, or
    // fail (ENOTCONN when the server is gone).
    virtual long revalidate() { return 0; }
    // Message devices (/dev/fuse): a writev() must arrive as one write
    // (libfuse sends each reply's header and data as separate iovecs).
    virtual bool whole_writes() const { return false; }

    // Reads/writes through an open file. Most inodes only care about the
    // offset; pipes and sockets also need the file (non-blocking mode, ends).
    virtual isize read_file(File* f, u64 offset, void* buf, usize count) { return read(offset, buf, count); }
    virtual isize write_file(File* f, u64 offset, const void* buf, usize count) {
        return write(offset, buf, count);
    }
    // Readiness for poll/select (POLLIN/POLLOUT/POLLHUP/POLLERR bits).
    // Plain files and most devices never block, so they're always ready.
    virtual u16 poll(File* f) { return 0x001 | 0x004; }
    // Regular files epoll may watch anyway (procfs: Android's init watches
    // /proc/self/mountinfo); other regular files and directories it refuses.
    virtual bool epoll_regular() const { return false; }
    // Edge-triggered epoll (EPOLLET): a counter that grows with every new
    // event on this file, so a waiter is told again only about new events
    // here. 0 = not tracked (epoll then falls back to the system-wide poll
    // sequence, which re-reports after any event anywhere).
    virtual u64 poll_generation() const { return 0; }
    // A "magic" symlink (/proc/<pid>/ns/*): readlink shows its text, but lookups
    // and open() take the link inode itself instead of following it.
    virtual bool magic_link() const { return false; }
    // A symlink that leads to an inode rather than a path (/proc/<pid>/exe,
    // /proc/<pid>/fd/N): following it jumps there, as Linux's nd_jump_link
    // does, so a deleted file or a memfd can still be reopened or executed.
    // Null: follow the text readlink gives.
    virtual Inode* jump_target() { return nullptr; }

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
// realpath(): absolute, without "."/"..", every symlink resolved.
long canonical_path(const char* path, const char* base, char* out, usize size);
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
// Attach a file system's root over the directory at `path` (on any file
// system). source/type are what /proc/mounts shows.
long mount_at(const char* path, Inode* fs_root, const char* source, const char* type,
              const char* options = nullptr); // options: what /proc/mounts lists (default "rw,relatime")
long unmount(const char* path);
u64 mount_generation(); // grows with every mount, move and unmount
long unmount_at(const char* path, Inode* cwd, const char* canon); // umount2: relative to cwd
void mount_source(const char* path, char* out, usize size); // what /proc/mounts shows as its device
long move_mount(const char* from, const char* to); // MS_MOVE
void bind_replicate(const char* src, const char* dst); // MS_BIND | MS_REC: copies of the mounts below src
// chroot/pivot_root: the process layer says where the caller's "/" is.
void set_process_root_hook(Inode* (*fn)());
// Resolve from the system's "/" whatever the caller's root (proc, sys, dev
// for mounts inside containers).
long resolve_global(const char* path, Inode** out);
// Mount namespaces: opaque tables (nullptr = the system's).
void set_process_mounts_hook(void* (*fn)());
void* mount_ns_copy(void* from);
void mount_ns_get(void* ns);
void mount_ns_put(void* ns);
bool is_mountpoint(Inode* dir); // "/", a mount's root, or a directory something is mounted on
// Make `new_root` the "/" directory (booting from a disk). Mount the RAM
// file system's /dev, /proc, ... on it afterwards (see ram_root()).
void set_root(Inode* new_root, const char* source, const char* type);
Inode* ram_root(); // the RAM file system created at boot (the root until set_root)
// Permission check used while walking paths (directories need search/x).
void set_permission_hook(long (*fn)(Inode*, int));
void for_each_mount(void (*fn)(const char* source, const char* path, const char* type, const char* options,
                               void* ctx),
                    void* ctx);

} // namespace vfs
