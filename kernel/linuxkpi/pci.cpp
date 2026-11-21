// LinuxKPI device glue: PCI drivers on top of Syluxium's device registry,
// interrupts (MSI/MSI-X vectors), DMA, MMIO mappings and firmware loading.

#include <arch/x86_64/interrupts.hpp>
#include <boot/limine_requests.hpp>
#include <lib/string.hpp>
#include <core/log.hpp>
#include <cxx/runtime.hpp>
#include <dev/device.hpp>
#include <drivers/pci/pci.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <linuxkpi/linuxkpi.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <mm/vmalloc.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>

namespace {

u64 sylux_vmalloc_bytes(usize n, void** out) {
    *out = vmalloc::alloc(n ? n : 1);
    return *out ? n : 0;
}
void sylux_vfree(void* p) { vmalloc::free(p); }
void* sylux_map_physical(u64 phys, usize size) { return vmalloc::map_physical(phys, size); }
void sylux_unmap_physical(void* v) { vmalloc::unmap_physical(v); }

} // namespace

// struct pci_dev and pci_device_id have a field named `class`.
#define class lkpi_class
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#undef class
#undef min
#undef max
#undef abs
#undef swap
#undef clamp

namespace {

// ---- interrupts ----

struct IrqAction {
    irq_handler_t handler = nullptr;
    irq_handler_t thread_fn = nullptr;
    void* dev_id = nullptr;
    const char* name = nullptr;
    bool disabled = false;
    sched::Semaphore* thread_wake = nullptr;
    volatile bool in_handler = false;
};

// irq numbers 1..255 are real interrupt vectors (MSI/MSI-X); numbers from
// POLLED_BASE up are devices with only legacy INTx, whose handlers a 1 ms
// poller calls (until ACPI _PRT interrupt routing exists).
constexpr unsigned POLLED_BASE = 256, MAX_IRQ = 320;
IrqAction g_irqs[MAX_IRQ];
unsigned g_next_polled = POLLED_BASE;
volatile int g_irq_depth = 0;

void irq_poller(void*) {
    for (;;) {
        sched::sleep_ms(1);
        for (unsigned irq = POLLED_BASE; irq < g_next_polled; irq++) {
            IrqAction& a = g_irqs[irq];
            if (!a.handler || a.disabled)
                continue;
            a.in_handler = true;
            irqreturn_t r = a.handler(static_cast<int>(irq), a.dev_id);
            a.in_handler = false;
            if (r == IRQ_WAKE_THREAD && a.thread_wake)
                a.thread_wake->signal();
        }
    }
}

void irq_trampoline(interrupts::Frame* frame) {
    u8 vector = static_cast<u8>(frame->vector);
    IrqAction& a = g_irqs[vector];
    if (!a.handler || a.disabled)
        return;
    __atomic_add_fetch(&g_irq_depth, 1, __ATOMIC_RELAXED);
    a.in_handler = true;
    irqreturn_t r = a.handler(vector, a.dev_id);
    a.in_handler = false;
    __atomic_sub_fetch(&g_irq_depth, 1, __ATOMIC_RELAXED);
    if (r == IRQ_WAKE_THREAD && a.thread_wake)
        a.thread_wake->signal();
}

void irq_thread(void* arg) {
    auto vector = static_cast<u8>(reinterpret_cast<uptr>(arg));
    IrqAction& a = g_irqs[vector];
    for (;;) {
        a.thread_wake->wait();
        if (a.thread_fn)
            a.thread_fn(vector, a.dev_id);
    }
}

irqreturn_t default_hardirq(int, void*) { return IRQ_WAKE_THREAD; }

// Give a legacy-only device a polled irq number (once).
void setup_polled(struct pci_dev* pdev) {
    if (pdev->irq || g_next_polled >= MAX_IRQ)
        return;
    static bool started = false;
    if (!started) {
        started = true;
        sched::spawn("lkpi-irqpoll", irq_poller, nullptr, -1, true);
    }
    pdev->irq = g_next_polled++;
    dev_info(&pdev->dev, "no MSI: interrupt handler polled every 1 ms (irq %u)", pdev->irq);
}

// Allocate an MSI/MSI-X vector for the device (once).
int setup_msi(struct pci_dev* pdev) {
    if (pdev->irq)
        return 0;
    auto* f = static_cast<pci::Function*>(pdev->lkpi_function);
    if (!f->msi_cap && !f->msix_cap)
        return -ENOSPC; // legacy INTx only
    u8 vector = interrupts::allocate_vector();
    interrupts::set_handler(vector, irq_trampoline);
    if (!pci::enable_msi(*f, vector))
        return -EIO;
    pdev->irq = vector;
    pdev->msix_enabled = f->msix_cap != 0;
    pdev->msi_enabled = !pdev->msix_enabled;
    return 0;
}

// ---- PCI drivers ----

bool id_matches(const pci_device_id* id, const dev::Device* d) {
    if (id->vendor != PCI_ANY_ID && id->vendor != d->vendor) return false;
    if (id->device != PCI_ANY_ID && id->device != d->device) return false;
    if (id->subvendor != PCI_ANY_ID && id->subvendor != d->subsystem_vendor) return false;
    if (id->subdevice != PCI_ANY_ID && id->subdevice != d->subsystem_device) return false;
    return ((id->lkpi_class ^ d->class_code) & id->class_mask) == 0;
}

bool is_terminator(const pci_device_id* id) {
    return !id->vendor && !id->device && !id->subvendor && !id->subdevice && !id->lkpi_class && !id->class_mask;
}

const pci_device_id* find_id(const pci_driver* drv, const dev::Device* d) {
    for (const pci_device_id* id = drv->id_table; id && !is_terminator(id); id++)
        if (id_matches(id, d))
            return id;
    return nullptr;
}

// A whole word on the kernel command line ("gpu" but not "nogpu").
bool cmdline_word(const char* w) {
    usize n = strlen(w);
    for (const char* p = boot::cmdline(); (p = strstr(p, w)); p += n) {
        bool start = p == boot::cmdline() || p[-1] == ' ';
        bool end = p[n] == 0 || p[n] == ' ';
        if (start && end)
            return true;
    }
    return false;
}

bool lkpi_match(const dev::Driver* drv, const dev::Device* d) {
    if (d->bus != dev::Bus::Pci || !find_id(static_cast<const pci_driver*>(drv->ctx), d))
        return false;
    // Real GPU drivers (i915, ...) are opt-in until they are proven on real
    // hardware: without "gpu" on the command line the firmware framebuffer
    // (simpledrm) drives the display. A black screen on a ThinkPad (UHD 620)
    // with i915 bound by default is why. "nogpu" still forces them off.
    if ((d->class_code >> 16) == 0x03 && !cmdline_word("gpu")) {
        static bool told;
        if (!told)
            klog::info("linuxkpi: GPU drivers off (boot option \"gpu\" turns them on)");
        told = true;
        return false;
    }
    if ((d->class_code >> 16) == 0x03 && strstr(boot::cmdline(), "nogpu")) {
        static bool said;
        if (!said)
            klog::info("linuxkpi: nogpu: not binding GPU drivers (firmware framebuffer only)");
        said = true;
        return false;
    }
    // Virtual-machine display drivers are opt-in ("bochsdrm"): the firmware
    // framebuffer serves VMs well, and they are mainly a test bed for the
    // GPU driver infrastructure (TTM, display handover) in QEMU.
    const char* name = static_cast<const pci_driver*>(drv->ctx)->name;
    if (name && !strcmp(name, "bochs-drm") && !strstr(boot::cmdline(), "bochsdrm"))
        return false;
    return true;
}

extern "C" void lkpi_gpu_probe_failed(const char* driver);
extern "C" int aperture_remove_conflicting_pci_devices(struct pci_dev* pdev, const char* name);

// A pci_dev for function `f` (bound or not: drivers also look at bridges
// and sibling functions).
void fill_pdev(pci_dev* pdev, pci::Function* f, dev::Device* d) {
    pdev->vendor = f->vendor_id;
    pdev->device = f->device_id;
    pdev->subsystem_vendor = d ? d->subsystem_vendor : 0;
    pdev->subsystem_device = d ? d->subsystem_device : 0;
    pdev->lkpi_class = (static_cast<u32>(f->class_code) << 16) | (static_cast<u32>(f->subclass) << 8) | f->prog_if;
    pdev->revision = f->revision;
    pdev->pin = f->irq_pin;
    pdev->bus_number = f->bus;
    {
        // One pci_bus per bus number (DRM prints pdev->bus->number).
        static struct pci_bus buses[256];
        buses[f->bus & 0xff].number = static_cast<unsigned char>(f->bus);
        pdev->bus = &buses[f->bus & 0xff];
    }
    pdev->devfn = static_cast<unsigned>((f->device << 3) | f->function);
    pdev->lkpi_function = f;
    pdev->dev.lkpi_registry = d;
    if (d)
        snprintf(pdev->dev.lkpi_name, sizeof(pdev->dev.lkpi_name), "%s", d->bus_id);
    else
        snprintf(pdev->dev.lkpi_name, sizeof(pdev->dev.lkpi_name), "%04x:%02x:%02x.%u", f->segment, f->bus, f->device,
                 f->function);
    pdev->dev.bus = &lkpi_pci_bus_type; // dev_is_pci()
    pdev->dev.dma_mask_storage = DMA_BIT_MASK(32); // Linux's default until the driver sets one
    pdev->dev.dma_mask = &pdev->dev.dma_mask_storage;
    pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
    for (int i = 0; i < DEVICE_COUNT_RESOURCE; i++) {
        const pci::Bar& b = f->bars[i];
        pdev->lkpi_res_start[i] = b.address;
        pdev->lkpi_res_len[i] = b.size;
        if (b.size)
            pdev->lkpi_res_flags[i] = (b.is_io ? IORESOURCE_IO : IORESOURCE_MEM) |
                                      (b.prefetchable ? IORESOURCE_PREFETCH : 0) | (b.is_64bit ? IORESOURCE_MEM_64 : 0);
        pdev->resource[i].start = b.address;
        pdev->resource[i].end = b.size ? b.address + b.size - 1 : 0;
        pdev->resource[i].flags = pdev->lkpi_res_flags[i];
        pdev->resource[i].name = pdev->dev.lkpi_name;
    }
}

// ---- GPU driver probes: watched, so a hanging driver can't stop the boot ----
// "gputimeout=<seconds>" (default 90): how long a GPU driver may take to probe.
// "gpufail=<step>": i915 fails that probe step (1..9, see i915_driver.c);
// "gpufail=takeover" / "gpufail=hang": any GPU driver takes the display over,
// then fails / hangs (tests the fallback and the watchdog in QEMU).
long cmdline_number(const char* key, long fallback) {
    const char* p = strstr(boot::cmdline(), key);
    if (!p || (p != boot::cmdline() && p[-1] != ' '))
        return fallback;
    p += strlen(key);
    if (*p < '0' || *p > '9')
        return fallback;
    long v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    return v;
}

struct GpuProbe {
    pci_driver* drv;
    pci_dev* pdev;
    const pci_device_id* id;
    int result;
    sched::Semaphore done{0};
};

void gpu_probe_thread(void* arg) {
    auto* job = static_cast<GpuProbe*>(arg);
    if (cmdline_word("gpufail=takeover") || cmdline_word("gpufail=hang")) {
        aperture_remove_conflicting_pci_devices(job->pdev, job->drv->name);
        if (cmdline_word("gpufail=hang")) {
            klog::warn("linuxkpi: gpufail=hang: %s probe hangs on purpose", job->drv->name);
            for (;;)
                sched::sleep_ms(1000);
        }
        klog::warn("linuxkpi: gpufail=takeover: %s probe fails on purpose", job->drv->name);
        job->result = -EIO;
    } else {
        job->result = job->drv->probe(job->pdev, job->id);
    }
    job->done.signal();
}

int gpu_probe(pci_driver* drv, pci_dev* pdev, const pci_device_id* id) {
    long timeout = cmdline_number("gputimeout=", 90);
    auto* job = new GpuProbe{drv, pdev, id, 0};
    klog::info("linuxkpi: %s: probing %s (watchdog %ld s)", drv->name, pdev->dev.lkpi_name, timeout);
    u64 start = clock::now_ms();
    sched::spawn("gpu-probe", gpu_probe_thread, job, -1, true);
    if (!job->done.wait_timeout(static_cast<u64>(timeout) * 1'000'000'000ull)) {
        klog::error("linuxkpi: %s: probe still running after %ld s - giving up on it (boot continues on the "
                    "firmware framebuffer; boot with nogpu to skip GPU drivers)", drv->name, timeout);
        return -ETIMEDOUT; // job leaks: the thread may still finish and touch it
    }
    int r = job->result;
    delete job;
    if (r)
        klog::error("linuxkpi: %s: probe failed (%d) after %lu ms", drv->name, r, clock::now_ms() - start);
    else
        klog::ok("linuxkpi: %s: probe finished in %lu ms", drv->name, clock::now_ms() - start);
    return r;
}

// pci_devs handed out for functions no LinuxKPI driver is bound to, one per function.
pci_dev* g_shadow[256];
pci_dev* g_bound[256]; // LinuxKPI drivers' pci_devs, by function index

void set_bound(pci::Function* f, pci_dev* pdev) {
    for (u32 i = 0; i < pci::count() && i < 256; i++)
        if (pci::function_at(i) == f)
            g_bound[i] = pdev;
}

long lkpi_probe(const dev::Driver* drv, dev::Device* d) {
    auto* pdrv = static_cast<pci_driver*>(drv->ctx);
    pci::Function* f = pci::function_of(d);
    auto* pdev = static_cast<pci_dev*>(kzalloc(sizeof(pci_dev), GFP_KERNEL));
    if (!pdev)
        return -ENOMEM;
    fill_pdev(pdev, f, d);
    pdev->driver = pdrv;
    pdev->dev.driver = &pdrv->driver;
    d->driver_data = pdev;
    set_bound(f, pdev);
    bool gpu = (d->class_code >> 16) == 0x03;
    int r = gpu ? gpu_probe(pdrv, pdev, find_id(pdrv, d)) : pdrv->probe(pdev, find_id(pdrv, d));
    if (r == -ETIMEDOUT && gpu) {
        // The probe thread is stuck and still uses pdev: keep it, but give
        // the screen back to the firmware framebuffer and boot on.
        d->driver_data = nullptr;
        set_bound(f, nullptr);
        lkpi_gpu_probe_failed(pdrv->name);
        return r;
    }
    if (r) {
        d->driver_data = nullptr;
        set_bound(f, nullptr);
        if ((d->class_code >> 16) == 0x03)
            lkpi_gpu_probe_failed(pdrv->name);
        kfree(pdev);
        return r;
    }
    return 0;
}

pci::Function& fn(const struct pci_dev* p) { return *static_cast<pci::Function*>(p->lkpi_function); }

} // namespace

extern "C" {

int lkpi_in_interrupt(void) { return __atomic_load_n(&g_irq_depth, __ATOMIC_RELAXED) > 0; }

int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn, unsigned long,
                         const char* name, void* dev_id) {
    if (irq == 0 || irq >= MAX_IRQ)
        return -EINVAL;
    IrqAction& a = g_irqs[irq];
    if (a.handler)
        return -EBUSY; // no IRQ sharing on MSI vectors
    a.dev_id = dev_id;
    a.name = name;
    a.thread_fn = thread_fn;
    a.disabled = false;
    if (thread_fn && !a.thread_wake) {
        a.thread_wake = new sched::Semaphore(0);
        char tname[32];
        snprintf(tname, sizeof(tname), "irq/%u-%s", irq, name ? name : "lkpi");
        sched::spawn(tname, irq_thread, reinterpret_cast<void*>(static_cast<uptr>(irq)), -1, true);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    a.handler = handler ? handler : default_hardirq;
    return 0;
}

int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags, const char* name, void* dev_id) {
    return request_threaded_irq(irq, handler, nullptr, flags, name, dev_id);
}

void free_irq(unsigned int irq, void*) {
    if (irq >= MAX_IRQ)
        return;
    g_irqs[irq].handler = nullptr;
    synchronize_irq(irq);
}

void disable_irq_nosync(unsigned int irq) {
    if (irq < MAX_IRQ)
        g_irqs[irq].disabled = true;
}
void disable_irq(unsigned int irq) {
    disable_irq_nosync(irq);
    synchronize_irq(irq);
}
void enable_irq(unsigned int irq) {
    if (irq < MAX_IRQ)
        g_irqs[irq].disabled = false;
}
void synchronize_irq(unsigned int irq) {
    if (irq >= MAX_IRQ)
        return;
    while (g_irqs[irq].in_handler)
        __builtin_ia32_pause();
}

// Tasklets: glue/wait.c.

// ---- PCI ----

int pci_register_driver(struct pci_driver* drv) {
    auto* d = new dev::Driver{drv->name, nullptr, nullptr, {}};
    d->match_ex = lkpi_match;
    d->probe_ex = lkpi_probe;
    d->ctx = drv;
    drv->lkpi = d;
    drv->driver.name = drv->name;
    dev::register_driver(d);
    return 0;
}

// The pci_dev of a function: the bound driver's, else a shadow one.
static pci_dev* pdev_of_index(u32 i) {
    pci::Function* f = pci::function_at(i);
    if (!f)
        return nullptr;
    dev::Device* d = f->entry;
    if (i < 256 && g_bound[i])
        return g_bound[i];
    if (i >= 256)
        return nullptr;
    if (!g_shadow[i]) {
        auto* pdev = static_cast<pci_dev*>(kzalloc(sizeof(pci_dev), GFP_KERNEL));
        if (!pdev)
            return nullptr;
        fill_pdev(pdev, f, d);
        g_shadow[i] = pdev;
    }
    return g_shadow[i];
}

struct pci_dev* pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn) {
    for (u32 i = 0; i < pci::count(); i++) {
        pci::Function* f = pci::function_at(i);
        if (f->segment == domain && f->bus == bus && ((f->device << 3) | f->function) == devfn)
            return pdev_of_index(i);
    }
    return nullptr;
}

