/*
 * System call entry (the `syscall` instruction) and the return-to-user path.
 *
 * On `syscall` the CPU loads RIP from LSTAR, saves the user RIP in RCX and
 * RFLAGS in R11, masks RFLAGS with FMASK (interrupts off) and switches CS/SS
 * - but NOT the stack. We switch to the thread's kernel stack by hand and
 * build the same register frame interrupts use (interrupts::Frame), so
 * fork/exec/signals can treat both alike.
 *
 * We return with iretq rather than sysretq: slightly slower, but immune to
 * the sysret non-canonical-RIP problem and works for any frame we've edited.
 */

#define OFFSET_KERNEL_STACK 24
#define OFFSET_USER_RSP     32
#define USER_DATA_SEL       0x2b   /* gdt::USER_DATA */
#define USER_CODE_SEL       0x33   /* gdt::USER_CODE */

.section .text

.global syscall_entry
syscall_entry:
    swapgs                              /* GS -> this CPU's Cpu */
    movq %rsp, %gs:OFFSET_USER_RSP
    movq %gs:OFFSET_KERNEL_STACK, %rsp

    pushq $USER_DATA_SEL                /* ss */
    pushq %gs:OFFSET_USER_RSP           /* rsp */
    pushq %r11                          /* rflags */
    pushq $USER_CODE_SEL                /* cs */
    pushq %rcx                          /* rip */
    pushq %rax                          /* "error code" slot: original syscall number (for restarts) */
    pushq $0x80                         /* "vector": marks a syscall frame */

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

    cld
    sti                                 /* syscalls run with interrupts on */
    movq %rsp, %rdi
    call syscall_dispatch
    cli
    jmp return_to_user_frame

/*
 * void return_to_user(interrupts::Frame* frame)
 * Load a complete user register frame and drop to ring 3. Also used to
 * start a new process.
 */
.global return_to_user
return_to_user:
    cli
    movq %rdi, %rsp
.global return_to_user_frame          /* also the exit path of compat_entry.S */
return_to_user_frame:
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
    addq $16, %rsp                      /* vector + error code */
    swapgs                              /* back to the user's GS base */
    iretq

.section .note.GNU-stack, "", @progbits
