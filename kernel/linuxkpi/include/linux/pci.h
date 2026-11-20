#ifndef _LKPI_LINUX_PCI_H
#define _LKPI_LINUX_PCI_H

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

LKPI_EXTERN_C_BEGIN

#define PCI_ANY_ID (~0U)
#define PCI_DEVICE(vend, dev) .vendor = (vend), .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_DEVICE_SUB(vend, dev, sv, sd) .vendor = (vend), .device = (dev), .subvendor = (sv), .subdevice = (sd)
#define PCI_VDEVICE(vend, dev) .vendor = PCI_VENDOR_ID_##vend, .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, 0, 0
#define PCI_DEVICE_CLASS(cls, mask) .class = (cls), .class_mask = (mask), .vendor = PCI_ANY_ID, .device = PCI_ANY_ID, .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID

#define PCI_VENDOR_ID_INTEL 0x8086
#define PCI_VENDOR_ID_AMD 0x1022
#define PCI_VENDOR_ID_ATI 0x1002
#define PCI_VENDOR_ID_NVIDIA 0x10de
#define PCI_VENDOR_ID_REALTEK 0x10ec
#define PCI_VENDOR_ID_BROADCOM 0x14e4
#define PCI_VENDOR_ID_ATHEROS 0x168c
#define PCI_VENDOR_ID_QCOM 0x17cb
#define PCI_VENDOR_ID_MEDIATEK 0x14c3
#define PCI_VENDOR_ID_REDHAT_QUMRANET 0x1af4
#define PCI_DEVICE_ID_REALTEK_8139 0x8139
#define PCI_DEVICE_ID_TTTECH_MC322 0x000a
#define PCI_VENDOR_ID_DLINK 0x1186
#define PCI_VENDOR_ID_TTTECH 0x0357
#define PCI_VENDOR_ID_SIEMENS 0x110a

#define DEVICE_COUNT_RESOURCE 6
#define IORESOURCE_IO 0x100
#define IORESOURCE_MEM 0x200
#define IORESOURCE_PREFETCH 0x2000
#define IORESOURCE_MEM_64 0x100000

struct pci_driver;

struct pci_dev {
    struct device dev;
    unsigned short vendor, device;
    unsigned short subsystem_vendor, subsystem_device;
    unsigned int class; /* class << 16 | subclass << 8 | prog-if */
    u8 revision;
    u8 pin;
    unsigned int irq;
    unsigned int devfn;
    unsigned char bus_number;
    int msi_enabled, msix_enabled;
    struct pci_driver* driver;
    resource_size_t lkpi_res_start[DEVICE_COUNT_RESOURCE];
    resource_size_t lkpi_res_len[DEVICE_COUNT_RESOURCE];
    unsigned long lkpi_res_flags[DEVICE_COUNT_RESOURCE];
    void __iomem* lkpi_iomap[DEVICE_COUNT_RESOURCE];
    void* lkpi_function; /* Syluxium pci::Function */
    struct pci_bus* bus; /* appended: layout shared with C++ */
};

struct pci_driver {
    const char* name;
    const struct pci_device_id* id_table;
    int (*probe)(struct pci_dev* dev, const struct pci_device_id* id);
    void (*remove)(struct pci_dev* dev);
    void (*shutdown)(struct pci_dev* dev);
    struct device_driver driver;
    void* lkpi; /* Syluxium registration */
};

#define to_pci_dev(d) container_of(d, struct pci_dev, dev)