// Iteration in enumeration order, starting after `from`.
static u32 index_after(struct pci_dev* from) {
    if (!from)
        return 0;
    for (u32 i = 0; i < pci::count(); i++)
        if (pci::function_at(i) == from->lkpi_function)
            return i + 1;
    return pci::count();
}

struct pci_dev* pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev* from) {
    for (u32 i = index_after(from); i < pci::count(); i++) {
        pci::Function* f = pci::function_at(i);
        if ((vendor == PCI_ANY_ID || f->vendor_id == vendor) && (device == PCI_ANY_ID || f->device_id == device))
            return pdev_of_index(i);
    }
    return nullptr;
}

struct pci_dev* pci_get_class(unsigned int cls, struct pci_dev* from) {
    for (u32 i = index_after(from); i < pci::count(); i++) {
        pci::Function* f = pci::function_at(i);
        u32 c = (static_cast<u32>(f->class_code) << 16) | (static_cast<u32>(f->subclass) << 8) | f->prog_if;
        if (c == cls)
            return pdev_of_index(i);
    }
    return nullptr;
}

int pci_dev_present(const struct pci_device_id* ids) {
    for (const pci_device_id* id = ids; id && !is_terminator(id); id++)
        for (u32 i = 0; i < pci::count(); i++) {
            pci::Function* f = pci::function_at(i);
            if (f->entry && id_matches(id, f->entry))
                return 1;
        }
    return 0;
}

