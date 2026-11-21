/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#define efi_enabled(f) false
#define EFI_BOOT 0
#define EFI_MEMMAP 1

/* UEFI runtime variables are not reachable from drivers here. */
typedef unsigned long efi_status_t;
typedef u16 efi_char16_t;
typedef struct { u8 b[16]; } efi_guid_t;
#define EFI_GUID(a, b, c, d...) ((efi_guid_t){ { (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
                                                (b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff, d } })
#define EFI_SUCCESS 0
#define EFI_ERROR_BIT (1UL << 63)
#define EFI_INVALID_PARAMETER (2 | EFI_ERROR_BIT)
#define EFI_UNSUPPORTED (3 | EFI_ERROR_BIT)
#define EFI_BUFFER_TOO_SMALL (5 | EFI_ERROR_BIT)
#define EFI_NOT_FOUND (14 | EFI_ERROR_BIT)
#define EFI_RT_SUPPORTED_GET_VARIABLE 0x0010
#define EFI_RT_SUPPORTED_SET_VARIABLE 0x0040
#define efi_rt_services_supported(mask) false
struct lkpi_efi {
    efi_status_t (*get_variable)(efi_char16_t *name, efi_guid_t *vendor, u32 *attr, unsigned long *data_size, void *data);
    efi_status_t (*set_variable)(efi_char16_t *name, efi_guid_t *vendor, u32 attr, unsigned long data_size, void *data);
};
extern struct lkpi_efi efi;
