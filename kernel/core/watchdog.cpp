// Lockup detector: a CPU whose scheduler tick stops advancing for 4 seconds
// while it isn't idle (stuck with interrupts off - a spinlock taken twice,
// an endless loop) gets a non-maskable interrupt, whose handler prints where
// it was: the instruction pointer and the call chain (resolve with
// addr2line -e build/syluxium.elf <addresses>).

#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/bootlog.hpp>
#include <core/log.hpp>
#include <core/watchdog.hpp>
#include <sched/sched.hpp>

namespace watchdog {

namespace {

constexpr u32 MAX = 64;
u64 g_last[MAX];
u32 g_stale[MAX];
bool g_reported[MAX];
volatile bool g_dump_request[MAX];
volatile bool g_thread_dump_request = false;

void nmi(interrupts::Frame* f) {
    Cpu* cpu = this_cpu();
    if (cpu->index >= MAX || !g_dump_request[cpu->index])
        return;
    g_dump_request[cpu->index] = false;
    klog::break_lock();
    sched::Thread* t = cpu->current;
    kprintf("\n[ LOCKUP ] CPU %u stuck (thread %s): rip %016lx\n[ LOCKUP ] call chain:", cpu->index,
            t ? t->name : "?", f->rip);
    const u64* fp = reinterpret_cast<const u64*>(f->rbp);
    for (int i = 0; i < 16 && reinterpret_cast<u64>(fp) >= 0xFFFF800000000000ull && (reinterpret_cast<u64>(fp) & 7) == 0;
         i++) {
        u64 ret = fp[1];
        if (ret < 0xFFFF800000000000ull)
            break;
        kprintf(" %016lx", ret);
        const u64* next = reinterpret_cast<const u64*>(fp[0]);
        if (next <= fp)
            break;
        fp = next;
    }
    kprintf("\n");
}

void thread(void*) {
    bool save_pending = false;
    for (;;) {
        sched::sleep_ms(1000);
        if (g_thread_dump_request) {
            g_thread_dump_request = false;
            sched::dump_threads();
            bootlog::save(); // onto the stick now: the next step may be the power button
        }
        if (save_pending) { // a lockup was reported a second ago (from the NMI)
            save_pending = false;
            bootlog::save();
        }
        u32 n = percpu::count();
        for (u32 i = 0; i < n && i < MAX; i++) {
            Cpu* c = percpu::get(i);
            if (!c || !c->online || c->tickless || c->current == c->rq.idle) {
                g_stale[i] = 0;
                g_last[i] = c ? c->ticks : 0;
                continue;
            }
            if (c->ticks != g_last[i]) {
                g_last[i] = c->ticks;
                g_stale[i] = 0;
                g_reported[i] = false;
                continue;
            }
            if (++g_stale[i] >= 4 && !g_reported[i]) {
                g_reported[i] = true;
                g_dump_request[i] = true;
                apic::send_nmi(c->lapic_id);
                save_pending = true;
            }
        }
    }
}

} // namespace

void request_thread_dump() {
    g_thread_dump_request = true;
}

void start() {
    interrupts::set_handler(interrupts::VEC_NMI, nmi);
    sched::spawn("watchdog", thread, nullptr, -1, true);
}

} // namespace watchdog