int pci_bus_read_config_dword(struct pci_bus* bus, unsigned int devfn, int where, u32* val) {
    volatile u8* cfg = pci::config_space(0, bus->number, static_cast<u8>(devfn >> 3), static_cast<u8>(devfn & 7));
    *val = cfg ? *reinterpret_cast<volatile u32*>(cfg + (where & ~3)) : 0xffffffff;
    return cfg ? 0 : -ENODEV;
}
int pci_bus_read_config_word(struct pci_bus* bus, unsigned int devfn, int where, u16* val) {
    volatile u8* cfg = pci::config_space(0, bus->number, static_cast<u8>(devfn >> 3), static_cast<u8>(devfn & 7));
    *val = cfg ? *reinterpret_cast<volatile u16*>(cfg + (where & ~1)) : 0xffff;
    return cfg ? 0 : -ENODEV;
}
int pci_bus_read_config_byte(struct pci_bus* bus, unsigned int devfn, int where, u8* val) {
    volatile u8* cfg = pci::config_space(0, bus->number, static_cast<u8>(devfn >> 3), static_cast<u8>(devfn & 7));
    *val = cfg ? cfg[where] : 0xff;
    return cfg ? 0 : -ENODEV;
}
int pci_bus_write_config_dword(struct pci_bus* bus, unsigned int devfn, int where, u32 val) {
    volatile u8* cfg = pci::config_space(0, bus->number, static_cast<u8>(devfn >> 3), static_cast<u8>(devfn & 7));
    if (cfg)
        *reinterpret_cast<volatile u32*>(cfg + (where & ~3)) = val;
    return cfg ? 0 : -ENODEV;
}
int pci_bus_write_config_word(struct pci_bus* bus, unsigned int devfn, int where, u16 val) {
    volatile u8* cfg = pci::config_space(0, bus->number, static_cast<u8>(devfn >> 3), static_cast<u8>(devfn & 7));
    if (cfg)
        *reinterpret_cast<volatile u16*>(cfg + (where & ~1)) = val;
    return cfg ? 0 : -ENODEV;
}
int pci_bus_write_config_byte(struct pci_bus* bus, unsigned int devfn, int where, u8 val) {
    volatile u8* cfg = pci::config_space(0, bus->number, static_cast<u8>(devfn >> 3), static_cast<u8>(devfn & 7));
    if (cfg)
        cfg[where] = val;
    return cfg ? 0 : -ENODEV;
}