int pci_register_driver(struct pci_driver* drv);
#define __pci_register_driver(drv, owner, name) pci_register_driver(drv)
void pci_unregister_driver(struct pci_driver* drv);
#define module_pci_driver(drv)                                                   \
    static int __init lkpi_pci_init_##drv(void) { return pci_register_driver(&(drv)); } \
    module_init(lkpi_pci_init_##drv)

int pci_enable_device(struct pci_dev* dev);
#define pci_enable_device_mem pci_enable_device
#define pcim_enable_device pci_enable_device
void pci_disable_device(struct pci_dev* dev);
void pci_set_master(struct pci_dev* dev);
void pci_clear_master(struct pci_dev* dev);
int pci_request_regions(struct pci_dev* dev, const char* name);
#define pci_request_mem_regions(d, n) pci_request_regions(d, n)
#define pci_request_selected_regions(d, mask, n) pci_request_regions(d, n)
#define pcim_request_all_regions(d, n) pci_request_regions(d, n)
void pci_release_regions(struct pci_dev* dev);
#define pci_release_mem_regions pci_release_regions
#define pci_release_selected_regions(d, m) pci_release_regions(d)
#define pci_resource_start(d, bar) ((d)->lkpi_res_start[bar])
#define pci_resource_len(d, bar) ((d)->lkpi_res_len[bar])
#define pci_resource_end(d, bar) ((d)->lkpi_res_start[bar] + (d)->lkpi_res_len[bar] - 1)
#define pci_resource_flags(d, bar) ((d)->lkpi_res_flags[bar])
void __iomem* pci_iomap(struct pci_dev* dev, int bar, unsigned long maxlen);
#define pci_ioremap_bar(d, bar) pci_iomap(d, bar, 0)
#define pci_iomap_range(d, bar, off, len) ((void __iomem*)((u8 __iomem*)pci_iomap(d, bar, 0) + (off)))
void pci_iounmap(struct pci_dev* dev, void __iomem* addr);
int pcim_iomap_regions(struct pci_dev* dev, int mask, const char* name);
void __iomem* const* pcim_iomap_table(struct pci_dev* dev);
#define pcim_iomap(d, bar, len) pci_iomap(d, bar, len)

int pci_read_config_byte(const struct pci_dev* dev, int where, u8* val);
int pci_read_config_word(const struct pci_dev* dev, int where, u16* val);
int pci_read_config_dword(const struct pci_dev* dev, int where, u32* val);
int pci_write_config_byte(const struct pci_dev* dev, int where, u8 val);
int pci_write_config_word(const struct pci_dev* dev, int where, u16 val);
int pci_write_config_dword(const struct pci_dev* dev, int where, u32 val);
int pci_find_capability(struct pci_dev* dev, int cap);
int pci_find_ext_capability(struct pci_dev* dev, int cap);
int pcie_capability_read_word(struct pci_dev* dev, int pos, u16* val);
int pcie_capability_write_word(struct pci_dev* dev, int pos, u16 val);
int pcie_capability_clear_and_set_word(struct pci_dev* dev, int pos, u16 clear, u16 set);
#define pcie_capability_set_word(d, p, s) pcie_capability_clear_and_set_word(d, p, 0, s)
#define pcie_capability_clear_word(d, p, c) pcie_capability_clear_and_set_word(d, p, c, 0)

#define PCI_IRQ_LEGACY 0x1
#define PCI_IRQ_INTX PCI_IRQ_LEGACY
#define PCI_IRQ_MSI 0x2
#define PCI_IRQ_MSIX 0x4
#define PCI_IRQ_ALL_TYPES (PCI_IRQ_LEGACY | PCI_IRQ_MSI | PCI_IRQ_MSIX)
int pci_alloc_irq_vectors(struct pci_dev* dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags);
void pci_free_irq_vectors(struct pci_dev* dev);
int pci_irq_vector(struct pci_dev* dev, unsigned int nr);
int pci_enable_msi(struct pci_dev* dev);
void pci_disable_msi(struct pci_dev* dev);
#define pci_disable_msix pci_disable_msi

static inline void pci_set_drvdata(struct pci_dev* d, void* data) { dev_set_drvdata(&d->dev, data); }
static inline void* pci_get_drvdata(struct pci_dev* d) { return dev_get_drvdata(&d->dev); }
static inline const char* pci_name(const struct pci_dev* d) { return dev_name(&d->dev); }
#define pci_dev_get(d) (d)
#define pci_dev_put(d) do { (void)(d); } while (0)
#define PCI_D0 0
#define PCI_D3hot 3
#define PCI_D3cold 4
#define pci_enable_wake(d, state, en) 0
#define pci_set_mwi(d) 0
#define pci_try_set_mwi(d) 0
#define pci_clear_mwi(d) do { (void)(d); } while (0)
#define pci_choose_state(d, m) PCI_D3hot
#define pci_save_state(d) 0
#define pci_restore_state(d) do { (void)(d); } while (0)
#define pci_set_power_state(d, s) 0
#define pci_wake_from_d3(d, e) 0
#define pci_disable_link_state(d, s) 0
#define pcie_aspm_enabled(d) 0
#define pci_is_pcie(d) 1
#define pci_dev_msi_enabled(d) ((d)->msi_enabled || (d)->msix_enabled)

/* Config space offsets used by drivers. */
#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_COMMAND_IO 0x1
#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_MASTER 0x4
#define PCI_COMMAND_INTX_DISABLE 0x400
#define PCI_STATUS 0x06
#define PCI_REVISION_ID 0x08
#define PCI_CLASS_REVISION 0x08
#define PCI_CACHE_LINE_SIZE 0x0c
#define PCI_LATENCY_TIMER 0x0d
#define PCI_BASE_ADDRESS_0 0x10
#define PCI_SUBSYSTEM_VENDOR_ID 0x2c
#define PCI_SUBSYSTEM_ID 0x2e
#define PCI_CAPABILITY_LIST 0x34
#define PCI_INTERRUPT_LINE 0x3c
#define PCI_INTERRUPT_PIN 0x3d
#define PCI_CAP_ID_PM 0x01
#define PCI_CAP_ID_MSI 0x05
#define PCI_CAP_ID_EXP 0x10
#define PCI_CAP_ID_MSIX 0x11
#define PCI_EXP_DEVCTL 8
#define PCI_EXP_LNKCTL 16
#define PCI_EXP_LNKSTA 18
#define PCI_EXP_DEVCTL2 40
#define PCI_EXT_CAP_ID_ERR 0x01

LKPI_EXTERN_C_END

#endif
