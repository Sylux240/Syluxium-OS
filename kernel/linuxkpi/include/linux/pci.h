#ifndef _LKPI_LINUX_PCI_H
#define _LKPI_LINUX_PCI_H

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pci_ids.h>
#include <linux/ioport.h>
#include <uapi/linux/pci_regs.h>

LKPI_EXTERN_C_BEGIN

#define PCI_ANY_ID (~0U)
#define PCI_DEVICE(vend, dev) .vendor = (vend), .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_DEVICE_SUB(vend, dev, sv, sd) .vendor = (vend), .device = (dev), .subvendor = (sv), .subdevice = (sd)
#define PCI_VDEVICE(vend, dev) .vendor = PCI_VENDOR_ID_##vend, .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, 0, 0
#define PCI_DEVICE_CLASS(cls, mask) .class = (cls), .class_mask = (mask), .vendor = PCI_ANY_ID, .device = PCI_ANY_ID, .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID

#define DEVICE_COUNT_RESOURCE 6

struct pci_driver;

struct pci_bus { unsigned char number; struct pci_bus *parent; };

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
    struct resource resource[DEVICE_COUNT_RESOURCE + 1]; /* BARs + expansion ROM (PCI_ROM_RESOURCE) */
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
#define pci_request_region(d, bar, name) 0
#define pci_release_region(d, bar) do { } while (0)
#define pcim_request_region(d, bar, name) 0
struct pci_dev* pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn);
#define pci_get_slot(bus, devfn) pci_get_domain_bus_and_slot(0, (bus)->number, devfn)
#define pci_get_bus_and_slot(bus, devfn) pci_get_domain_bus_and_slot(0, bus, devfn)
struct pci_dev* pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev* from);
#define pci_get_subsys(v, d, sv, sd, from) pci_get_device(v, d, from)
struct pci_dev* pci_get_class(unsigned int cls, struct pci_dev* from);
#define pci_get_base_class(cls, from) pci_get_class(cls, from)
int pci_dev_present(const struct pci_device_id* ids);
int pci_bus_read_config_dword(struct pci_bus* bus, unsigned int devfn, int where, u32* val);
int pci_bus_read_config_word(struct pci_bus* bus, unsigned int devfn, int where, u16* val);
int pci_bus_read_config_byte(struct pci_bus* bus, unsigned int devfn, int where, u8* val);
int pci_bus_write_config_dword(struct pci_bus* bus, unsigned int devfn, int where, u32 val);
int pci_bus_write_config_word(struct pci_bus* bus, unsigned int devfn, int where, u16 val);
int pci_bus_write_config_byte(struct pci_bus* bus, unsigned int devfn, int where, u8 val);
struct pci_dev* pci_upstream_bridge(struct pci_dev* dev);
void __iomem* pci_map_rom(struct pci_dev* pdev, size_t* size);
void pci_unmap_rom(struct pci_dev* pdev, void __iomem* rom);
#define pci_platform_rom(pdev, size) ((void __iomem *)NULL)
#define pci_enable_rom(pdev) 0
#define pci_disable_rom(pdev) do { } while (0)
#define pci_domain_nr(bus) 0
#define pci_is_root_bus(bus) ((bus)->number == 0)
#define pci_dev_id(d) ((u16)(((d)->bus_number << 8) | (d)->devfn))
#define PCI_DEVFN(slot, func) ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)
#define PCI_BUS_NUM(x) (((x) >> 8) & 0xff)
#define PCI_ROM_RESOURCE 6
#define PCI_NUM_RESOURCES 7
#define pci_resource_n(d, n) (&(d)->resource[n])
#define pci_is_thunderbolt_attached(d) false
#define pci_is_bridge(d) (((d)->class >> 8) == 0x0604)
#define pci_pcie_type(d) 0
#define pcie_get_speed_cap(d) PCI_SPEED_UNKNOWN
#define pcie_get_width_cap(d) PCIE_LNK_WIDTH_UNKNOWN
#define pcie_bandwidth_available(d, lim_dev, speed, width) 0
#define pci_rebar_get_possible_sizes(d, bar) 0
#define pci_resize_resource(d, bar, size) (-ENOTSUPP)
#define pci_rebar_bytes_to_size(b) 0
#define pci_release_resource(d, bar) do { } while (0)
#define pci_assign_unassigned_bus_resources(bus) do { } while (0)
#define pci_find_host_bridge(bus) ((void *)NULL)
#define pci_ignore_hotplug(d) do { } while (0)
#define pci_d3cold_disable(d) do { } while (0)
#define pci_d3cold_enable(d) do { } while (0)
#define pci_reset_function(d) 0
#define pci_reset_function_locked(d) 0
#define pci_dev_is_disconnected(d) false
#define pci_channel_offline(d) false
#define pci_is_enabled(d) 1
#define pci_dev_run_wake(d) false
#define pci_acpi_set_companion(d) do { } while (0)
#define pcie_aspm_support_enabled() false
#define pci_enable_atomic_ops_to_root(d, mask) (-EINVAL)
#define pcie_capability_read_dword(d, pos, v) (*(v) = 0, 0)
#define pci_find_next_capability(d, pos, cap) 0
#define pci_bus_max_busnr(bus) 0
enum pci_bus_speed { PCIE_SPEED_2_5GT = 0x14, PCIE_SPEED_5_0GT = 0x15, PCIE_SPEED_8_0GT = 0x16, PCIE_SPEED_16_0GT = 0x17,
                     PCIE_SPEED_32_0GT = 0x18, PCIE_SPEED_64_0GT = 0x19, PCI_SPEED_UNKNOWN = 0xff };
enum pcie_link_width { PCIE_LNK_WIDTH_RESRV = 0, PCIE_LNK_X1 = 1, PCIE_LNK_X2 = 2, PCIE_LNK_X4 = 4, PCIE_LNK_X8 = 8,
                       PCIE_LNK_X12 = 12, PCIE_LNK_X16 = 16, PCIE_LNK_X32 = 32, PCIE_LNK_WIDTH_UNKNOWN = 0xff };
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

LKPI_EXTERN_C_END

#endif