// The bridge above a device: the PCI(e) bridge whose secondary..subordinate
// bus range holds the device's bus (none for devices on bus 0).
struct pci_dev* pci_upstream_bridge(struct pci_dev* dev) {
    if (!dev || dev->bus_number == 0)
        return nullptr;
    for (u32 i = 0; i < pci::count(); i++) {
        pci::Function* f = pci::function_at(i);
        if ((f->header_type & 0x7f) != 1)
            continue;
        u8 secondary = pci::read8(*f, 0x19);
        if (secondary == dev->bus_number)
            return pdev_of_index(i);
    }
    return nullptr;
}

// Expansion ROM (the video BIOS of discrete GPUs): enable the ROM BAR,
// map it, and return the image size from its headers.
void __iomem* pci_map_rom(struct pci_dev* pdev, size_t* size) {
    pci::Function& f = fn(pdev);
    u32 rom = pci::read32(f, 0x30);
    if ((f.header_type & 0x7f) != 0)
        return nullptr;
    pci::write32(f, 0x30, 0xfffff800);
    u32 mask = pci::read32(f, 0x30) & 0xfffff800;
    pci::write32(f, 0x30, rom);
    if (!mask || !(rom & 0xfffff800))
        return nullptr;
    u64 len = (~static_cast<u64>(mask) + 1) & 0xffffffff;
    pci::write32(f, 0x30, rom | 1); // enable decoding
    pci::write16(f, PCI_COMMAND, pci::read16(f, PCI_COMMAND) | PCI_COMMAND_MEMORY);
    auto* v = static_cast<u8*>(ioremap(rom & 0xfffff800, len));
    if (!v)
        return nullptr;
    // Walk the PCI data structures to find the end of the last image.
    u64 off = 0, end = 0;
    while (off + 0x1a < len && v[off] == 0x55 && v[off + 1] == 0xaa) {
        u16 pds = static_cast<u16>(v[off + 0x18] | (v[off + 0x19] << 8));
        if (off + pds + 0x18 > len || v[off + pds] != 'P' || v[off + pds + 1] != 'C' || v[off + pds + 2] != 'I' ||
            v[off + pds + 3] != 'R')
            break;
        u64 image = static_cast<u64>(v[off + pds + 0x10] | (v[off + pds + 0x11] << 8)) * 512;
        end = off + image;
        if (v[off + pds + 0x15] & 0x80 || !image)
            break;
        off += image;
    }
    if (!end) {
        iounmap(v);
        pci::write32(f, 0x30, rom);
        return nullptr;
    }
    *size = end;
    return v;
}
void pci_unmap_rom(struct pci_dev* pdev, void __iomem* rom) {
    iounmap(rom);
    pci::write32(fn(pdev), 0x30, pci::read32(fn(pdev), 0x30) & ~1u);
}

