#ifndef _LKPI_LINUX_DEVICE_H
#define _LKPI_LINUX_DEVICE_H

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/pm.h>

LKPI_EXTERN_C_BEGIN

struct device;
struct module;

struct device_driver {
    const char* name;
    void* owner;
    const struct dev_pm_ops* pm;
};

/* sysfs: attributes are accepted and ignored (no sysfs yet). */
struct kobject {
    const char* name;
    struct kobject* parent;
    struct kref kref;
};
struct attribute {
    const char* name;
    unsigned short mode;
};
struct attribute_group {
    const char* name;
    unsigned short (*is_visible)(struct kobject*, struct attribute*, int);
    struct attribute** attrs;
    struct bin_attribute** bin_attrs;
};
struct device_attribute {
    struct attribute attr;
    ssize_t (*show)(struct device* dev, struct device_attribute* attr, char* buf);
    ssize_t (*store)(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
};
#define __ATTR(_name, _mode, _show, _store) { .attr = {.name = #_name, .mode = _mode}, .show = _show, .store = _store }
#define __ATTR_RO(_name) { .attr = {.name = #_name, .mode = 0444}, .show = _name##_show }
#define __ATTR_RW(_name) __ATTR(_name, 0644, _name##_show, _name##_store)
#define __ATTR_WO(_name) { .attr = {.name = #_name, .mode = 0200}, .store = _name##_store }
#define DEVICE_ATTR(_name, _mode, _show, _store) struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define DEVICE_ATTR_RO(_name) struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_RW(_name) struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define DEVICE_ATTR_WO(_name) struct device_attribute dev_attr_##_name = __ATTR_WO(_name)
#define __ATTRIBUTE_GROUPS(_name) static const struct attribute_group* _name##_groups[] = { &_name##_group, NULL, }
#define ATTRIBUTE_GROUPS(_name) static const struct attribute_group _name##_group = { .attrs = _name##_attrs, }; \
    __ATTRIBUTE_GROUPS(_name)
static inline int sysfs_emit(char* buf, const char* fmt, ...) { return 0; }
static inline int sysfs_create_link(struct kobject* kobj, struct kobject* target, const char* name) { return 0; }
static inline void sysfs_remove_link(struct kobject* kobj, const char* name) {}
static inline int sysfs_create_group(struct kobject* kobj, const struct attribute_group* grp) { return 0; }
static inline void sysfs_remove_group(struct kobject* kobj, const struct attribute_group* grp) {}
enum kobject_action { KOBJ_ADD, KOBJ_REMOVE, KOBJ_CHANGE, KOBJ_MOVE, KOBJ_ONLINE, KOBJ_OFFLINE, KOBJ_BIND, KOBJ_UNBIND };
static inline int kobject_uevent(struct kobject* kobj, enum kobject_action action) { return 0; }
static inline int kobject_uevent_env(struct kobject* kobj, enum kobject_action action, char* envp[]) { return 0; }
struct kobj_uevent_env { char buf[64]; int buflen; };
static inline int add_uevent_var(struct kobj_uevent_env* env, const char* format, ...) { return 0; }

#ifndef __cplusplus /* "class" and "namespace" are C++ keywords */
struct class {
    const char* name;
    const struct attribute_group** dev_groups;
    int (*dev_uevent)(const struct device* dev, struct kobj_uevent_env* env);
    void (*dev_release)(struct device* dev);
    const struct dev_pm_ops* pm;
    const void* (*namespace_fn)(const struct device* dev);
    void (*get_ownership)(const struct device* dev, u32* uid, u32* gid);
    const void* ns_type;
    const void* (*namespace)(const struct device* dev);
};
#endif
struct device_type {
    const char* name;
    const struct attribute_group** groups;
    void (*release)(struct device* dev);
};

/* The common part of every device (struct pci_dev embeds one). */
struct device {
    struct device* parent;
    dev_t devt;
    const char* init_name;
    char lkpi_name[32];
    void* driver_data;
    void* platform_data;
    struct device_driver* driver;
    u64 dma_mask_storage;
    u64* dma_mask;
    u64 coherent_dma_mask;
    void* lkpi_registry; /* Syluxium dev::Device */
    struct kobject kobj;
#ifdef __cplusplus
    const void* class_;
#else
    const struct class* class;
#endif
    const struct device_type* type;
    const struct attribute_group** groups;
    void (*release)(struct device* dev);
    struct device_node* of_node;
    struct fwnode_handle* fwnode;
    bool lkpi_added;
};

static inline void* dev_get_drvdata(const struct device* d) { return d->driver_data; }
static inline void dev_set_drvdata(struct device* d, void* data) { d->driver_data = data; }
static inline const char* dev_name(const struct device* d) { return d->init_name ? d->init_name : d->lkpi_name; }
static inline void* dev_get_platdata(const struct device* d) { return d->platform_data; }
#define get_device(d) (d)
#define device_set_wakeup_enable(d, e) 0
#define device_set_wakeup_capable(d, e) do { (void)(d); } while (0)
#define device_may_wakeup(d) 0
#define device_wakeup_enable(d) 0
#define device_enable_async_suspend(d) do { (void)(d); } while (0)
#define dev_set_uevent_suppress(d, v) do { (void)(d); } while (0)
#define dev_to_node(d) 0
#define dev_is_pci(d) 0
void put_device(struct device* dev);
void device_initialize(struct device* dev);
int device_add(struct device* dev);
void device_del(struct device* dev);
int device_register(struct device* dev);
void device_unregister(struct device* dev);
int dev_set_name(struct device* dev, const char* fmt, ...) __printf(2, 3);
int device_rename(struct device* dev, const char* new_name);
static inline int device_bind_driver(struct device* dev) { return 0; }
static inline void device_release_driver(struct device* dev) {}
static inline int device_is_registered(struct device* dev) { return dev->lkpi_added; }
#ifndef __cplusplus
struct device* device_create(const struct class* cls, struct device* parent, u32 devt, void* drvdata, const char* fmt, ...);
static inline int class_register(const struct class* cls) { return 0; }
static inline void class_unregister(const struct class* cls) {}
struct class* class_create(const char* name);
void class_destroy(const struct class* cls);
#endif
static inline int device_create_file(struct device* dev, const struct device_attribute* attr) { return 0; }
static inline void device_remove_file(struct device* dev, const struct device_attribute* attr) {}

int lkpi_dev_printk(const char* level, const struct device* d, const char* fmt, ...) __printf(3, 4);
#define dev_printk(level, d, fmt, ...) lkpi_dev_printk(level, d, fmt, ##__VA_ARGS__)
#define dev_emerg(d, fmt, ...) lkpi_dev_printk(KERN_EMERG, d, fmt, ##__VA_ARGS__)
#define dev_crit(d, fmt, ...) lkpi_dev_printk(KERN_CRIT, d, fmt, ##__VA_ARGS__)
#define dev_alert(d, fmt, ...) lkpi_dev_printk(KERN_ALERT, d, fmt, ##__VA_ARGS__)
#define dev_err(d, fmt, ...) lkpi_dev_printk(KERN_ERR, d, fmt, ##__VA_ARGS__)
#define dev_warn(d, fmt, ...) lkpi_dev_printk(KERN_WARNING, d, fmt, ##__VA_ARGS__)
#define dev_notice(d, fmt, ...) lkpi_dev_printk(KERN_NOTICE, d, fmt, ##__VA_ARGS__)
#define dev_info(d, fmt, ...) lkpi_dev_printk(KERN_INFO, d, fmt, ##__VA_ARGS__)
#define dev_dbg(d, fmt, ...) do { if (0) lkpi_dev_printk(KERN_DEBUG, d, fmt, ##__VA_ARGS__); } while (0)
#define dev_vdbg dev_dbg
#define dev_err_once dev_err
#define dev_warn_once dev_warn
#define dev_info_once dev_info
#define dev_notice_once dev_notice
#define dev_dbg_once dev_dbg
#define dev_err_ratelimited dev_err
#define dev_warn_ratelimited dev_warn
#define dev_info_ratelimited dev_info
#define dev_dbg_ratelimited dev_dbg
#define dev_err_probe(d, err, fmt, ...) ({ dev_err(d, fmt, ##__VA_ARGS__); (err); })

LKPI_EXTERN_C_END

#endif
