// LinuxKPI device glue: PCI drivers on top of Syluxium's device registry,
// interrupts (MSI/MSI-X vectors), DMA, MMIO mappings and firmware loading.

#include <arch/x86_64/interrupts.hpp>
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

IrqAction g_irqs[256];
volatile int g_irq_depth = 0;

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

// Allocate an MSI/MSI-X vector for the device (once).
int setup_msi(struct pci_dev* pdev) {
    if (pdev->irq)
        return 0;
    auto* f = static_cast<pci::Function*>(pdev->lkpi_function);
    if (!f->msi_cap && !f->msix_cap)
        return -ENOSPC; // legacy INTx routing (ACPI _PRT) isn't supported yet
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

bool lkpi_match(const dev::Driver* drv, const dev::Device* d) {
    return d->bus == dev::Bus::Pci && find_id(static_cast<const pci_driver*>(drv->ctx), d);
}

long lkpi_probe(const dev::Driver* drv, dev::Device* d) {
    auto* pdrv = static_cast<pci_driver*>(drv->ctx);
    pci::Function* f = pci::function_of(d);
    auto* pdev = static_cast<pci_dev*>(kzalloc(sizeof(pci_dev), GFP_KERNEL));
    if (!pdev)
        return -ENOMEM;
    pdev->vendor = d->vendor;
    pdev->device = d->device;
    pdev->subsystem_vendor = d->subsystem_vendor;
    pdev->subsystem_device = d->subsystem_device;
    pdev->lkpi_class = d->class_code;
    pdev->revision = d->revision;
    pdev->pin = f->irq_pin;
    pdev->bus_number = f->bus;
    pdev->devfn = static_cast<unsigned>((f->device << 3) | f->function);
    pdev->driver = pdrv;
    pdev->lkpi_function = f;
    pdev->dev.lkpi_registry = d;
    snprintf(pdev->dev.lkpi_name, sizeof(pdev->dev.lkpi_name), "%s", d->bus_id);
    pdev->dev.dma_mask_storage = DMA_BIT_MASK(32); // Linux's default until the driver sets one
    pdev->dev.dma_mask = &pdev->dev.dma_mask_storage;
    pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
    pdev->dev.driver = &pdrv->driver;
    for (int i = 0; i < DEVICE_COUNT_RESOURCE; i++) {
        const pci::Bar& b = f->bars[i];
        pdev->lkpi_res_start[i] = b.address;
        pdev->lkpi_res_len[i] = b.size;
        if (b.size)
            pdev->lkpi_res_flags[i] = (b.is_io ? IORESOURCE_IO : IORESOURCE_MEM) |
                                      (b.prefetchable ? IORESOURCE_PREFETCH : 0) | (b.is_64bit ? IORESOURCE_MEM_64 : 0);
    }
    d->driver_data = pdev;
    int r = pdrv->probe(pdev, find_id(pdrv, d));
    if (r) {
        d->driver_data = nullptr;
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
    if (irq == 0 || irq > 255)
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
    if (irq > 255)
        return;
    g_irqs[irq].handler = nullptr;
    synchronize_irq(irq);
}

void disable_irq_nosync(unsigned int irq) {
    if (irq <= 255)
        g_irqs[irq].disabled = true;
}
void disable_irq(unsigned int irq) {
    disable_irq_nosync(irq);
    synchronize_irq(irq);
}
void enable_irq(unsigned int irq) {
    if (irq <= 255)
        g_irqs[irq].disabled = false;
}
void synchronize_irq(unsigned int irq) {
    if (irq > 255)
        return;
    while (g_irqs[irq].in_handler)
        __builtin_ia32_pause();
}

static void tasklet_work(struct work_struct* w) {
    auto* t = container_of(w, struct tasklet_struct, work);
    if (t->disabled)
        return;
    if (t->callback)
        t->callback(t);
    else if (t->func)
        t->func(t->data);
}
void tasklet_init(struct tasklet_struct* t, void (*func)(unsigned long), unsigned long data) {
    lkpi_init_work(&t->work, tasklet_work);
    t->func = func;
    t->callback = nullptr;
    t->data = data;
    t->disabled = 0;
}
void tasklet_setup(struct tasklet_struct* t, void (*cb)(struct tasklet_struct*)) {
    tasklet_init(t, nullptr, 0);
    t->callback = cb;
}
void tasklet_schedule(struct tasklet_struct* t) { queue_work(system_highpri_wq, &t->work); }
void tasklet_kill(struct tasklet_struct* t) { cancel_work_sync(&t->work); }

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

void pci_unregister_driver(struct pci_driver*) {
    // Built-in drivers stay registered; unbinding comes with hot-unplug support.
}

int pci_enable_device(struct pci_dev* p) {
    pci::enable(fn(p));
    return 0;
}
void pci_disable_device(struct pci_dev* p) { pci::write16(fn(p), PCI_COMMAND, pci::read16(fn(p), PCI_COMMAND) & ~0x7); }
void pci_set_master(struct pci_dev* p) { pci::write16(fn(p), PCI_COMMAND, pci::read16(fn(p), PCI_COMMAND) | PCI_COMMAND_MASTER); }
void pci_clear_master(struct pci_dev* p) { pci::write16(fn(p), PCI_COMMAND, pci::read16(fn(p), PCI_COMMAND) & ~PCI_COMMAND_MASTER); }
int pci_request_regions(struct pci_dev*, const char*) { return 0; }
void pci_release_regions(struct pci_dev*) {}

void __iomem* pci_iomap(struct pci_dev* p, int bar, unsigned long) {
    if (bar < 0 || bar >= DEVICE_COUNT_RESOURCE || !p->lkpi_res_len[bar])
        return nullptr;
    if (p->lkpi_res_flags[bar] & IORESOURCE_IO)
        return nullptr; // port I/O BARs: drivers must use inb/outb with pci_resource_start
    if (!p->lkpi_iomap[bar])
        p->lkpi_iomap[bar] = const_cast<void*>(reinterpret_cast<volatile void*>(pci::map_bar(fn(p), bar)));
    return p->lkpi_iomap[bar];
}
void pci_iounmap(struct pci_dev*, void __iomem*) {} // BAR mappings are permanent

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
    if (!(flags & (PCI_IRQ_MSI | PCI_IRQ_MSIX)))
        return -ENOSPC;
    int r = setup_msi(p);
    return r ? r : 1;
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

dma_addr_t dma_map_page(struct device*, void* page_virt, size_t offset, size_t, enum dma_data_direction) {
    return mm::direct_virt_to_phys(page_virt) + offset;
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
