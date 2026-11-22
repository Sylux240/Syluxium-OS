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

/* SSB (Sonics Silicon Backplane) cores: tg3 includes ssb_driver_gige.h. */
#ifndef LKPI_SSB_DEVICE_ID
#define LKPI_SSB_DEVICE_ID
struct ssb_device_id {
	__u16 vendor;
	__u16 coreid;
	__u8 revision;
	__u8 __pad;
} __attribute__((packed, aligned(2)));
#define SSB_DEVICE(_vendor, _coreid, _revision) { .vendor = _vendor, .coreid = _coreid, .revision = _revision, }
#define SSB_ANY_VENDOR 0xFFFF
#define SSB_ANY_ID 0xFFFF
#define SSB_ANY_REV 0xFF
#endif
