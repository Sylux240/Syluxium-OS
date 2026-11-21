/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: DMI (SMBIOS) matching - no table is read yet, nothing matches. */
enum dmi_field { DMI_NONE, DMI_BIOS_VENDOR, DMI_BIOS_VERSION, DMI_BIOS_DATE, DMI_BIOS_RELEASE, DMI_EC_FIRMWARE_RELEASE,
	DMI_SYS_VENDOR, DMI_PRODUCT_NAME, DMI_PRODUCT_VERSION, DMI_PRODUCT_SERIAL, DMI_PRODUCT_UUID, DMI_PRODUCT_SKU,
	DMI_PRODUCT_FAMILY, DMI_BOARD_VENDOR, DMI_BOARD_NAME, DMI_BOARD_VERSION, DMI_BOARD_SERIAL, DMI_BOARD_ASSET_TAG,
	DMI_CHASSIS_VENDOR, DMI_CHASSIS_TYPE, DMI_CHASSIS_VERSION, DMI_CHASSIS_SERIAL, DMI_CHASSIS_ASSET_TAG, DMI_STRING_MAX, DMI_OEM_STRING };
struct dmi_strmatch { unsigned char slot:7; unsigned char exact_match:1; char substr[79]; };
struct dmi_system_id { int (*callback)(const struct dmi_system_id *); const char *ident; struct dmi_strmatch matches[4]; void *driver_data; };
#define DMI_MATCH(a, b) { .slot = a, .substr = b }
#define DMI_EXACT_MATCH(a, b) { .slot = a, .substr = b, .exact_match = 1 }
#define dmi_check_system(list) 0
#define dmi_first_match(list) ((const struct dmi_system_id *)NULL)
#define dmi_get_system_info(field) ((const char *)NULL)
#define dmi_match(field, str) false
#define dmi_name_in_vendors(s) false

struct dmi_header { u8 type; u8 length; u16 handle; } __attribute__((packed));
static inline int dmi_walk(void (*decode)(const struct dmi_header *, void *), void *private_data) { return -ENXIO; }
