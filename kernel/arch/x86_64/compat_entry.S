/*
 * System call entry for 32-bit (ia32) programs.
 *
 * 32-bit Linux programs enter the kernel with `int $0x80` (glibc calls it
 * through AT_SYSINFO / the vsyscall page or directly). Arguments are in
 * ebx, ecx, edx, esi, edi, ebp and the number in eax - a different ABI and
 * different syscall numbers than 64-bit `syscall`, so these frames go to the
 * compat dispatcher (syscall/compat32.cpp) instead of syscall_dispatch.
 *
 * The register frame is the same interrupts::Frame every other entry
 * builds (vector 0x80 marks it as a restartable syscall frame, the "error
 * code" slot keeps the original number), and the exit is the shared
 * return_to_user_frame path (iretq), which returns to compatibility mode
 * because the saved CS is the 32-bit user code segment.
 */

#define OFFSET_KERNEL_STACK 24
#define OFFSET_USER_RSP     32
#define USER_DATA_SEL       0x1b
#define USER32_CODE_SEL     0x3b

.macro PUSH_REGS
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
.endm

.section .text

/*
 * int $0x80 (IDT gate with DPL 3, interrupts off on entry). The CPU already
 * switched to the thread's kernel stack (TSS rsp0) and pushed ss/rsp/rflags/
 * cs/rip. The gate is only meant for user mode; a ring-0 `int $0x80` would be
 * a kernel bug, so it's routed to the generic stub (logs "unhandled vector").
 */
.global int80_entry
int80_entry:
    testb $3, 8(%rsp)                   /* cs (rip is at 0) */
    jz int80_from_kernel
    swapgs
    pushq %rax                          /* "error code" slot: original syscall number */
    pushq $0x80                         /* "vector": marks a syscall frame */
    PUSH_REGS
    cld
    sti
    movq %rsp, %rdi
    call compat_syscall_dispatch
    cli
    jmp return_to_user_frame

int80_from_kernel:
    jmp isr_stubs + 0x80 * 16

/*
 * SYSCALL executed in compatibility mode (AMD CPUs only; Intel raises #UD).
 * The CPU enters at MSR_CSTAR with RCX = return EIP, R11 = EFLAGS and the
 * user stack still loaded, exactly like 64-bit SYSCALL. Linux's convention
 * for this path: ECX is lost, so argument 2 travels in EBP and argument 6
 * on the user stack; compat_syscall32_dispatch picks them up from there.
 * Nothing on Syluxium emits it (the vsyscall page uses int $0x80), but the
 * MSR must point somewhere safe: left at 0, a 32-bit `syscall` would run
 * ring-0 code at address 0 on the user's stack.
 */
.global syscall32_entry
syscall32_entry:
    swapgs
    movq %rsp, %gs:OFFSET_USER_RSP
    movq %gs:OFFSET_KERNEL_STACK, %rsp
    pushq $USER_DATA_SEL                /* ss */
    pushq %gs:OFFSET_USER_RSP           /* rsp */
    pushq %r11                          /* rflags */
    pushq $USER32_CODE_SEL              /* cs */
    pushq %rcx                          /* rip */
    pushq %rax
    pushq $0x80
    PUSH_REGS
    cld
    sti
    movq %rsp, %rdi
    call compat_syscall32_dispatch
    cli
    jmp return_to_user_frame

.section .note.GNU-stack, "", @progbits
