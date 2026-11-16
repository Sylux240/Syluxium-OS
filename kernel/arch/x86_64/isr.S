/*
 * Interrupt entry stubs for all 256 vectors.
 *
 * Each stub is exactly 16 bytes apart, so the C++ side finds stub N at
 * isr_stubs + N * 16 without needing a table. Every stub makes the stack
 * look the same (error code + vector number) and jumps to isr_common.
 */

.section .text

.global isr_stubs
.align 16
isr_stubs:
.set vec, 0
.rept 256
    .align 16
    /* These exceptions push an error code themselves; all others get a fake 0. */
    .if (vec == 8) || (vec == 10) || (vec == 11) || (vec == 12) || (vec == 13) || (vec == 14) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
    .else
        pushq $0
    .endif
    pushq $vec
    jmp isr_common
    .set vec, vec + 1
.endr

/*
 * Coming from user mode (CS RPL = 3) the GS base holds the user's value;
 * swapgs puts the kernel's per-CPU pointer back (and again on the way out).
 * Stack here: vector, error code, rip, cs, ...  -> cs is at 24(%rsp).
 */
isr_common:
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* The CPU aligned RSP to 16 before pushing its 5 qwords; with our 17 more,
     * RSP is 16-byte aligned here as the SysV ABI requires for the call. */
    movq %rsp, %rdi
    call isr_dispatch
    cli                 /* a handler may have enabled interrupts; the exit path must not be interrupted */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    testb $3, 24(%rsp)  /* returning to user mode? */
    jz 2f
    swapgs
2:
    addq $16, %rsp      /* drop vector + error code */
    iretq

.section .note.GNU-stack, "", @progbits
