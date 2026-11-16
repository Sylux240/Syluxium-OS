/*
 * void context_switch(Thread* prev, Thread* next)
 *
 * Saves the callee-saved registers of `prev` on its stack, stores its stack
 * pointer in prev->saved_rsp (offset 0), then loads next's stack and
 * restores its registers. The `ret` continues wherever `next` last called
 * context_switch, or, for a brand-new thread, at thread_start().
 *
 * Caller-saved registers don't need saving: the C++ caller already assumes
 * they're clobbered by the call.
 */
.section .text
.global context_switch
context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq (%rsi), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.section .note.GNU-stack, "", @progbits
