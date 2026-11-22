#pragma once

#include <fs/vfs.hpp>

// eBPF objects without execution: the bpf() system call's maps (hash,
// array, per-CPU and LRU variants, LPM tries), programs (accepted and kept,
// never verified or run), cgroup attachments (recorded, no effect), BTF
// blobs and links, object IDs, and pinning in the BPF file system (bpffs,
// mounted at /sys/fs/bpf).
//
// What it's for: Android's netd and bpfloader (Waydroid) need the maps and
// pins to start; they keep traffic statistics and firewall rules in maps
// that their cgroup programs would update. Here the programs never run, so
// maps change only through bpf() from user space: per-app traffic counters
// stay at zero and the BPF firewall filters nothing.
namespace bpf {

void init();                 // /sys/fs/bpf (the mount point)
vfs::Inode* fs_mount();      // a new bpffs root, for mount -t bpf
void register_syscalls();    // bpf (321)
long sys_bpf_call(int cmd, u64 uattr, u32 size); // (also the i386 entry)

} // namespace bpf
