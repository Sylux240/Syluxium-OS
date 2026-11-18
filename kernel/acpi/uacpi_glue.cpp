// The kernel side of uACPI (deps/uacpi, MIT): the AML interpreter that runs
// the firmware's ACPI code. It needs memory, I/O, PCI config space, locks,
// timers, the SCI interrupt and a work queue from us.

#include <acpi/acpi.hpp>
#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/log.hpp>
#include <drivers/pci/pci.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <mm/vmalloc.hpp>
#include <mm/vmm.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>

#include <uacpi/kernel_api.h>

namespace {

// ---- SCI and other ACPI interrupts ----
struct IrqSlot {
    uacpi_interrupt_handler handler;
    uacpi_handle ctx;
};
IrqSlot g_irq[256];

void irq_trampoline(interrupts::Frame* frame) {
    IrqSlot& s = g_irq[frame->vector & 0xFF];
    if (s.handler)
        s.handler(s.ctx);
}

// ---- work queue (GPE handlers, Notify) ----
struct Work {
    uacpi_work_handler handler;
    uacpi_handle ctx;
    Work* next;
};
Work* g_work_head = nullptr;
Work* g_work_tail = nullptr;
u32 g_work_pending = 0;
IrqSpinlock g_work_lock;
sched::WaitQueue g_work_wait, g_work_done;
bool g_work_started = false;

void work_thread(void*) {
    for (;;) {
        g_work_wait.wait_until([] { return g_work_head != nullptr; });
        Work* w;
        {
            LockGuard guard(g_work_lock);
            w = g_work_head;
            if (!w)
                continue;
            g_work_head = w->next;
            if (!g_work_head)
                g_work_tail = nullptr;
        }
        w->handler(w->ctx);
        delete w;
        {
            LockGuard guard(g_work_lock);
            g_work_pending--;
        }
        g_work_done.wake_all();
    }
}

} // namespace

extern "C" {

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out) {
    *out = acpi::rsdp_phys();
    return *out ? UACPI_STATUS_OK : UACPI_STATUS_NOT_FOUND;
}

// RAM (tables) through the direct map; anything else (device registers)
// mapped uncached.
void* uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    u64 probe;
    bool direct = true;
    for (u64 p = addr & ~0xFFFull; p < addr + len && direct; p += 4096)
        direct = vmm::AddressSpace::kernel().translate(reinterpret_cast<uptr>(mm::phys_to_virt<u8>(p)), &probe);
    if (direct)
        return mm::phys_to_virt<u8>(addr);
    u64 base = addr & ~0xFFFull;
    auto* v = static_cast<u8*>(vmalloc::map_physical(base, (addr - base) + len));
    return v ? v + (addr - base) : nullptr;
}

void uacpi_kernel_unmap(void* addr, uacpi_size) {
    if (!mm::is_direct_mapped(addr))
        vmalloc::unmap_physical(reinterpret_cast<void*>(reinterpret_cast<uptr>(addr) & ~0xFFFull));
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* msg) {
    usize n = strlen(msg);
    char line[256];
    usize k = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
    memcpy(line, msg, k);
    while (k && (line[k - 1] == '\n' || line[k - 1] == '\r'))
        k--;
    line[k] = 0;
    if (level == UACPI_LOG_ERROR)
        klog::error("ACPI: %s", line);
    else if (level == UACPI_LOG_WARN)
        klog::warn("ACPI: %s", line);
    else if (level == UACPI_LOG_INFO)
        klog::info("ACPI: %s", line);
    // debug/trace: silent
}

uacpi_status uacpi_kernel_initialize(uacpi_init_level) { return UACPI_STATUS_OK; }
void uacpi_kernel_deinitialize(void) {}

