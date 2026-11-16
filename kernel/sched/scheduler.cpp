#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/fpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/vmalloc.hpp>
#include <mm/vmm.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>

// How a switch works
// ------------------
// schedule() takes the CPU's run-queue lock, picks the next thread and calls
// context_switch(). The lock stays held across the switch and is released by
// the *next* thread in finish_switch(). Until then the previous thread is
// marked on_cpu, so no other CPU can steal it while its registers are still
// being saved.
//
// Wakeups always enqueue a thread on the run queue of the CPU it last ran
// on; only idle CPUs move threads around (work stealing).

extern "C" void context_switch(sched::Thread* prev, sched::Thread* next);

namespace sched {

void thread_start_trampoline();

namespace {

using RunQueue = Cpu::RunQueue;

bool g_running = false;
u64 g_next_tid = 1;

IrqSpinlock g_threads_lock; // all-threads list, reaped flags, join waiters
AllList g_all_threads;
u64 g_thread_count = 0;

IrqSpinlock g_reap_lock;
RunList g_reap_list;        // dead threads, linked through run_node
WaitList g_reaper_waiters;

u64 irq_save() {
    u64 flags = arch::read_rflags();
    arch::cli();
    return flags;
}

void irq_restore(u64 flags) {
    if (flags & arch::RFLAGS_IF)
        arch::sti();
}

constexpr u32 MSR_FS_BASE = 0xC0000100;
constexpr u32 MSR_KERNEL_GS_BASE = 0xC0000102;

// Load the per-thread CPU state of the thread that's about to run.
void load_thread_state(Cpu* cpu, Thread* t) {
    u64 top = t->kernel_stack_top();
    cpu->kernel_stack_top = top;              // syscall entry stack
    gdt::set_kernel_stack(cpu->tables, top);  // interrupt-from-user stack
    if (t->fpu_state)
        fpu::restore(t->fpu_state);
    if (t->process) {
        arch::wrmsr(MSR_FS_BASE, t->fs_base);
        arch::wrmsr(MSR_KERNEL_GS_BASE, t->gs_base); // becomes GS on the swapgs back to user
    }
    u64 root = t->aspace ? t->aspace->root() : vmm::AddressSpace::kernel().root();
    if ((arch::read_cr3() & ~0xFFFull) != root)
        arch::write_cr3(root);
}

// Save the per-thread CPU state of the thread being switched away from.
void save_thread_state(Thread* t) {
    if (t->fpu_state)
        fpu::save(t->fpu_state);
    if (t->process)
        t->gs_base = arch::rdmsr(MSR_KERNEL_GS_BASE);
}

// Runs on the new thread right after every switch (lock still held).
void finish_switch() {
    Cpu* cpu = this_cpu();
    load_thread_state(cpu, cpu->current);
    Thread* prev = cpu->switch_prev;
    __atomic_store_n(&prev->on_cpu, false, __ATOMIC_RELEASE);
    cpu->rq.lock.unlock();

    if (prev == &cpu->boot_thread) {
        // This CPU has left its bootloader stack for good.
        __atomic_store_n(&cpu->online, true, __ATOMIC_RELEASE);
    } else if (prev->state == ThreadState::Dead) {
        g_reap_lock.lock();
        g_reap_list.push_back(prev);
        Thread* reaper = g_reaper_waiters.pop_front();
        g_reap_lock.unlock();
        if (reaper)
            wake(reaper);
    }
}

// Take a runnable thread from another CPU. Called with our own lock held;
// try_lock on the victim avoids lock-order deadlocks between two stealers.
Thread* steal_for(Cpu* me) {
    u32 n = percpu::count();
    for (u32 k = 1; k < n; k++) {
        Cpu* victim = percpu::get((me->index + k) % n);
        if (!victim || victim->rq.ready.size() == 0 || !victim->rq.lock.try_lock())
            continue;
        for (Thread* t = victim->rq.ready.front(); t; t = victim->rq.ready.next_of(t)) {
            if (!t->pinned && !__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
                victim->rq.ready.remove(t);
                t->cpu = me;
                victim->rq.lock.unlock();
                return t;
            }
        }
        victim->rq.lock.unlock();
    }
    return nullptr;
}

bool work_elsewhere(Cpu* me) {
    for (u32 i = 0; i < percpu::count(); i++) {
        Cpu* c = percpu::get(i);
        if (c && c != me && c->rq.ready.size() > 0)
            return true;
    }
    return false;
}

void insert_sleeper(RunQueue& rq, Thread* t) {
    Thread* pos = rq.sleepers.front();
    while (pos && pos->wake_deadline <= t->wake_deadline)
        pos = rq.sleepers.next_of(pos);
    rq.sleepers.insert_before(t, pos);
}

Cpu* least_loaded_cpu() {
    Cpu* best = nullptr;
    usize best_load = ~usize(0);
    for (u32 i = 0; i < percpu::count(); i++) {
        Cpu* c = percpu::get(i);
        if (!c || !__atomic_load_n(&c->online, __ATOMIC_ACQUIRE))
            continue;
        usize load = c->rq.ready.size() + (c->current != c->rq.idle ? 1 : 0);
        if (load < best_load) {
            best = c;
            best_load = load;
        }
    }
    return best ? best : this_cpu();
}

void idle_loop(void*) {
    for (;;) {
        // `sti; hlt` as one asm statement: sti only takes effect after the
        // next instruction, so a wakeup IPI can't slip in before the hlt.
        asm volatile("sti; hlt" ::: "memory");
    }
}

void reaper_loop(void*) {
    for (;;) {
        g_reap_lock.lock();
        while (g_reap_list.empty())
            block_on(g_reaper_waiters, g_reap_lock);
        Thread* t = g_reap_list.pop_front();
        g_reap_lock.unlock();

        vmalloc::free(t->stack);
        t->stack = nullptr;
        if (t->fpu_state) {
            fpu::free_state(t->fpu_state);
            t->fpu_state = nullptr;
        }

        g_threads_lock.lock();
        g_all_threads.remove(t);
        g_thread_count--;
        t->reaped = true;
        bool detached = t->detached;
        while (Thread* waiter = t->join_waiters.pop_front())
            wake(waiter);
        g_threads_lock.unlock();

        if (detached)
            delete t;
    }
}

Thread* create_thread(const char* name, void (*entry)(void*), void* arg, bool detached) {
    auto* t = new Thread;
    t->stack = vmalloc::alloc(KERNEL_STACK_SIZE); // guard page below catches overflows
    if (!t->stack)
        panic("sched: out of memory for a kernel stack");
    t->stack_size = KERNEL_STACK_SIZE;
    t->tid = __atomic_fetch_add(&g_next_tid, 1, __ATOMIC_RELAXED);
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->entry = entry;
    t->arg = arg;
    t->detached = detached;

    // Fake the stack context_switch() expects: six callee-saved registers,
    // then a return address. The new thread "returns" into thread_start.
    auto* sp = reinterpret_cast<u64*>(static_cast<u8*>(t->stack) + KERNEL_STACK_SIZE);
    *--sp = 0;                                                 // alignment slot (SysV ABI)
    *--sp = reinterpret_cast<u64>(&thread_start_trampoline);   // `ret` target
    for (int i = 0; i < 6; i++)
        *--sp = 0;                                             // rbp rbx r12 r13 r14 r15
    t->saved_rsp = reinterpret_cast<u64>(sp);

    g_threads_lock.lock();
    g_all_threads.push_back(t);
    g_thread_count++;
    g_threads_lock.unlock();
    return t;
}

} // namespace

// First code every new thread runs (reached via `ret` from context_switch).
void thread_start_trampoline() {
    finish_switch();
    arch::sti();
    Thread* t = current();
    t->entry(t->arg);
    exit(0);
}

Thread* current() {
    Thread* t;
    asm volatile("movq %%gs:8, %0" : "=r"(t));
    return t;
}

bool running() { return g_running; }

u64 thread_count() {
    LockGuard guard(g_threads_lock);
    return g_thread_count;
}

void schedule() {
    u64 flags = irq_save();
    Cpu* cpu = this_cpu();
    if (cpu->preempt_count != 0)
        panic("schedule() with preemption disabled (count %d) - sleeping while holding a spinlock?",
              cpu->preempt_count);

    RunQueue& rq = cpu->rq;
    rq.lock.lock();
    cpu->need_resched = false;

    Thread* prev = cpu->current;
    if (prev->state == ThreadState::Running) {
        // Preempted or yielding: back of the line (the idle thread never queues).
        prev->state = ThreadState::Ready;
        if (prev != rq.idle)
            rq.ready.push_back(prev);
    }
    // Blocked / Dead threads simply aren't queued. A thread that was woken
    // before it got here is already Ready and queued.

    Thread* next = rq.ready.pop_front();
    if (!next)
        next = steal_for(cpu);
    if (!next)
        next = rq.idle;

    next->state = ThreadState::Running;
    next->slice_left = TIME_SLICE_TICKS;
    if (next == prev) {
        rq.lock.unlock();
        irq_restore(flags);
        return;
    }

    __atomic_store_n(&next->on_cpu, true, __ATOMIC_RELAXED);
    next->cpu = cpu;
    next->context_switches++;
    cpu->current = next;
    cpu->switch_prev = prev;
    cpu->context_switches++;

    save_thread_state(prev);
    context_switch(prev, next);
    // We're back: some CPU (maybe a different one) switched to us.
    finish_switch();
    irq_restore(flags);
}

void wake(Thread* t) {
    u64 flags = irq_save();

    // t->cpu can change under us (stealing); lock, then confirm.
    Cpu* c;
    for (;;) {
        c = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
        c->rq.lock.lock();
        if (t->cpu == c)
            break;
        c->rq.lock.unlock();
    }

    bool queued = false;
    if (t->state == ThreadState::Blocked) {
        if (t->sleep_node.linked())
            c->rq.sleepers.remove(t);
        t->wake_deadline = 0;
        t->state = ThreadState::Ready;
        c->rq.ready.push_back(t);
        queued = true;
    }
    bool target_idle = c->current == c->rq.idle;
    c->rq.lock.unlock();

    if (queued && target_idle) {
        if (c == this_cpu())
            c->need_resched = true;
        else if (__atomic_load_n(&c->online, __ATOMIC_ACQUIRE))
            apic::send_ipi(c->lapic_id, interrupts::VEC_RESCHEDULE);
    }
    irq_restore(flags);
}

bool block_on(WaitList& list, IrqSpinlock& lock, u64 deadline_ns) {
    Thread* me = current();
    me->state = ThreadState::Blocked;
    list.push_back(me);

    if (deadline_ns) {
        RunQueue& rq = this_cpu()->rq; // interrupts are off: we can't migrate here
        rq.lock.lock();
        me->wake_deadline = deadline_ns;
        insert_sleeper(rq, me);
        rq.lock.unlock();
    }

    lock.unlock();
    schedule();
    lock.lock();

    bool timed_out = false;
    if (me->wait_node.linked()) {
        // Nobody removed us from the list: woken by the timeout (or spuriously).
        list.remove(me);
        timed_out = deadline_ns && clock::now_ns() >= deadline_ns;
    }
    if (me->sleep_node.linked()) {
        RunQueue& rq = me->cpu->rq;
        rq.lock.lock();
        if (me->sleep_node.linked())
            rq.sleepers.remove(me);
        rq.lock.unlock();
    }
    return !timed_out;
}

Thread* create(const char* name, void (*entry)(void*), void* arg, bool detached) {
    return create_thread(name, entry, arg, detached);
}

void start(Thread* t, int cpu_index) {
    if (cpu_index >= 0) {
        t->cpu = percpu::get(static_cast<u32>(cpu_index));
        if (!t->cpu)
            panic("sched: start(%s) on nonexistent CPU %d", t->name, cpu_index);
        t->pinned = true;
    } else {
        t->cpu = least_loaded_cpu();
    }
    wake(t);
}

Thread* spawn(const char* name, void (*entry)(void*), void* arg, int cpu_index, bool detached) {
    Thread* t = create_thread(name, entry, arg, detached);
    start(t, cpu_index);
    return t;
}

void exit(int code) {
    arch::cli();
    Thread* me = current();
    if (me == &this_cpu()->boot_thread)
        panic("sched::exit() called from the boot context");
    me->exit_code = code;
    me->state = ThreadState::Dead;
    schedule();
    panic("sched: dead thread %s was scheduled again", me->name);
}

int join(Thread* t) {
    KASSERT(!t->detached);
    g_threads_lock.lock();
    while (!t->reaped)
        block_on(t->join_waiters, g_threads_lock);
    g_threads_lock.unlock();
    int code = t->exit_code;
    delete t;
    return code;
}

void yield() { schedule(); }

void sleep_ns(u64 ns) {
    if (ns == 0) {
        yield();
        return;
    }
    IrqSpinlock lock;
    WaitList list;
    u64 deadline = clock::now_ns() + ns;
    lock.lock();
    while (clock::now_ns() < deadline)
        block_on(list, lock, deadline);
    lock.unlock();
}

void timer_tick(bool from_user) {
    Cpu* cpu = this_cpu();
    cpu->ticks++;
    if (!cpu->online)
        return;

    // CPU time accounting: who was running when the tick hit?
    Thread* running = cpu->current;
    if (running == cpu->rq.idle) {
        cpu->idle_ticks++;
    } else if (from_user) {
        cpu->user_ticks++;
        running->utime_ticks++;
    } else {
        cpu->system_ticks++;
        running->stime_ticks++;
    }

    u64 now = clock::now_ns();
    RunQueue& rq = cpu->rq;
    rq.lock.lock();

    // Wake threads whose timeout expired.
    while (Thread* t = rq.sleepers.front()) {
        if (t->wake_deadline > now)
            break;
        rq.sleepers.remove(t);
        t->wake_deadline = 0;
        if (t->state == ThreadState::Blocked) {
            t->state = ThreadState::Ready;
            rq.ready.push_back(t);
        }
    }

    Thread* cur = cpu->current;
    if (cur == rq.idle) {
        if (!rq.ready.empty() || (cpu->ticks % 4 == 0 && work_elsewhere(cpu)))
            cpu->need_resched = true;
    } else if (cur->slice_left > 0 && --cur->slice_left == 0) {
        if (!rq.ready.empty())
            cpu->need_resched = true;
        else
            cur->slice_left = TIME_SLICE_TICKS; // nobody waiting: keep running
    }
    rq.lock.unlock();
}

void prepare_cpu(Cpu* cpu) {
    char name[16];
    snprintf(name, sizeof(name), "idle/%u", cpu->index);
    Thread* idle = create_thread(name, idle_loop, nullptr, true);
    idle->cpu = cpu;
    idle->pinned = true;
    cpu->rq.idle = idle;
}

void init() {
    interrupts::set_handler(interrupts::VEC_TIMER, [](interrupts::Frame* f) { timer_tick(f->cs & 3); });
    interrupts::set_handler(interrupts::VEC_RESCHEDULE, [](interrupts::Frame*) { this_cpu()->need_resched = true; });
    prepare_cpu(this_cpu());
    spawn("reaper", reaper_loop, nullptr, -1, true);
}

void enter() {
    arch::cli();
    Cpu* cpu = this_cpu();
    RunQueue& rq = cpu->rq;
    rq.lock.lock();

    Thread* boot = &cpu->boot_thread;
    boot->state = ThreadState::Blocked; // the boot context never runs again

    Thread* next = rq.ready.pop_front();
    if (!next)
        next = rq.idle;
    next->state = ThreadState::Running;
    next->slice_left = TIME_SLICE_TICKS;
    __atomic_store_n(&next->on_cpu, true, __ATOMIC_RELAXED);
    next->cpu = cpu;
    cpu->current = next;
    cpu->switch_prev = boot;
    g_running = true;

    context_switch(boot, next);
    panic("sched: boot context resumed");
}

} // namespace sched

extern "C" void sched_preempt_point() {
    if (sched::running() && arch::interrupts_enabled() && cpu_local::preempt_count() == 0 && cpu_local::need_resched())
        sched::schedule();
}
