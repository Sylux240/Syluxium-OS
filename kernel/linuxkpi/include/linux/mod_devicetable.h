#ifndef _LKPI_LINUX_MOD_DEVICETABLE_H
#define _LKPI_LINUX_MOD_DEVICETABLE_H

#include <linux/types.h>

struct pci_device_id {
    u32 vendor, device;
    u32 subvendor, subdevice;
    u32 class, class_mask;
    kernel_ulong_t driver_data;
};

struct usb_device_id {
    u16 match_flags;
    u16 idVendor, idProduct;
    u16 bcdDevice_lo, bcdDevice_hi;
    u8 bDeviceClass, bDeviceSubClass, bDeviceProtocol;
    u8 bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;
    u8 bInterfaceNumber;
    kernel_ulong_t driver_info;
};

#endif
