// Process-level support for 32-bit (ia32) programs. Structure layouts and
// behavior follow Linux's IA32 emulation (arch/x86/ia32, arch/x86/kernel/tls.c).

#include <arch/x86_64/compat_segments.hpp>
#include <arch/x86_64/fpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/compat.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sched.hpp>

namespace compat {

using namespace proc;

namespace {

// ---- vsyscall page ----

// Machine code of the three entry points (offsets match compat.hpp).
constexpr u8 VSYSCALL_CODE[] = {
    // 0x00 __kernel_vsyscall:      int $0x80; ret
    0xCD, 0x80, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    // 0x10 __kernel_sigreturn:     popl %eax; movl $119, %eax; int $0x80
    0x58, 0xB8, 0x77, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    // 0x20 __kernel_rt_sigreturn:  movl $173, %eax; int $0x80
    0xB8, 0xAD, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x90,
};

// ---- Signal frames (Linux ia32 layouts, 4-byte packing) ----

struct SigContext32 {
    u16 gs, gsh, fs, fsh, es, esh, ds, dsh;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 trapno, err, eip;
    u16 cs, csh;
    u32 eflags, esp_at_signal;
    u16 ss, ssh;
    u32 fpstate; // -> struct _fpstate_32
    u32 oldmask, cr2;
};
static_assert(sizeof(SigContext32) == 88);

struct StackT32 {
    u32 ss_sp;
    i32 ss_flags;
    u32 ss_size;
};

struct UContext32 {
    u32 uc_flags;
    u32 uc_link;
    StackT32 uc_stack;
    SigContext32 mcontext;
    u32 sigmask[2];
};
static_assert(sizeof(UContext32) == 116);

struct SigInfo32 {
    i32 si_signo, si_errno, si_code;
    u8 fields[116];
};
static_assert(sizeof(SigInfo32) == 128);

// rt_sigframe_ia32 (SA_SIGINFO handlers; returns through rt_sigreturn).
struct RtSigFrame32 {
    u32 pretcode;
    i32 sig;
    u32 pinfo;
    u32 puc;
    SigInfo32 info;
    UContext32 uc;
    u8 retcode[8];
};
static_assert(sizeof(RtSigFrame32) == 268);

// sigframe_ia32 (plain handlers; returns through sigreturn). The unused
// fpstate keeps extramask at the offset old programs expect.
struct SigFrame32 {
    u32 pretcode;
    i32 sig;
    SigContext32 sc;
    u8 fpstate_unused[624];
    u32 extramask;
    u8 retcode[8];
};
static_assert(sizeof(SigFrame32) == 732);

// struct _fpstate_32 starts with a 112-byte i387 FSAVE-format header; the
// FXSAVE/XSAVE image follows at offset 112 (status/magic at 108/110).
constexpr usize FPSTATE_HEADER = 112;
constexpr u16 X86_FXSR_MAGIC = 0x0000; // "the FXSR image follows"

constexpr u64 SA_SIGINFO = 0x4, SA_RESTORER = 0x04000000;
constexpr u64 FIX_EFLAGS = 0x50DD5; // AC OF DF TF SF ZF AF PF CF RF: what sigreturn may change
constexpr u64 UNBLOCKABLE = (1ull << (signal::SIGKILL - 1)) | (1ull << (signal::SIGSTOP - 1));

void fill_sigcontext(SigContext32* c, const interrupts::Frame* f, u64 blocked, u32 fpstate) {
    memset(c, 0, sizeof(*c));
    c->gs = compat_segments::read_gs();
    c->fs = compat_segments::read_fs();
    c->es = c->ds = gdt::USER_DATA;
    c->edi = static_cast<u32>(f->rdi); c->esi = static_cast<u32>(f->rsi);
    c->ebp = static_cast<u32>(f->rbp); c->esp = static_cast<u32>(f->rsp);
    c->ebx = static_cast<u32>(f->rbx); c->edx = static_cast<u32>(f->rdx);
    c->ecx = static_cast<u32>(f->rcx); c->eax = static_cast<u32>(f->rax);
    c->trapno = f->vector < 32 ? static_cast<u32>(f->vector) : 0;
    c->err = f->vector < 32 ? static_cast<u32>(f->error_code) : 0;
    c->eip = static_cast<u32>(f->rip);
    c->cs = static_cast<u16>(f->cs);
    c->eflags = static_cast<u32>(f->rflags);
    c->esp_at_signal = static_cast<u32>(f->rsp);
    c->ss = static_cast<u16>(f->ss);
    c->fpstate = fpstate;
    c->oldmask = static_cast<u32>(blocked);
}

void restore_sigcontext(interrupts::Frame* f, const SigContext32& c) {
    f->rdi = c.edi; f->rsi = c.esi; f->rbp = c.ebp; f->rsp = c.esp;
    f->rbx = c.ebx; f->rdx = c.edx; f->rcx = c.ecx; f->rax = c.eax;
    // r8-r15 don't exist for 32-bit code; clear whatever the kernel left there.
    f->r8 = f->r9 = f->r10 = f->r11 = f->r12 = f->r13 = f->r14 = f->r15 = 0;
    f->rip = c.eip;
    f->rflags = (f->rflags & ~FIX_EFLAGS) | (c.eflags & FIX_EFLAGS) | arch::RFLAGS_IF;
    // Always back to the flat 32-bit segments: a context can't smuggle in
    // a 64-bit or kernel selector. (FS/GS are left as they are.)
    f->cs = gdt::USER32_CODE;
    f->ss = gdt::USER_DATA;
    f->vector = 0; // not a restartable syscall frame any more
}

// Copy the thread's FPU state into a user _fpstate_32 at `addr`.
bool save_fpstate(sched::Thread* t, uptr addr) {
    usize size = fpu::state_size();
    const u8* image = static_cast<const u8*>(t->fpu_state);
    u8 header[FPSTATE_HEADER] = {};
    // cw / sw (the first two words of the FSAVE environment) from FXSAVE's fcw / fsw.
    header[0] = image[0]; header[1] = image[1];
    header[4] = image[2]; header[5] = image[3];
    header[108] = image[2]; header[109] = image[3]; // status
    header[110] = X86_FXSR_MAGIC & 0xFF;
    header[111] = X86_FXSR_MAGIC >> 8;
    return uaccess::copy_to(reinterpret_cast<void*>(addr), header, FPSTATE_HEADER) == 0 &&
           uaccess::copy_to(reinterpret_cast<void*>(addr + FPSTATE_HEADER), image, size) == 0;
}

void restore_fpstate(sched::Thread* t, u32 addr) {
    if (!addr)
        return;
    usize size = fpu::state_size();
    auto* tmp = static_cast<u8*>(kmalloc(size));
    if (tmp && uaccess::copy_from(tmp, reinterpret_cast<void*>(static_cast<uptr>(addr) + FPSTATE_HEADER), size) == 0) {
        fpu::sanitize(tmp);
        memcpy(t->fpu_state, tmp, size);
        fpu::restore(t->fpu_state);
    }
    kfree(tmp);
}

// ---- TLS descriptors ----

constexpr u32 UD_SEG_32BIT = 1 << 0, UD_CONTENTS_SHIFT = 1, UD_READ_EXEC_ONLY = 1 << 3,
              UD_LIMIT_IN_PAGES = 1 << 4, UD_SEG_NOT_PRESENT = 1 << 5, UD_USEABLE = 1 << 6, UD_LM = 1 << 7;

u32 contents(const UserDesc& d) { return (d.flags >> UD_CONTENTS_SHIFT) & 3; }

// "Clear this slot" encodings (Linux LDT_empty / LDT_zero).
bool desc_empty(const UserDesc& d) {
    bool classic = d.base_addr == 0 && d.limit == 0 && contents(d) == 0 && (d.flags & UD_READ_EXEC_ONLY) &&
                   !(d.flags & UD_SEG_32BIT) && !(d.flags & UD_LIMIT_IN_PAGES) && (d.flags & UD_SEG_NOT_PRESENT) &&
                   !(d.flags & UD_USEABLE);
    bool zero = d.base_addr == 0 && d.limit == 0 && (d.flags & 0x7F) == 0;
    return classic || zero;
}

// Only present 32-bit data segments may go into the TLS slots (Linux tls_desc_okay).
bool desc_okay(const UserDesc& d) {
    if (desc_empty(d))
        return true;
    if (!(d.flags & UD_SEG_32BIT) || contents(d) > 1 || (d.flags & UD_SEG_NOT_PRESENT) || (d.flags & UD_LM))
        return false;
    return true;
}

u64 encode(const UserDesc& d) {
    if (desc_empty(d))
        return 0;
    u64 type = ((d.flags & UD_READ_EXEC_ONLY) ? 0 : 2) | (static_cast<u64>(contents(d)) << 2) | 1 /* accessed */;
    u64 desc = (d.limit & 0xFFFF)
             | (static_cast<u64>(d.base_addr & 0xFFFFFF) << 16)
             | (type << 40)
             | (1ull << 44)                 // S: code/data segment
             | (3ull << 45)                 // DPL 3
             | (1ull << 47)                 // present
             | (static_cast<u64>((d.limit >> 16) & 0xF) << 48)
             | (static_cast<u64>((d.flags & UD_USEABLE) ? 1 : 0) << 52)
             | (1ull << 54)                 // D: 32-bit
             | (static_cast<u64>((d.flags & UD_LIMIT_IN_PAGES) ? 1 : 0) << 55)
             | (static_cast<u64>(d.base_addr >> 24) << 56);
    return desc;
}

void decode(u64 desc, UserDesc* d) {
    u32 entry = d->entry_number;
    memset(d, 0, sizeof(*d));
    d->entry_number = entry;
    if (!desc) {
        d->flags = UD_READ_EXEC_ONLY | UD_SEG_NOT_PRESENT; // what Linux reports for an empty slot
        return;
    }
    d->base_addr = static_cast<u32>(((desc >> 16) & 0xFFFFFF) | ((desc >> 56) << 24));
    d->limit = static_cast<u32>((desc & 0xFFFF) | (((desc >> 48) & 0xF) << 16));
    u32 type = (desc >> 40) & 0xF;
    d->flags = (((desc >> 54) & 1) ? UD_SEG_32BIT : 0) | (((type >> 2) & 3) << UD_CONTENTS_SHIFT) |
               ((type & 2) ? 0 : UD_READ_EXEC_ONLY) | (((desc >> 55) & 1) ? UD_LIMIT_IN_PAGES : 0) |
               (((desc >> 47) & 1) ? 0 : UD_SEG_NOT_PRESENT) | (((desc >> 52) & 1) ? UD_USEABLE : 0);
}

} // namespace

long map_vsyscall_page(Memory* mem) {
    uptr addr;
    long r = mem->map(COMPAT_VSYSCALL, mm::PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, nullptr, 0, 0, &addr);
    if (r < 0)
        return r;
    if ((r = mem->write_remote(COMPAT_VSYSCALL, VSYSCALL_CODE, sizeof(VSYSCALL_CODE))) < 0)
        return r;
    return mem->protect(COMPAT_VSYSCALL, mm::PAGE_SIZE, PROT_READ | PROT_EXEC);
}

interrupts::Frame initial_frame(u64 entry, u64 stack_ptr) {
    interrupts::Frame f{};
    f.rip = entry;
    f.cs = gdt::USER32_CODE;
    f.rflags = 0x202; // IF set, reserved bit 1
    f.rsp = stack_ptr;
    f.ss = gdt::USER_DATA;
    return f;
}

bool setup_signal_frame(interrupts::Frame* frame, int sig, const proc::SigAction& act, u64 blocked) {
    sched::Thread* t = sched::current();
    bool rt = act.flags & SA_SIGINFO;
    usize fp_size = fpu::state_size();

    // FPU state above the frame, its FXSAVE/XSAVE image 64-byte aligned.
    // On the alternate signal stack if the handler wants it (SA_ONSTACK).
    bool switched;
    signal::AltStack uc_stack;
    uptr esp = signal::handler_stack(t, act.flags, frame->rsp & 0xFFFFFFFF, &switched, &uc_stack);
    uptr fp_addr = align_down<uptr>(esp - fp_size, 64) - FPSTATE_HEADER;
    usize frame_size = rt ? sizeof(RtSigFrame32) : sizeof(SigFrame32);
    // As on Linux: the handler starts with ((esp + 4) & 15) == 0, i.e. as if
    // `call`ed with a 16-byte aligned stack.
    uptr sp = ((fp_addr - frame_size + 4) & ~uptr(15)) - 4;
    if (sp < USER_MIN)
        return false;
    fpu::save(t->fpu_state);
    if (!save_fpstate(t, fp_addr))
        return false;

    u32 restorer = static_cast<u32>((act.flags & SA_RESTORER) ? act.restorer
                                                              : (rt ? VSYSCALL_RT_SIGRETURN : VSYSCALL_SIGRETURN));
    if (rt) {
        RtSigFrame32 f{};
        f.pretcode = restorer;
        f.sig = sig;
        f.pinfo = static_cast<u32>(sp + __builtin_offsetof(RtSigFrame32, info));
        f.puc = static_cast<u32>(sp + __builtin_offsetof(RtSigFrame32, uc));
        f.info.si_signo = sig;
        f.uc.uc_stack = StackT32{static_cast<u32>(uc_stack.sp), static_cast<i32>(uc_stack.flags),
                                 static_cast<u32>(uc_stack.size)};
        fill_sigcontext(&f.uc.mcontext, frame, blocked, static_cast<u32>(fp_addr));
        f.uc.sigmask[0] = static_cast<u32>(blocked);
        f.uc.sigmask[1] = static_cast<u32>(blocked >> 32);
        // movl $173, %eax; int $0x80 - never executed, but debuggers look for it.
        const u8 code[8] = {0xB8, 0xAD, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x00};
        memcpy(f.retcode, code, sizeof(code));
        if (uaccess::copy_to(reinterpret_cast<void*>(sp), &f, sizeof(f)) < 0)
            return false;
        frame->rdx = f.pinfo;
        frame->rcx = f.puc;
    } else {
        auto* f = static_cast<SigFrame32*>(kmalloc(sizeof(SigFrame32)));
        if (!f)
            return false;
        memset(f, 0, sizeof(*f));
        f->pretcode = restorer;
        f->sig = sig;
        fill_sigcontext(&f->sc, frame, blocked, static_cast<u32>(fp_addr));
        f->extramask = static_cast<u32>(blocked >> 32);
        // popl %eax; movl $119, %eax; int $0x80
        const u8 code[8] = {0x58, 0xB8, 0x77, 0x00, 0x00, 0x00, 0xCD, 0x80};
        memcpy(f->retcode, code, sizeof(code));
        long r = uaccess::copy_to(reinterpret_cast<void*>(sp), f, sizeof(*f));
        kfree(f);
        if (r < 0)
            return false;
        frame->rdx = 0;
        frame->rcx = 0;
    }

    frame->rip = static_cast<u32>(act.handler);
    frame->rsp = sp;
    frame->rax = static_cast<u64>(sig);
    frame->cs = gdt::USER32_CODE;
    frame->ss = gdt::USER_DATA;
    frame->rflags &= ~(0x400ull | 0x100ull); // clear DF and TF for the handler
    return true;
}

long sigreturn(interrupts::Frame* frame, bool rt) {
    sched::Thread* t = sched::current();
    uptr esp = frame->rsp & 0xFFFFFFFF;
    u64 mask;
    u32 fpstate;
    if (rt) {
        // The handler's `ret` popped pretcode.
        RtSigFrame32 f;
        if (uaccess::copy_from(&f, reinterpret_cast<void*>(esp - 4), sizeof(f)) < 0)
            proc::exit_current(signal::SIGSEGV);
        restore_sigcontext(frame, f.uc.mcontext);
        signal::restore_altstack(signal::AltStack{f.uc.uc_stack.ss_sp, static_cast<u32>(f.uc.uc_stack.ss_flags),
                                                  f.uc.uc_stack.ss_size},
                                 frame->rsp & 0xFFFFFFFF);
        mask = f.uc.sigmask[0] | (static_cast<u64>(f.uc.sigmask[1]) << 32);
        fpstate = f.uc.mcontext.fpstate;
    } else {
        // The handler's `ret` popped pretcode and the trampoline's `popl %eax` the signal number.
        auto* f = static_cast<SigFrame32*>(kmalloc(sizeof(SigFrame32)));
        if (!f || uaccess::copy_from(f, reinterpret_cast<void*>(esp - 8), sizeof(*f)) < 0) {
            kfree(f);
            proc::exit_current(signal::SIGSEGV);
        }
        restore_sigcontext(frame, f->sc);
        mask = f->sc.oldmask | (static_cast<u64>(f->extramask) << 32);
        fpstate = f->sc.fpstate;
        kfree(f);
    }
    t->sigmask = mask & ~UNBLOCKABLE;
    restore_fpstate(t, fpstate);
    return static_cast<long>(frame->rax); // the dispatcher stores this back into eax
}

long set_tls(sched::Thread* t, UserDesc* d) {
    if (!desc_okay(*d))
        return -err::INVAL;
    int idx;
    if (d->entry_number == 0xFFFFFFFF) {
        for (idx = 0; idx < gdt::TLS_ENTRIES; idx++)
            if (!t->tls_desc[idx])
                break;
        if (idx == gdt::TLS_ENTRIES)
            return -err::SRCH;
        d->entry_number = static_cast<u32>(gdt::TLS_FIRST_ENTRY + idx);
    } else {
        idx = static_cast<int>(d->entry_number) - gdt::TLS_FIRST_ENTRY;
        if (idx < 0 || idx >= gdt::TLS_ENTRIES)
            return -err::INVAL;
    }
    t->tls_desc[idx] = encode(*d);
    return 0;
}

long get_tls(sched::Thread* t, UserDesc* d) {
    int idx = static_cast<int>(d->entry_number) - gdt::TLS_FIRST_ENTRY;
    if (idx < 0 || idx >= gdt::TLS_ENTRIES)
        return -err::INVAL;
    decode(t->tls_desc[idx], d);
    return 0;
}

long clone_tls(sched::Thread* child, sched::Thread* parent, bool settls, u64 tls) {
    memcpy(child->tls_desc, parent->tls_desc, sizeof(child->tls_desc));
    // The parent is running (in this syscall): its selectors are live in the CPU.
    child->user_fs_sel = compat_segments::read_fs();
    child->user_gs_sel = compat_segments::read_gs();
    if (!settls)
        return 0;
    UserDesc d;
    if (uaccess::get(&d, reinterpret_cast<const UserDesc*>(tls & 0xFFFFFFFF)) < 0)
        return -err::FAULT;
    return set_tls(child, &d);
}

} // namespace compat
