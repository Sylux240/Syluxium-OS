/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: ACPI video extension - not used (i915 drives the backlight). */
struct acpi_device;
enum acpi_backlight_type { acpi_backlight_undef = -1, acpi_backlight_none = 0, acpi_backlight_video, acpi_backlight_vendor,
	acpi_backlight_native, acpi_backlight_nvidia_wmi_ec, acpi_backlight_apple_gmux, acpi_backlight_dell_uart };
#define acpi_video_register() 0
#define acpi_video_unregister() do { } while (0)
#define acpi_video_register_backlight() do { } while (0)
#define acpi_video_get_backlight_type() acpi_backlight_native
#define acpi_video_backlight_use_native() true
#define acpi_video_get_edid(dev, type, device_id, edid) (-ENODEV)
#define acpi_video_report_nolcd() do { } while (0)
#define ACPI_VIDEO_DISPLAY_LCD 4
