/* LinuxKPI: platform devices (virtual devices such as mac80211_hwsim radios). */
#ifndef _LKPI_LINUX_PLATFORM_DEVICE_H
#define _LKPI_LINUX_PLATFORM_DEVICE_H
#include <linux/lkpi_all.h>
LKPI_EXTERN_C_BEGIN
struct platform_device_id { char name[20]; kernel_ulong_t driver_data; };
struct platform_device {
    const char* name;
    int id;
    struct device dev;
    const struct platform_device_id* id_entry;
    struct resource* resource; /* copies made at registration */
    unsigned int num_resources;
    struct platform_driver* lkpi_driver; /* bound driver */
};
struct platform_driver {
    int (*probe)(struct platform_device*);
    int (*remove)(struct platform_device*);
    void (*remove_new)(struct platform_device*);
    void (*shutdown)(struct platform_device*);
    struct device_driver driver;
    const struct platform_device_id* id_table;
};
#define to_platform_device(x) container_of((x), struct platform_device, dev)
#define PLATFORM_DEVID_NONE (-1)
#define PLATFORM_DEVID_AUTO (-2)
struct platform_device* platform_device_register_simple(const char* name, int id, const void* res, unsigned int num);
struct platform_device* platform_device_alloc(const char* name, int id);
int platform_device_add(struct platform_device* pdev);
void platform_device_put(struct platform_device* pdev);
void platform_device_unregister(struct platform_device* pdev);
int platform_driver_register(struct platform_driver* drv);
#define __platform_driver_register(drv, mod) platform_driver_register(drv)
void platform_driver_unregister(struct platform_driver* drv);
static inline void* platform_get_drvdata(const struct platform_device* pdev) { return dev_get_drvdata(&pdev->dev); }
static inline void platform_set_drvdata(struct platform_device* pdev, void* data) { dev_set_drvdata(&pdev->dev, data); }
LKPI_EXTERN_C_END

/* Registration with resources and platform data (copied). */
struct resource;
struct platform_device_info {
    struct device* parent;
    const char* name;
    int id;
    const struct resource* res;
    unsigned int num_res;
    const void* data;
    size_t size_data;
    u64 dma_mask;
};
struct platform_device* platform_device_register_full(const struct platform_device_info* info);
struct platform_device* platform_device_register_resndata(struct device* parent, const char* name, int id,
                                                          const struct resource* res, unsigned int num,
                                                          const void* data, size_t size);
#define platform_device_register_data(parent, name, id, data, size) \
    platform_device_register_resndata(parent, name, id, NULL, 0, data, size)
struct resource* lkpi_platform_get_resource(struct platform_device* pdev, unsigned int type, unsigned int num);
#endif /* _LKPI_LINUX_PLATFORM_DEVICE_H */
