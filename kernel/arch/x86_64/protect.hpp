#pragma once

#include <lib/types.hpp>

// Hardware protection of the kernel against user memory:
//  - SMEP: the kernel can't execute code in user pages.
//  - SMAP: the kernel can't read or write user pages, except inside a
//    user-access window (STAC ... CLAC) -- what uaccess:: opens around its
//    copies.
// Boot options: "nosmep", "nosmap"; "smap=strict" panics on a stray kernel
// access to user memory instead of reporting it and letting it through.
namespace protect {

void init_this_cpu(); // after the CPU's control registers are set up (BSP and every AP)

bool smep();
bool smap();

// The SMAP state, for the page fault handler: a kernel access to a present
// user page outside a window. Returns true if the fault was handled (the
// access is allowed to retry); false = report it as a kernel fault.
bool smap_violation(u64 addr, u64 rip, u64* rflags, u64 caller);
void report_pending(); // log the code addresses noted by smap_violation (call with no locks held)

extern bool g_smap;

// Open / close a user-access window (no-ops without SMAP).
inline void user_begin() {
    if (g_smap)
        asm volatile(".byte 0x0f, 0x01, 0xcb" ::: "memory"); // stac
}
inline void user_end() {
    if (g_smap)
        asm volatile(".byte 0x0f, 0x01, 0xca" ::: "memory"); // clac
}

// RAII form.
struct UserAccess {
    UserAccess() { user_begin(); }
    ~UserAccess() { user_end(); }
    UserAccess(const UserAccess&) = delete;
    UserAccess& operator=(const UserAccess&) = delete;
};

} // namespace protect