void pci_unregister_driver(struct pci_driver*) {
    // Built-in drivers stay registered; unbinding comes with hot-unplug support.
}

// Like Linux, pdev->irq is usable right after enabling: MSI if the device
// has it, else a polled legacy interrupt.
int pci_enable_device(struct pci_dev* p) {
    pci::enable(fn(p));
    if (!p->irq && setup_msi(p) != 0)
        setup_polled(p);
    return 0;
}
void pci_disable_device(struct pci_dev* p) { pci::write16(fn(p), PCI_COMMAND, pci::read16(fn(p), PCI_COMMAND) & ~0x7); }
void pci_set_master(struct pci_dev* p) { pci::write16(fn(p), PCI_COMMAND, pci::read16(fn(p), PCI_COMMAND) | PCI_COMMAND_MASTER); }
void pci_clear_master(struct pci_dev* p) { pci::write16(fn(p), PCI_COMMAND, pci::read16(fn(p), PCI_COMMAND) & ~PCI_COMMAND_MASTER); }
int pci_request_regions(struct pci_dev*, const char*) { return 0; }
void pci_release_regions(struct pci_dev*) {}

void __iomem* pci_iomap(struct pci_dev* p, int bar, unsigned long maxlen) {
    if (bar < 0 || bar >= DEVICE_COUNT_RESOURCE || !p->lkpi_res_len[bar])
        return nullptr;
    if (p->lkpi_res_flags[bar] & IORESOURCE_IO)
        return nullptr; // port I/O BARs: drivers must use inb/outb with pci_resource_start
    // Part of a BAR (GPUs: i915 maps only the register half of BAR0, the
    // other half (the GTT) is mapped write-combining elsewhere; mapping it
    // uncached too would alias two memory types).
    if (maxlen && maxlen < p->lkpi_res_len[bar])
        return ioremap(p->lkpi_res_start[bar], maxlen);
    if (!p->lkpi_iomap[bar])
        p->lkpi_iomap[bar] = const_cast<void*>(reinterpret_cast<volatile void*>(pci::map_bar(fn(p), bar)));
    return p->lkpi_iomap[bar];
}
void pci_iounmap(struct pci_dev* p, void __iomem* addr) {
    for (int i = 0; i < DEVICE_COUNT_RESOURCE; i++)
        if (p->lkpi_iomap[i] == addr)
            return; // whole-BAR mappings are permanent
    iounmap(addr); // partial ones (pci_iomap with maxlen) are not
}

