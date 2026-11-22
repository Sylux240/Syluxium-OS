#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <core/log.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/panic.hpp>
#include <sched/sched.hpp>

extern "C" char isr_stubs[]; // isr.S: 256 stubs, 16 bytes apart

namespace interrupts {

namespace {

struct PACKED IdtEntry {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
};
static_assert(sizeof(IdtEntry) == 16);

struct PACKED Idtr {
    u16 limit;
    u64 base;
};

constexpr u8 GATE_INTERRUPT = 0x8E; // present, DPL0, 64-bit interrupt gate (IF cleared on entry)
constexpr u8 GATE_USER_INTERRUPT = 0xEE; // same, but DPL3: `int N` allowed from ring 3

ALIGNED(16) IdtEntry g_idt[256];
Handler g_handlers[256];
u8 g_next_vector = VEC_DEVICE_FIRST;

const char* const exception_names[32] = {
    "Divide Error",          "Debug",                  "Non-Maskable Interrupt", "Breakpoint",
    "Overflow",              "Bound Range Exceeded",   "Invalid Opcode",         "Device Not Available",
    "Double Fault",          "Coprocessor Overrun",    "Invalid TSS",            "Segment Not Present",
    "Stack-Segment Fault",   "General Protection Fault", "Page Fault",           "Reserved",
    "x87 FP Exception",      "Alignment Check",        "Machine Check",          "SIMD FP Exception",
    "Virtualization Exception", "Control Protection",  "Reserved",               "Reserved",
    "Reserved",              "Reserved",               "Reserved",               "Reserved",
    "Hypervisor Injection",  "VMM Communication",      "Security Exception",     "Reserved",
};

void set_gate(u8 vector, u64 handler, u8 ist, u8 type_attr) {
    g_idt[vector] = IdtEntry{
        .offset_low = static_cast<u16>(handler),
        .selector = gdt::KERNEL_CODE,
        .ist = ist,
        .type_attr = type_attr,
        .offset_mid = static_cast<u16>(handler >> 16),
        .offset_high = static_cast<u32>(handler >> 32),
        .reserved = 0,
    };
}

void breakpoint_handler(Frame* frame) {
    klog::debug("breakpoint hit at rip=%p", reinterpret_cast<void*>(frame->rip));
}

UserFaultHook g_user_fault_hook = nullptr;
UserFaultHook g_return_to_user_hook = nullptr;

} // namespace

const char* exception_name(u64 vector) { return vector < 32 ? exception_names[vector] : "Unknown"; }

void set_user_fault_hook(UserFaultHook hook) { g_user_fault_hook = hook; }
void set_return_to_user_hook(UserFaultHook hook) { g_return_to_user_hook = hook; }

[[noreturn]] void fatal_exception(Frame* frame) {
    const char* name = exception_name(frame->vector);
    klog::break_lock();
    kprintf("\n\x1b[91mCPU exception %lu: %s (error code %#lx)\x1b[0m\n",
            frame->vector, name, frame->error_code);
    if (frame->vector == VEC_PAGE_FAULT) {
        u64 ec = frame->error_code;
        kprintf("  faulting address: %p (%s, %s, %s%s)\n",
                reinterpret_cast<void*>(arch::read_cr2()),
                ec & 1 ? "protection violation" : "page not present",
                ec & 2 ? "write" : "read",
                ec & 4 ? "user mode" : "kernel mode",
                ec & 16 ? ", instruction fetch" : "");
    }
    dump_frame(frame);
    panic("unhandled %s", name);
}

void init() {
    for (int v = 0; v < 256; v++) {
        u64 stub = reinterpret_cast<u64>(isr_stubs) + v * 16;
        set_gate(static_cast<u8>(v), stub, 0, GATE_INTERRUPT);
    }
    // Run these on dedicated stacks: they can fire while the current stack is unusable.
    set_gate(VEC_DOUBLE_FAULT, reinterpret_cast<u64>(isr_stubs) + VEC_DOUBLE_FAULT * 16,
             gdt::IST_DOUBLE_FAULT, GATE_INTERRUPT);
    set_gate(VEC_NMI, reinterpret_cast<u64>(isr_stubs) + VEC_NMI * 16, gdt::IST_NMI, GATE_INTERRUPT);
    set_gate(VEC_MACHINE_CHECK, reinterpret_cast<u64>(isr_stubs) + VEC_MACHINE_CHECK * 16,
             gdt::IST_MACHINE_CHECK, GATE_INTERRUPT);

    set_handler(VEC_BREAKPOINT, breakpoint_handler);
    set_handler(VEC_PANIC_HALT, [](Frame*) { arch::halt_forever(); });
    load_idt();
}

void load_idt() {
    Idtr idtr{sizeof(g_idt) - 1, reinterpret_cast<u64>(g_idt)};
    asm volatile("lidt %0" : : "m"(idtr) : "memory");
}

void set_handler(u8 vector, Handler handler) { g_handlers[vector] = handler; }

void set_user_gate(u8 vector, void (*entry)()) {
    set_gate(vector, reinterpret_cast<u64>(entry), 0, GATE_USER_INTERRUPT);
}

u8 allocate_vector() {
    // Compare-and-swap, so the counter stops at the end of the device range
    // instead of wrapping round to vectors that belong to the CPU (with
    // several MSI-X vectors per device the range can run out).
    u8 cur = __atomic_load_n(&g_next_vector, __ATOMIC_RELAXED);
    for (;;) {
        u8 v = cur == VEC_SYSCALL_INT80 ? static_cast<u8>(cur + 1) : cur; // 0x80: 32-bit system calls
        if (v < VEC_DEVICE_FIRST || v > VEC_DEVICE_LAST)
            return 0;
        if (__atomic_compare_exchange_n(&g_next_vector, &cur, static_cast<u8>(v + 1), false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return v;
    }
}

void dump_frame(const Frame* f) {
    kprintf("  RIP=%016lx CS=%04lx RFLAGS=%016lx\n", f->rip, f->cs, f->rflags);
    kprintf("  RSP=%016lx SS=%04lx\n", f->rsp, f->ss);
    kprintf("  RAX=%016lx RBX=%016lx RCX=%016lx\n", f->rax, f->rbx, f->rcx);
    kprintf("  RDX=%016lx RSI=%016lx RDI=%016lx\n", f->rdx, f->rsi, f->rdi);
    kprintf("  RBP=%016lx R8 =%016lx R9 =%016lx\n", f->rbp, f->r8, f->r9);
    kprintf("  R10=%016lx R11=%016lx R12=%016lx\n", f->r10, f->r11, f->r12);
    kprintf("  R13=%016lx R14=%016lx R15=%016lx\n", f->r13, f->r14, f->r15);
    kprintf("  CR0=%016lx CR3=%016lx CR4=%016lx\n", arch::read_cr0(), arch::read_cr3(), arch::read_cr4());
}

} // namespace interrupts

// ---- MSR access that survives a #GP ----
// The faulting instruction (rdmsr/wrmsr, 2 bytes) is found by its address;
// the handler sets RSI = 1 and resumes after it.
extern "C" char rdmsr_safe_insn[], wrmsr_safe_insn[];

namespace arch {

bool rdmsr_safe(u32 msr, u64* value) {
    u32 lo = 0, hi = 0;
    u64 fault = 0;
    asm volatile(".globl rdmsr_safe_insn\nrdmsr_safe_insn:\n\trdmsr"
                 : "=a"(lo), "=d"(hi), "+S"(fault)
                 : "c"(msr)
                 : "memory");
    *value = fault ? 0 : (static_cast<u64>(hi) << 32) | lo;
    return !fault;
}

bool wrmsr_safe(u32 msr, u64 value) {
    u64 fault = 0;
    asm volatile(".globl wrmsr_safe_insn\nwrmsr_safe_insn:\n\twrmsr"
                 : "+S"(fault)
                 : "c"(msr), "a"(static_cast<u32>(value)), "d"(static_cast<u32>(value >> 32))
                 : "memory");
    return !fault;
}

} // namespace arch

namespace {

bool fixup_msr_fault(interrupts::Frame* frame) {
    if (frame->vector != 13 || (frame->cs & 3))
        return false;
    if (frame->rip != reinterpret_cast<u64>(rdmsr_safe_insn) && frame->rip != reinterpret_cast<u64>(wrmsr_safe_insn))
        return false;
    frame->rsi = 1;
    frame->rip += 2;
    return true;
}

} // namespace

// Called from isr_common with interrupts disabled.
extern "C" void isr_dispatch(interrupts::Frame* frame) {
    using namespace interrupts;
    u64 vector = frame->vector;

    // Acknowledge APIC interrupts first, so a handler may switch threads.
    // (Spurious interrupts and the masked legacy PIC range need no EOI.)
    if (vector >= VEC_DEVICE_FIRST && vector != VEC_SPURIOUS)
        apic::eoi();

    if (Handler h = g_handlers[vector]) {
        h(frame);
    } else if (vector < 32 && fixup_msr_fault(frame)) {
        // rdmsr_safe/wrmsr_safe: resumed after the instruction
    } else if (vector < 32) {
        if ((frame->cs & 3) && g_user_fault_hook)
            g_user_fault_hook(frame); // kills the process, or queues a signal for its handler
        // Returning for a user process means a signal was queued for its handler.
        if (!(frame->cs & 3) || !sched::current()->process)
            fatal_exception(frame);
    } else if (vector != VEC_SPURIOUS && (vector < VEC_LEGACY_PIC_BASE || vector >= VEC_DEVICE_FIRST)) {
        klog::warn("unhandled interrupt vector %lu", vector);
    }

    // Preempt on the way out if the scheduler asked for it and the
    // interrupted code was preemptible (interrupts on, no spinlock held).
    Cpu* cpu = this_cpu();
    if (vector >= 32 && cpu->need_resched && cpu->preempt_count == 0 && (frame->rflags & arch::RFLAGS_IF))
        sched::schedule();

    if ((frame->cs & 3) && g_return_to_user_hook)
        g_return_to_user_hook(frame);
}