// ---- PCI ----
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle* out) {
    volatile u8* cfg = pci::config_space(a.segment, a.bus, a.device, a.function);
    if (!cfg)
        return UACPI_STATUS_NOT_FOUND;
    *out = const_cast<u8*>(cfg);
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle) {}
uacpi_status uacpi_kernel_pci_read8(uacpi_handle d, uacpi_size off, uacpi_u8* v) {
    *v = *(static_cast<volatile u8*>(d) + off);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read16(uacpi_handle d, uacpi_size off, uacpi_u16* v) {
    *v = *reinterpret_cast<volatile u16*>(static_cast<volatile u8*>(d) + off);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read32(uacpi_handle d, uacpi_size off, uacpi_u32* v) {
    *v = *reinterpret_cast<volatile u32*>(static_cast<volatile u8*>(d) + off);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write8(uacpi_handle d, uacpi_size off, uacpi_u8 v) {
    *(static_cast<volatile u8*>(d) + off) = v;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write16(uacpi_handle d, uacpi_size off, uacpi_u16 v) {
    *reinterpret_cast<volatile u16*>(static_cast<volatile u8*>(d) + off) = v;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write32(uacpi_handle d, uacpi_size off, uacpi_u32 v) {
    *reinterpret_cast<volatile u32*>(static_cast<volatile u8*>(d) + off) = v;
    return UACPI_STATUS_OK;
}

// ---- I/O ports (the handle is the base port) ----
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size, uacpi_handle* out) {
    *out = reinterpret_cast<uacpi_handle>(static_cast<uptr>(base));
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle) {}
static u16 port(uacpi_handle h, uacpi_size off) { return static_cast<u16>(reinterpret_cast<uptr>(h) + off); }
uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size off, uacpi_u8* v) {
    *v = arch::inb(port(h, off));
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size off, uacpi_u16* v) {
    *v = arch::inw(port(h, off));
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size off, uacpi_u32* v) {
    u32 val;
    asm volatile("inl %1, %0" : "=a"(val) : "Nd"(port(h, off)));
    *v = val;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v) {
    arch::outb(port(h, off), v);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v) {
    arch::outw(port(h, off), v);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v) {
    asm volatile("outl %0, %1" : : "a"(v), "Nd"(port(h, off)));
    return UACPI_STATUS_OK;
}

// ---- MMIO ----
uacpi_u8 uacpi_kernel_mmio_read8(void* a) { return *static_cast<volatile u8*>(a); }
uacpi_u16 uacpi_kernel_mmio_read16(void* a) { return *static_cast<volatile u16*>(a); }
uacpi_u32 uacpi_kernel_mmio_read32(void* a) { return *static_cast<volatile u32*>(a); }
uacpi_u64 uacpi_kernel_mmio_read64(void* a) { return *static_cast<volatile u64*>(a); }
void uacpi_kernel_mmio_write8(void* a, uacpi_u8 v) { *static_cast<volatile u8*>(a) = v; }
void uacpi_kernel_mmio_write16(void* a, uacpi_u16 v) { *static_cast<volatile u16*>(a) = v; }
void uacpi_kernel_mmio_write32(void* a, uacpi_u32 v) { *static_cast<volatile u32*>(a) = v; }
void uacpi_kernel_mmio_write64(void* a, uacpi_u64 v) { *static_cast<volatile u64*>(a) = v; }

// ---- memory ----
void* uacpi_kernel_alloc(uacpi_size size) { return kmalloc(size ? size : 1); }
void* uacpi_kernel_alloc_zeroed(uacpi_size size) { return kzalloc(size ? size : 1); }
void uacpi_kernel_free(void* mem) { kfree(mem); }

// ---- time ----
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return clock::now_ns(); }
void uacpi_kernel_stall(uacpi_u8 usec) {
    u64 until = clock::now_ns() + static_cast<u64>(usec) * 1000;
    while (clock::now_ns() < until)
        asm volatile("pause");
}
void uacpi_kernel_sleep(uacpi_u64 msec) { sched::sleep_ms(msec); }

// ---- synchronization ----
uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 initial) { return new sched::Semaphore(initial); }
void uacpi_kernel_free_semaphore(uacpi_handle h) { delete static_cast<sched::Semaphore*>(h); }
uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle h, uacpi_u16 timeout_ms) {
    auto* s = static_cast<sched::Semaphore*>(h);
    if (timeout_ms == 0xFFFF) {
        s->wait();
        return UACPI_STATUS_OK;
    }
    if (timeout_ms == 0)
        return s->try_wait() ? UACPI_STATUS_OK : UACPI_STATUS_TIMEOUT;
    return s->wait_timeout(static_cast<u64>(timeout_ms) * 1'000'000) ? UACPI_STATUS_OK : UACPI_STATUS_TIMEOUT;
}
void uacpi_kernel_signal_semaphore(uacpi_handle h) { static_cast<sched::Semaphore*>(h)->signal(); }

uacpi_thread_id uacpi_kernel_get_thread_id(void) { return sched::current(); }

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    u64 flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    if (state & (1u << 9))
        arch::sti();
}

uacpi_handle uacpi_kernel_create_spinlock(void) { return new IrqSpinlock; }
void uacpi_kernel_free_spinlock(uacpi_handle h) { delete static_cast<IrqSpinlock*>(h); }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) {
    static_cast<IrqSpinlock*>(h)->lock();
    return 0;
}
void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags) { static_cast<IrqSpinlock*>(h)->unlock(); }

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* r) {
    if (r->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        klog::error("ACPI: firmware reported a fatal error (type %u, code %#x)", r->fatal.type, r->fatal.code);
    return UACPI_STATUS_OK;
}

// SCI: level-triggered, active low (unless the MADT overrides say otherwise).
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
                                                     uacpi_handle* out) {
    u8 vector = interrupts::allocate_vector();
    if (!vector)
        return UACPI_STATUS_INTERNAL_ERROR;
    g_irq[vector] = IrqSlot{handler, ctx};
    interrupts::set_handler(vector, irq_trampoline);
    u32 gsi = irq;
    bool level = true, active_low = true;
    const acpi::MadtInfo& m = acpi::madt();
    for (u32 i = 0; i < m.override_count; i++) {
        if (m.overrides[i].isa_irq == irq) {
            gsi = m.overrides[i].gsi;
            level = m.overrides[i].level_triggered || irq == 9;
            active_low = m.overrides[i].active_low || irq == 9;
        }
    }
    if (!ioapic::route_gsi(gsi, vector, this_cpu()->lapic_id, level, active_low))
        return UACPI_STATUS_INTERNAL_ERROR;
    *out = reinterpret_cast<uacpi_handle>(static_cast<uptr>(vector));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler, uacpi_handle h) {
    g_irq[reinterpret_cast<uptr>(h) & 0xFF] = IrqSlot{nullptr, nullptr};
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_work_item(void) { return reinterpret_cast<uacpi_handle>(1); }
void uacpi_kernel_free_work_item(uacpi_handle) {}

void uacpi_kernel_schedule_work(uacpi_work_type, uacpi_handle, uacpi_work_handler handler, uacpi_handle ctx) {
    if (!__atomic_exchange_n(&g_work_started, true, __ATOMIC_ACQ_REL))
        sched::spawn("acpi-work", work_thread, nullptr, -1, true);
    auto* w = new Work{handler, ctx, nullptr};
    {
        LockGuard guard(g_work_lock);
        if (g_work_tail)
            g_work_tail->next = w;
        else
            g_work_head = w;
        g_work_tail = w;
        g_work_pending++;
    }
    g_work_wait.wake_all();
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    g_work_done.wait_until([] { return g_work_pending == 0; });
    return UACPI_STATUS_OK;
}

} // extern "C"
