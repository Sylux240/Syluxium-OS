#ifndef _LKPI_LINUX_DEVICE_H
#define _LKPI_LINUX_DEVICE_H

#include <linux/atomic.h>
#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

struct device_driver {
    const char* name;
    void* owner;
};

/* The common part of every device (struct pci_dev embeds one). */
struct device {
    struct device* parent;
    const char* init_name;
    char lkpi_name[32];
    void* driver_data;
    void* platform_data;
    struct device_driver* driver;
    u64 dma_mask_storage;
    u64* dma_mask;
    u64 coherent_dma_mask;
    void* lkpi_registry; /* Syluxium dev::Device */
};

static inline void* dev_get_drvdata(const struct device* d) { return d->driver_data; }
static inline void dev_set_drvdata(struct device* d, void* data) { d->driver_data = data; }
static inline const char* dev_name(const struct device* d) { return d->init_name ? d->init_name : d->lkpi_name; }
#define get_device(d) (d)
#define put_device(d) do { (void)(d); } while (0)

int lkpi_dev_printk(const char* level, const struct device* d, const char* fmt, ...) __printf(3, 4);
#define dev_emerg(d, fmt, ...) lkpi_dev_printk(KERN_EMERG, d, fmt, ##__VA_ARGS__)
#define dev_crit(d, fmt, ...) lkpi_dev_printk(KERN_CRIT, d, fmt, ##__VA_ARGS__)
#define dev_err(d, fmt, ...) lkpi_dev_printk(KERN_ERR, d, fmt, ##__VA_ARGS__)
#define dev_warn(d, fmt, ...) lkpi_dev_printk(KERN_WARNING, d, fmt, ##__VA_ARGS__)
#define dev_notice(d, fmt, ...) lkpi_dev_printk(KERN_NOTICE, d, fmt, ##__VA_ARGS__)
#define dev_info(d, fmt, ...) lkpi_dev_printk(KERN_INFO, d, fmt, ##__VA_ARGS__)
#define dev_dbg(d, fmt, ...) lkpi_dev_printk(KERN_DEBUG, d, fmt, ##__VA_ARGS__)
#define dev_err_once dev_err
#define dev_warn_once dev_warn
#define dev_info_once dev_info
#define dev_err_ratelimited dev_err
#define dev_warn_ratelimited dev_warn
#define dev_dbg_ratelimited dev_dbg
#define dev_err_probe(d, err, fmt, ...) ({ dev_err(d, fmt, ##__VA_ARGS__); (err); })

LKPI_EXTERN_C_END

#endif
