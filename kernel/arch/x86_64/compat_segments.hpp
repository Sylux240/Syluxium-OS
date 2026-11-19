#pragma once

#include <lib/types.hpp>

struct Cpu;
namespace sched { struct Thread; }

// User segment registers for 32-bit (ia32) programs.
//
// 64-bit programs find their thread-local storage through the FS base MSR
// and never care about selector values. 32-bit glibc instead asks the kernel
// for a GDT descriptor (set_thread_area) and loads its selector into %gs, so
// for those threads the selectors and the descriptors behind them are part of
// the thread's state: saved when it's switched out, reloaded when it's
// switched back in (on whichever CPU that is). Same scheme as Linux.
namespace compat_segments {

// Scheduler, interrupts off: remember the user's FS/GS selectors.
void save(sched::Thread* t);

// Scheduler, interrupts off, before the FS/GS base MSRs are written:
//  - compat threads: install the thread's TLS descriptors into this CPU's
//    GDT, load DS/ES with the flat user data segment (compatibility mode,
//    unlike 64-bit mode, really uses them) and reload FS/GS. t->fs_base /
//    t->gs_base are refreshed from the loaded descriptors.
//  - 64-bit threads: the same if they loaded a TLS selector themselves
//    (Wine's 32-bit TEB in %fs), else drop selectors another thread left.
void load(Cpu* cpu, sched::Thread* t, bool compat);

// execve: forget the old program's TLS descriptors and selectors and load
// the segment state the new program starts with.
void reset(sched::Thread* t, bool compat);

// set_thread_area on the running thread: make the new descriptor visible
// immediately (the program loads %gs right after the call).
void install_current(sched::Thread* t);

// arch_prctl(ARCH_SET_FS / ARCH_SET_GS) on the running thread: a 64-bit
// base replaces whatever selector was loaded (Linux does the same).
void clear_fs();
void clear_gs();

// Live selector values (for clone/fork: the child starts with the parent's).
u16 read_fs();
u16 read_gs();

} // namespace compat_segments
