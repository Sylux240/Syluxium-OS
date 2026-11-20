// Memory management system calls.

#include <fs/ramfs.hpp>
#include <mm/layout.hpp>
#include <proc/process.hpp>
#include <syscall/syscall.hpp>

namespace syscall {

namespace {

using namespace proc;

long sys_mmap(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    uptr addr = a0;
    usize len = a1;
    u32 prot = static_cast<u32>(a2);
    u32 flags = static_cast<u32>(a3);
    int fd = static_cast<int>(a4);
    u64 offset = a5;

    if (len == 0 || !(flags & (MAP_PRIVATE | MAP_SHARED)))
        return -err::INVAL;
    Process* p = current();

    vfs::Inode* file = nullptr;
    vfs::File* f = nullptr;
    bool prepared = false; // mmap_prepare's reference on the device mapping, dropped below
    if (!(flags & MAP_ANONYMOUS)) {
        f = fd_get(p, fd);
        if (!f)
            return -err::BADF;
        if (!vfs::is_reg(f->inode->mode)) {
            // Device memory (sound buffers; framebuffers and GPUs later): the
            // driver must support it, and the mapping is always shared.
            // Check the first and last page now so mmap() fails cleanly.
            u64 phys;
            long r = !(flags & MAP_SHARED) ? -err::INVAL : f->inode->mmap_prepare(f, offset, len, prot & PROT_WRITE, &offset);
            prepared = r == 0;
            u64 last = offset + align_up<u64>(len, 4096) - 4096;
            if (r == 0)
                r = f->inode->mmap_phys(offset, &phys);
            if (r == 0)
                r = f->inode->mmap_phys(last, &phys);
            if (r < 0) {
                if (prepared)
                    f->inode->mmap_range_ref(offset, -1);
                vfs::file_put(f);
                return r;
            }
        }
        file = f->inode;
    }
    // A sealed memfd can't get new writable shared mappings.
    if (file && (flags & MAP_SHARED) && (prot & PROT_WRITE) && file->shared_mappable()) {
        long seals = file->seals(false, 0);
        if (seals > 0 && (seals & (ramfs::RegularFile::F_SEAL_WRITE | ramfs::RegularFile::F_SEAL_FUTURE_WRITE))) {
            vfs::file_put(f);
            return -err::PERM;
        }
    }
    // Anonymous shared memory (shared with fork()ed children): backed by a
    // hidden tmpfs file, so it uses the same shared-page machinery.
    if ((flags & MAP_ANONYMOUS) && (flags & MAP_SHARED)) {
        auto* anon = new ramfs::RegularFile;
        anon->nlink = 0; // no directory entry: lives exactly as long as its mappings
        anon->truncate(align_up<u64>(len, 4096));
        file = anon;
        flags &= ~MAP_ANONYMOUS;
        offset = 0;
        uptr result;
        long r = p->mem->map(addr, len, prot, flags, file, offset, ~0ull, &result);
        if (r < 0)
            anon->mmap_ref(0); // never mapped: nobody refers to it (frees it)
        return r < 0 ? r : static_cast<long>(result);
    }
    // MAP_SHARED of ramfs/tmpfs/memfd files shares pages between processes;
    // MAP_SHARED of disk files (ext2, FAT) is still a private copy.
    uptr result;
    long r = p->mem->map(addr, len, prot, flags, file, offset, ~0ull, &result);
    if (prepared)
        file->mmap_range_ref(offset, -1); // the mapping holds its own reference now (or failed)
    if (f)
        vfs::file_put(f); // the mapping keeps its own reference (Inode::mmap_ref)
    return r < 0 ? r : static_cast<long>(result);
}

long sys_munmap(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return current()->mem->unmap(a0, a1);
}

long sys_mprotect(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return current()->mem->protect(a0, a1, static_cast<u32>(a2));
}

long sys_brk(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return current()->mem->brk(a0);
}

long sys_msync(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a0 & 0xFFF)
        return -err::INVAL;
    return 0; // shared mappings are the file's own pages (tmpfs) or device memory: nothing to write back
}

long sys_madvise(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return 0; // advice only
}

} // namespace

void register_memory() {
    set(9, sys_mmap, "mmap");
    set(10, sys_mprotect, "mprotect");
    set(11, sys_munmap, "munmap");
    set(12, sys_brk, "brk");
    set(26, sys_msync, "msync");
    set(28, sys_madvise, "madvise");
}

} // namespace syscall