int pcim_iomap_regions(struct pci_dev* p, int mask, const char*) {
    for (int i = 0; i < DEVICE_COUNT_RESOURCE; i++)
        if ((mask & (1 << i)) && !pci_iomap(p, i, 0))
            return -ENOMEM;
    return 0;
}
void __iomem* const* pcim_iomap_table(struct pci_dev* p) { return p->lkpi_iomap; }

int pci_read_config_byte(const struct pci_dev* p, int where, u8* v) { *v = pci::read8(fn(p), static_cast<u16>(where)); return 0; }
int pci_read_config_word(const struct pci_dev* p, int where, u16* v) { *v = pci::read16(fn(p), static_cast<u16>(where)); return 0; }
int pci_read_config_dword(const struct pci_dev* p, int where, u32* v) { *v = pci::read32(fn(p), static_cast<u16>(where)); return 0; }
int pci_write_config_byte(const struct pci_dev* p, int where, u8 v) { pci::write8(fn(p), static_cast<u16>(where), v); return 0; }
int pci_write_config_word(const struct pci_dev* p, int where, u16 v) { pci::write16(fn(p), static_cast<u16>(where), v); return 0; }
int pci_write_config_dword(const struct pci_dev* p, int where, u32 v) { pci::write32(fn(p), static_cast<u16>(where), v); return 0; }

