/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: hybrid-graphics switching (Apple gmux & co.) - not supported. */
enum vga_switcheroo_state { VGA_SWITCHEROO_OFF, VGA_SWITCHEROO_ON, VGA_SWITCHEROO_NOT_FOUND };
enum vga_switcheroo_client_id { VGA_SWITCHEROO_UNKNOWN_ID = 0x1000, VGA_SWITCHEROO_IGD = 0, VGA_SWITCHEROO_DIS, VGA_SWITCHEROO_MAX_CLIENTS };
enum vga_switcheroo_handler_flags_t { VGA_SWITCHEROO_CAN_SWITCH_DDC = (1 << 0), VGA_SWITCHEROO_NEEDS_EDP_CONFIG = (1 << 1) };
struct pci_dev;
struct vga_switcheroo_client_ops {
	void (*set_gpu_state)(struct pci_dev *dev, enum vga_switcheroo_state);
	void (*reprobe)(struct pci_dev *dev);
	bool (*can_switch)(struct pci_dev *dev);
	void (*gpu_bound)(struct pci_dev *dev, enum vga_switcheroo_client_id);
};
#define vga_switcheroo_register_client(dev, ops, driver_power_control) 0
#define vga_switcheroo_unregister_client(dev) do { } while (0)
#define vga_switcheroo_client_fb_set(dev, info) do { } while (0)
#define vga_switcheroo_handler_flags() 0
#define vga_switcheroo_lock_ddc(pdev) (-ENODEV)
#define vga_switcheroo_unlock_ddc(pdev) (-ENODEV)
#define vga_switcheroo_process_delayed_switch() do { } while (0)
#define vga_switcheroo_client_probe_defer(pdev) false
#define vga_switcheroo_get_client_state(dev) VGA_SWITCHEROO_ON
#define vga_switcheroo_init_domain_pm_ops(dev, domain) (-EINVAL)
#define vga_switcheroo_fini_domain_pm_ops(dev) do { } while (0)
#define vga_switcheroo_set_dynamic_switch(pdev, dynamic) do { } while (0)
#define vga_switcheroo_register_audio_client(pdev, ops, id) 0
