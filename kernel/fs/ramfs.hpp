#pragma once

#include <fs/vfs.hpp>

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

class RegularFile : public vfs::Inode {
public:
    RegularFile();
    ~RegularFile() override;
    isize read(u64 offset, void* buf, usize count) override;
    isize write(u64 offset, const void* buf, usize count) override;
    long truncate(u64 new_size) override;

private:
    long reserve(u64 capacity);
    u8* m_data = nullptr;
    u64 m_capacity = 0;
    Spinlock m_lock;
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