int pci_find_capability(struct pci_dev* p, int cap) {
    if (!(pci::read16(fn(p), PCI_STATUS) & 0x10))
        return 0;
    u8 pos = pci::read8(fn(p), PCI_CAPABILITY_LIST) & 0xFC;
    for (int guard = 0; pos && guard < 48; guard++) {
        if (pci::read8(fn(p), pos) == cap)
            return pos;
        pos = pci::read8(fn(p), static_cast<u16>(pos + 1)) & 0xFC;
    }
    return 0;
}

int pci_find_ext_capability(struct pci_dev* p, int cap) {
    u16 pos = 0x100;
    for (int guard = 0; pos && guard < 64; guard++) {
        u32 h = pci::read32(fn(p), pos);
        if (!h || h == 0xFFFFFFFF)
            return 0;
        if ((h & 0xFFFF) == static_cast<u32>(cap))
            return pos;
        pos = static_cast<u16>((h >> 20) & 0xFFC);
    }
    return 0;
}

int pcie_capability_read_word(struct pci_dev* p, int pos, u16* v) {
    *v = 0;
    if (!fn(p).pcie_cap)
        return -EINVAL;
    *v = pci::read16(fn(p), static_cast<u16>(fn(p).pcie_cap + pos));
    return 0;
}
int pcie_capability_write_word(struct pci_dev* p, int pos, u16 v) {
    if (!fn(p).pcie_cap)
        return -EINVAL;
    pci::write16(fn(p), static_cast<u16>(fn(p).pcie_cap + pos), v);
    return 0;
}
int pcie_capability_clear_and_set_word(struct pci_dev* p, int pos, u16 clear, u16 set) {
    u16 v;
    if (int r = pcie_capability_read_word(p, pos, &v))
        return r;
    return pcie_capability_write_word(p, pos, static_cast<u16>((v & ~clear) | set));
}

int pci_alloc_irq_vectors(struct pci_dev* p, unsigned int min_vecs, unsigned int, unsigned int flags) {
    if (min_vecs > 1)
        return -ENOSPC; // one vector per device for now
    if (p->irq)
        return 1;
    if ((flags & (PCI_IRQ_MSI | PCI_IRQ_MSIX)) && setup_msi(p) == 0)
        return 1;
    if (flags & PCI_IRQ_LEGACY) {
        setup_polled(p);
        return p->irq ? 1 : -ENOSPC;
    }
    return -ENOSPC;
}
void pci_free_irq_vectors(struct pci_dev*) {}
int pci_irq_vector(struct pci_dev* p, unsigned int nr) { return nr == 0 && p->irq ? static_cast<int>(p->irq) : -EINVAL; }
int pci_enable_msi(struct pci_dev* p) { return setup_msi(p); }
void pci_disable_msi(struct pci_dev*) {}

// ---- DMA (no IOMMU: bus address = physical address) ----

void* dma_alloc_coherent(struct device* dev, size_t size, dma_addr_t* handle, gfp_t) {
    unsigned order = pmm::order_for_size(size ? size : 1);
    u64 phys = pmm::alloc_pages(order, true);
    if (!phys)
        return nullptr;
    if (dev && dev->coherent_dma_mask && phys + size - 1 > dev->coherent_dma_mask)
        klog::warn("linuxkpi: %s: DMA buffer at %#lx is above the device's mask %#lx", dev_name(dev), phys,
                   dev->coherent_dma_mask);
    *handle = phys;
    return mm::phys_to_virt<void>(phys);
}

