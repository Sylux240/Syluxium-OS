// Memory management system calls.

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
    if (!(flags & MAP_ANONYMOUS)) {
        f = fd_get(p, fd);
        if (!f)
            return -err::BADF;
        if (!vfs::is_reg(f->inode->mode)) {
            // Device memory (sound buffers; framebuffers and GPUs later): the
            // driver must support it, and the mapping is always shared.
            // Check the first and last page now so mmap() fails cleanly.
            u64 phys;
            u64 last = offset + align_up<u64>(len, 4096) - 4096;
            long r = !(flags & MAP_SHARED) ? -err::INVAL : f->inode->mmap_phys(offset, &phys);
            if (r == 0)
                r = f->inode->mmap_phys(last, &phys);
            if (r < 0) {
                vfs::file_put(f);
                return r;
            }
        }
        file = f->inode;
    }
    // MAP_SHARED file mappings are treated as private copies for now; with
    // a single process there's nobody to share writes with yet.
    uptr result;
    long r = p->mem->map(addr, len, prot, flags, file, offset, ~0ull, &result);
    if (f)
        vfs::file_put(f); // TODO: keep the inode alive for the mapping's lifetime
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
    set(28, sys_madvise, "madvise");
}

} // namespace syscall
