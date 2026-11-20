/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: VGA arbiter - one display driver per machine here. */
struct pci_dev;
#define VGA_RSRC_NONE 0x00
#define VGA_RSRC_LEGACY_IO 0x01
#define VGA_RSRC_LEGACY_MEM 0x02
#define VGA_RSRC_LEGACY_MASK (VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM)
#define VGA_RSRC_NORMAL_IO 0x04
#define VGA_RSRC_NORMAL_MEM 0x08
#define vga_get_interruptible(pdev, rsrc) 0
#define vga_get_uninterruptible(pdev, rsrc) 0
#define vga_put(pdev, rsrc) do { } while (0)
#define vga_set_legacy_decoding(pdev, decodes) do { } while (0)
#define vga_client_register(pdev, set_decode) 0
#define vga_client_unregister(pdev) do { } while (0)
#define vga_default_device() ((struct pci_dev *)NULL)
#define vga_remove_vgacon(pdev) 0