void dma_free_coherent(struct device*, size_t size, void*, dma_addr_t handle) {
    if (handle)
        pmm::free_pages(handle, pmm::order_for_size(size ? size : 1));
}

dma_addr_t dma_map_single(struct device*, void* ptr, size_t, enum dma_data_direction) {
    return mm::direct_virt_to_phys(ptr);
}


int dma_set_mask(struct device* dev, u64 mask) {
    dev->dma_mask_storage = mask;
    dev->dma_mask = &dev->dma_mask_storage;
    return 0;
}
int dma_set_coherent_mask(struct device* dev, u64 mask) {
    dev->coherent_dma_mask = mask;
    return 0;
}
int dma_set_mask_and_coherent(struct device* dev, u64 mask) {
    dma_set_mask(dev, mask);
    return dma_set_coherent_mask(dev, mask);
}

struct dma_pool {
    struct device* dev;
    size_t size;
};
struct dma_pool* dma_pool_create(const char*, struct device* dev, size_t size, size_t align, size_t) {
    auto* p = static_cast<struct dma_pool*>(kzalloc(sizeof(struct dma_pool), GFP_KERNEL));
    if (p) {
        p->dev = dev;
        p->size = size > align ? size : align;
    }
    return p;
}
void dma_pool_destroy(struct dma_pool* p) { kfree(p); }
void* dma_pool_alloc(struct dma_pool* p, gfp_t, dma_addr_t* handle) {
    void* v = dma_alloc_coherent(p->dev, p->size, handle, 0); // zeroed
    return v;
}
void dma_pool_free(struct dma_pool* p, void* vaddr, dma_addr_t handle) { dma_free_coherent(p->dev, p->size, vaddr, handle); }

// ---- MMIO ----

void __iomem* ioremap(phys_addr_t phys, size_t size) {
    u64 base = phys & ~0xFFFull;
    void* v = sylux_map_physical(base, size + (phys - base));
    return v ? static_cast<u8*>(v) + (phys - base) : nullptr;
}
void iounmap(volatile void __iomem* addr) {
    if (addr)
        sylux_unmap_physical(reinterpret_cast<void*>(reinterpret_cast<uptr>(addr) & ~0xFFFull));
}

// ---- firmware ----

int request_firmware(const struct firmware** out, const char* name, struct device* dev) {
    *out = nullptr;
    char path[256];
    snprintf(path, sizeof(path), "/lib/firmware/%s", name);
    vfs::Inode* node;
    if (vfs::resolve(path, nullptr, true, &node) != 0 || vfs::is_dir(node->mode)) {
        if (dev)
            dev_err(dev, "firmware %s not found in /lib/firmware", name);
        return -ENOENT;
    }
    auto* fw = static_cast<struct firmware*>(kzalloc(sizeof(struct firmware), GFP_KERNEL));
    if (!fw)
        return -ENOMEM;
    void* buf;
    if (!sylux_vmalloc_bytes(node->size, &buf)) {
        kfree(fw);
        return -ENOMEM;
    }
    isize n = node->read(0, buf, node->size);
    if (n < 0 || static_cast<u64>(n) != node->size) {
        sylux_vfree(buf);
        kfree(fw);
        return -EIO;
    }
    fw->data = static_cast<const u8*>(buf);
    fw->size = node->size;
    fw->priv = buf;
    *out = fw;
    return 0;
}

void release_firmware(const struct firmware* fw) {
    if (!fw)
        return;
    sylux_vfree(fw->priv);
    kfree(fw);
}

struct FirmwareRequest {
    struct work_struct work;
    char name[128];
    struct device* dev;
    void* context;
    void (*cont)(const struct firmware*, void*);
};

static void firmware_work(struct work_struct* w) {
    auto* r = container_of(w, FirmwareRequest, work);
    const struct firmware* fw = nullptr;
    request_firmware(&fw, r->name, r->dev);
    r->cont(fw, r->context);
    kfree(r);
}

int request_firmware_nowait(void*, bool, const char* name, struct device* dev, gfp_t, void* context,
                            void (*cont)(const struct firmware*, void*)) {
    auto* r = static_cast<FirmwareRequest*>(kzalloc(sizeof(FirmwareRequest), GFP_KERNEL));
    if (!r)
        return -ENOMEM;
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->dev = dev;
    r->context = context;
    r->cont = cont;
    INIT_WORK(&r->work, firmware_work);
    schedule_work(&r->work);
    return 0;
}

} // extern "C"

namespace linuxkpi {

void init_pci() {}

} // namespace linuxkpi

// i915_driver.c: "gpufail=<n>" fails probe step n.
extern "C" int lkpi_gpu_fail_step(void) { return static_cast<int>(cmdline_number("gpufail=", 0)); }
