/* LinuxKPI: the ACPI interfaces Linux drivers call. Devices have no ACPI
 * companion here (ACPI_HANDLE() is NULL), so _DSM / method evaluation
 * never happens and drivers take their non-ACPI paths. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/uuid.h>

typedef void *acpi_handle;
typedef u32 acpi_status;
typedef u64 acpi_size;
typedef u32 acpi_object_type;
typedef char *acpi_string;
#define AE_OK 0
#define AE_ERROR 1
#define AE_NOT_FOUND 5
#define ACPI_SUCCESS(s) ((s) == AE_OK)
#define ACPI_FAILURE(s) ((s) != AE_OK)
#define ACPI_TYPE_ANY 0x00
#define ACPI_TYPE_INTEGER 0x01
#define ACPI_TYPE_STRING 0x02
#define ACPI_TYPE_BUFFER 0x03
#define ACPI_TYPE_PACKAGE 0x04
#define ACPI_TYPE_DEVICE 0x06
#define ACPI_FULL_PATHNAME 0
#define ACPI_SINGLE_NAME 1
#define ACPI_ALLOCATE_BUFFER ((acpi_size)(-1))
#define ACPI_STATE_S0 0
#define ACPI_STATE_S3 3
#define ACPI_STATE_S4 4
#define ACPI_VIDEO_CLASS "video"
#define ACPI_FREE(p) kfree(p)

union acpi_object {
	acpi_object_type type;
	struct { acpi_object_type type; u64 value; } integer;
	struct { acpi_object_type type; u32 length; char *pointer; } string;
	struct { acpi_object_type type; u32 length; u8 *pointer; } buffer;
	struct { acpi_object_type type; u32 count; union acpi_object *elements; } package;
};
struct acpi_buffer { acpi_size length; void *pointer; };
struct acpi_object_list { u32 count; union acpi_object *pointer; };
struct acpi_device_id { u8 id[16]; unsigned long driver_data; u32 cls; u32 cls_msk; };
struct acpi_device;
struct acpi_bus_event { struct list_head node; char device_class[20]; char bus_id[15]; u32 type; u32 data; };
struct fwnode_handle;

#define ACPI_HANDLE(dev) ((acpi_handle)NULL)
#define ACPI_HANDLE_FWNODE(fw) ((acpi_handle)NULL)
#define ACPI_COMPANION(dev) ((struct acpi_device *)NULL)
#define ACPI_COMPANION_SET(dev, adev) do { } while (0)
#define acpi_device_handle(adev) ((acpi_handle)NULL)
#define acpi_fwnode_handle(adev) ((struct fwnode_handle *)NULL)
#define acpi_find_child_device(parent, addr, check) ((struct acpi_device *)NULL)
#define acpi_get_name(h, type, buf) AE_NOT_FOUND
#define acpi_get_handle(parent, path, ret) AE_NOT_FOUND
#define acpi_has_method(h, name) false
#define acpi_evaluate_object(h, path, args, ret) AE_NOT_FOUND
#define acpi_evaluate_integer(h, path, args, ret) AE_NOT_FOUND
#define acpi_evaluate_dsm(h, guid, rev, func, argv4) ((union acpi_object *)NULL)
#define acpi_evaluate_dsm_typed(h, guid, rev, func, argv4, type) ((union acpi_object *)NULL)
#define acpi_check_dsm(h, guid, rev, funcs) false
#define acpi_target_system_state() ACPI_STATE_S0
#define acpi_disabled 1
#define acpi_video_backlight_use_native_internal() true
#define register_acpi_notifier(nb) 0
#define unregister_acpi_notifier(nb) 0
#define acpi_dev_present(hid, uid, hrv) false
#define acpi_dev_get_first_match_dev(hid, uid, hrv) ((struct acpi_device *)NULL)
#define acpi_dev_put(adev) do { } while (0)
#define acpi_device_hid(adev) ""
#define acpi_dev_name(adev) ""
#define has_acpi_companion(dev) false
#define acpi_match_device(ids, dev) ((const struct acpi_device_id *)NULL)
#define acpi_device_get_match_data(dev) ((const void *)NULL)
#define acpi_os_map_memory(p, s) ((void *)NULL)
#define acpi_os_unmap_memory(v, s) do { } while (0)
#define acpi_get_table(sig, inst, out) AE_NOT_FOUND
#define acpi_put_table(t) do { } while (0)
#define acpi_bus_get_device(h, adev) (-ENODEV)
struct acpi_resource_i2c_serialbus { u8 type; u16 slave_address; u32 connection_speed; struct { char *string_ptr; } resource_source; };
struct acpi_resource { u32 type; u32 length; union { struct acpi_resource_i2c_serialbus i2c_serial_bus; } data; };
#define ACPI_RESOURCE_TYPE_SERIAL_BUS 19
#define ACPI_RESOURCE_SERIAL_TYPE_I2C 1
#define acpi_dev_get_resources(adev, list, preproc, data) (-ENODEV)
#define acpi_dev_free_resource_list(list) do { } while (0)
#define i2c_acpi_get_i2c_resource(ares, i2c) false
#define acpi_handle_debug(h, fmt, ...) do { } while (0)
#define acpi_handle_info(h, fmt, ...) do { } while (0)
#define acpi_handle_err(h, fmt, ...) do { } while (0)
#define acpi_handle_warn(h, fmt, ...) do { } while (0)
