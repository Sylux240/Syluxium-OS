#pragma once

#include <arch/x86_64/interrupts.hpp>
#include <lib/errno.hpp>
#include <lib/types.hpp>

// Linux x86_64 system call interface.
//
// Syluxium implements the Linux syscall ABI directly (numbers, argument
// order, struct layouts, errno values), so unmodified Linux programs run.
// Arguments arrive in rdi, rsi, rdx, r10, r8, r9; the result (or -errno)
// goes back in rax.
namespace syscall {

using Handler = long (*)(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, interrupts::Frame* frame);

void init_cpu();          // program the SYSCALL MSRs on this CPU
void register_all();      // fill the dispatch table

void set(u32 number, Handler handler, const char* name);
Handler native(u32 number); // the registered x86_64 handler, or null

// Handler groups (one per source file).
void register_file();
void register_memory();
void register_process();
void register_time();
void register_net();
void register_events();
void register_ipc();
void register_inotify();
void register_compat(); // 32-bit (ia32) table + int $0x80 gate (compat32.cpp)

// readv/writev on a socket as one message (iovecs already in kernel memory,
// pointing at user buffers). *is_socket false: fd isn't one, nothing done.
struct KIoVec {
    u64 base, len;
};
long socket_vectored(u64 fd, const KIoVec* iov, u64 count, bool write, bool* is_socket);
void xattrs_forget(u64 dev, u64 ino); // (vfs::forget_xattrs)

} // namespace syscall

// Shorthand for handler definitions.
#define SYSCALL_ARGS u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, interrupts::Frame* frame
#define SYSCALL_UNUSED (void)a0, (void)a1, (void)a2, (void)a3, (void)a4, (void)a5, (void)frame
