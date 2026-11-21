/*
 * User-memory accesses the futex code makes while holding a spinlock.
 * A page fault there must not be resolved (that can sleep on the address
 * space's lock): the page-fault handler sees the faulting instruction is
 * one of the two below and resumes at futex_access_fixup instead, which
 * returns -EFAULT. The caller drops its lock, faults the page in the normal
 * way and tries again. Callers open a SMAP window (protect::UserAccess).
 *
 * int futex_try_read(const u32* uaddr, u32* out)          0 / -EFAULT
 * int futex_try_cmpxchg(u32* uaddr, u32* expected, u32 desired)
 *     0: swapped; 1: *uaddr wasn't *expected (*expected = its value); -EFAULT
 */
.section .text
.global futex_try_read
futex_try_read:
.global futex_access_insn_read
futex_access_insn_read:
    movl (%rdi), %eax
    movl %eax, (%rsi)
    xorl %eax, %eax
    ret

.global futex_try_cmpxchg
futex_try_cmpxchg:
    movl (%rsi), %eax
.global futex_access_insn_cmpxchg
futex_access_insn_cmpxchg:
    lock cmpxchgl %edx, (%rdi)
    jne 1f
    xorl %eax, %eax
    ret
1:  movl %eax, (%rsi)
    movl $1, %eax
    ret

/* Both instructions are leaf-function bodies: the stack holds just the
 * return address, so returning from here goes straight back to the caller. */
.global futex_access_fixup
futex_access_fixup:
    movl $-14, %eax
    ret
