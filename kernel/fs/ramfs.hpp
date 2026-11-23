#pragma once

#include <fs/vfs.hpp>
#include <sched/sync.hpp>

// In-memory filesystem: the root filesystem, filled from the initramfs.
namespace ramfs {

class Dir : public vfs::Inode {
public:
    Dir();
    vfs::Inode* lookup(const char* name, usize len) override;
    long create(const char* name, usize len, u32 mode, const char* target, vfs::Inode** out) override;
    long unlink(const char* name, usize len, bool remove_dir) override;
    long readdir(u64 index, vfs::DirEntry* out) override;
    long rename(const char* old_name, usize old_len, vfs::Inode* new_dir, const char* new_name,
                usize new_len) override;
    isize read(u64, void*, usize) override { return -err::ISDIR; }
    isize write(u64, const void*, usize) override { return -err::ISDIR; }

    bool is_ramfs_dir() const override { return true; }

    // Attach an existing inode under `name`.
    long attach(const char* name, usize len, vfs::Inode* node);
    usize child_count() const { return m_count; }
    // Swap the inode an existing entry points to (mount/umount); returns the old one.
    vfs::Inode* replace(const char* name, usize len, vfs::Inode* node);
    long link(const char* name, usize len, vfs::Inode* target) override {
        if (!target->shared_mappable()) // RAM files only (not device or proc nodes)
            return -err::XDEV;
        long r = attach(name, len, target);
        if (r == 0) {
            target->nlink++;
            target->ctime = vfs::now();
        }
        return r;
    }

private:
    struct Child {
        char* name;
        vfs::Inode* inode;
        Child* next;
    };
    Child* find(const char* name, usize len);
    Child* m_children = nullptr;
    usize m_count = 0;
    Spinlock m_lock;
};

// A file in RAM, stored as an array of physical pages. Because its pages
// are real pages it can be mapped MAP_SHARED by many processes at once: the
// backing store for /tmp, /dev/shm (POSIX shm), memfd_create, System V
// shared memory and anonymous shared mappings, like Linux's tmpfs.
//
// Lifetime: the inode lives while a directory entry, an open file or a
// mapping refers to it, and frees itself (and its pages) after the last.
class RegularFile : public vfs::Inode {
public:
    RegularFile();
    ~RegularFile() override;
    isize read(u64 offset, void* buf, usize count) override;
    isize write(u64 offset, const void* buf, usize count) override;
    long truncate(u64 new_size) override;
    long punch_hole(u64 offset, u64 len) override;
    long private_page(u64 offset, u64* phys) override;
    long mmap_phys(u64 offset, u64* phys) override;
    bool shared_mappable() const override { return true; }
    void mmap_ref(int delta) override;
    long seals(bool add, u32 bits) override;
    void on_open(vfs::File* f) override;
    void on_release(vfs::File* f) override;
    void unlinked() override { maybe_free(); }

    // memfd_create: allow seals (F_ADD_SEALS); otherwise only F_SEAL_SEAL is set.
    void allow_sealing() { m_seals = 0; }
    u32 seal_bits() const { return m_seals; }
    u32 mapping_count() const { return __atomic_load_n(&m_maps, __ATOMIC_ACQUIRE); }

    static constexpr u32 F_SEAL_SEAL = 1, F_SEAL_SHRINK = 2, F_SEAL_GROW = 4, F_SEAL_WRITE = 8,
                         F_SEAL_FUTURE_WRITE = 0x10;

private:
    u64 page(u64 index, bool allocate); // physical address of page `index` (0 = hole)
    void free_pages_from(u64 first);
    void maybe_free();

    u64* m_pages = nullptr;   // physical page addresses, 0 = not allocated (reads as zeros)
    u64 m_slots = 0;          // entries in m_pages
    IrqSpinlock m_pages_lock; // m_pages/m_slots (taken by the page-fault path)
    sched::Mutex m_io;        // serializes read/write/truncate; pages aren't freed under a copy
    u32 m_opens = 0, m_maps = 0;
    u32 m_seals = F_SEAL_SEAL;
};

class Symlink : public vfs::Inode {
public:
    explicit Symlink(const char* target);
    ~Symlink() override;
    isize readlink(char* buf, usize size) override;

private:
    char* m_target;
};

} // namespace ramfs
