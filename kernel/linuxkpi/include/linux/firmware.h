#ifndef _LKPI_LINUX_FIRMWARE_H
#define _LKPI_LINUX_FIRMWARE_H

#include <linux/device.h>
#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* Firmware blobs are loaded from /lib/firmware/<name> (linux-firmware
 * layout), as on Linux. */
struct firmware {
    size_t size;
    const u8* data;
    void* priv;
};

int request_firmware(const struct firmware** fw, const char* name, struct device* dev);
#define request_firmware_direct request_firmware
#define firmware_request_nowarn request_firmware
int request_firmware_nowait(void* module, bool uevent, const char* name, struct device* dev, gfp_t gfp, void* context,
                            void (*cont)(const struct firmware* fw, void* context));
void release_firmware(const struct firmware* fw);

LKPI_EXTERN_C_END

#endif
