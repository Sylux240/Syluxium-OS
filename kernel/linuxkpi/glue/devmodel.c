/* LinuxKPI: the driver model for buses Linux code registers itself (MDIO:
 * PHY devices and PHY drivers bind through bus->match), and managed
 * resources (devres). PCI and USB drivers bind through pci.cpp/usb.cpp. */
#include <linux/lkpi_all.h>
#include <linux/device/bus.h>
#include <linux/devres.h>

/* ---- buses ---- */

#define LKPI_MAX_BUSES 8

struct lkpi_bus {
    const struct bus_type *bus;
    struct device **devs;
    int ndevs, devs_cap;
    struct device_driver **drvs;
    int ndrvs, drvs_cap;
};

static struct lkpi_bus g_buses[LKPI_MAX_BUSES];
static DEFINE_MUTEX(g_bus_lock);

static struct lkpi_bus *find_bus(const struct bus_type *bus)
{
    for (int i = 0; i < LKPI_MAX_BUSES; i++)
        if (bus && g_buses[i].bus == bus)
            return &g_buses[i];
    return NULL;
}

static bool grow(void ***arr, int *cap, int need)
{
    if (need <= *cap)
        return true;
    int ncap = *cap ? *cap * 2 : 16;
    void **n = krealloc(*arr, ncap * sizeof(void *), GFP_KERNEL);
    if (!n)
        return false;
    *arr = n;
    *cap = ncap;
    return true;
}

int bus_register(const struct bus_type *bus)
{
    mutex_lock(&g_bus_lock);
    if (find_bus(bus)) {
        mutex_unlock(&g_bus_lock);
        return 0;
    }
    for (int i = 0; i < LKPI_MAX_BUSES; i++) {
        if (!g_buses[i].bus) {
            g_buses[i].bus = bus;
            mutex_unlock(&g_bus_lock);
            return 0;
        }
    }
    mutex_unlock(&g_bus_lock);
    pr_warn("linuxkpi: too many buses, %s not registered\n", bus->name);
    return -ENOMEM;
}

void bus_unregister(const struct bus_type *bus)
{
    mutex_lock(&g_bus_lock);
    struct lkpi_bus *b = find_bus(bus);
    if (b) {
        kfree(b->devs);
        kfree(b->drvs);
        memset(b, 0, sizeof(*b));
    }
    mutex_unlock(&g_bus_lock);
}

/* Bind `dev` to `drv`: the bus's probe, else the driver's. Lock held. */
static int really_probe(struct lkpi_bus *b, struct device *dev, struct device_driver *drv)
{
    int r = 0;
    dev->driver = drv;
    if (b->bus->probe)
        r = b->bus->probe(dev);
    else if (drv->probe)
        r = drv->probe(dev);
    if (r) {
        dev->driver = NULL;
        if (r != -ENODEV && r != -ENXIO)
            pr_warn("linuxkpi: %s: probing %s failed (%d)\n", drv->name ? drv->name : "?", dev_name(dev), r);
    }
    return r;
}

static void unbind(struct lkpi_bus *b, struct device *dev)
{
    struct device_driver *drv = dev->driver;
    if (!drv)
        return;
    if (b && b->bus->remove)
        b->bus->remove(dev);
    else if (drv->remove)
        drv->remove(dev);
    dev->driver = NULL;
}

int driver_register(struct device_driver *drv)
{
    mutex_lock(&g_bus_lock);
    struct lkpi_bus *b = find_bus(drv->bus);
    if (!b) {
        mutex_unlock(&g_bus_lock);
        pr_warn("linuxkpi: driver %s: its bus isn't registered\n", drv->name ? drv->name : "?");
        return -EINVAL;
    }
    if (!grow((void ***)&b->drvs, &b->drvs_cap, b->ndrvs + 1)) {
        mutex_unlock(&g_bus_lock);
        return -ENOMEM;
    }
    b->drvs[b->ndrvs++] = drv;
    for (int i = 0; i < b->ndevs; i++) {
        struct device *dev = b->devs[i];
        if (!dev->driver && (!b->bus->match || b->bus->match(dev, drv) > 0))
            really_probe(b, dev, drv);
    }
    mutex_unlock(&g_bus_lock);
    return 0;
}

void driver_unregister(struct device_driver *drv)
{
    mutex_lock(&g_bus_lock);
    struct lkpi_bus *b = find_bus(drv->bus);
    if (b) {
        for (int i = 0; i < b->ndevs; i++)
            if (b->devs[i]->driver == drv)
                unbind(b, b->devs[i]);
        for (int i = 0; i < b->ndrvs; i++) {
            if (b->drvs[i] == drv) {
                b->drvs[i] = b->drvs[--b->ndrvs];
                break;
            }
        }
    }
    mutex_unlock(&g_bus_lock);
}

/* device_add()/device_del() of a device on a registered bus (glue/lib.c). */
void lkpi_bus_device_added(struct device *dev)
{
    mutex_lock(&g_bus_lock);
    struct lkpi_bus *b = find_bus(dev->bus);
    if (!b || !grow((void ***)&b->devs, &b->devs_cap, b->ndevs + 1)) {
        mutex_unlock(&g_bus_lock);
        return;
    }
    b->devs[b->ndevs++] = dev;
    for (int i = 0; i < b->ndrvs && !dev->driver; i++)
        if (!b->bus->match || b->bus->match(dev, b->drvs[i]) > 0)
            really_probe(b, dev, b->drvs[i]);
    mutex_unlock(&g_bus_lock);
}

void lkpi_bus_device_removed(struct device *dev)
{
    mutex_lock(&g_bus_lock);
    struct lkpi_bus *b = find_bus(dev->bus);
    if (b) {
        unbind(b, dev);
        for (int i = 0; i < b->ndevs; i++) {
            if (b->devs[i] == dev) {
                b->devs[i] = b->devs[--b->ndevs];
                break;
            }
        }
    }
    mutex_unlock(&g_bus_lock);
}

void device_release_driver(struct device *dev)
{
    mutex_lock(&g_bus_lock);
    unbind(find_bus(dev->bus), dev);
    mutex_unlock(&g_bus_lock);
}

int bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data, int (*fn)(struct device *, void *))
{
    /* Snapshot under the lock; callbacks may register devices. */
    mutex_lock(&g_bus_lock);
    struct lkpi_bus *b = find_bus(bus);
    int n = b ? b->ndevs : 0, r = 0;
    struct device **copy = n ? kmalloc_array(n, sizeof(*copy), GFP_KERNEL) : NULL;
    if (copy)
        memcpy(copy, b->devs, n * sizeof(*copy));
    else
        n = 0;
    mutex_unlock(&g_bus_lock);
    bool started = !start;
    for (int i = 0; i < n && !r; i++) {
        if (!started) {
            started = copy[i] == start;
            continue;
        }
        r = fn(copy[i], data);
    }
    kfree(copy);
    return r;
}

struct find_ctx {
    const void *data;
    int (*match)(struct device *dev, const void *data);
    struct device *found;
};

static int find_one(struct device *dev, void *p)
{
    struct find_ctx *c = p;
    if (c->match(dev, c->data)) {
        c->found = dev;
        return 1;
    }
    return 0;
}

struct device *bus_find_device(const struct bus_type *bus, struct device *start, const void *data,
                               int (*match)(struct device *dev, const void *data))
{
    struct find_ctx c = {data, match, NULL};
    bus_for_each_dev(bus, start, &c, find_one);
    return c.found;
}

static int match_name(struct device *dev, const void *name)
{
    return !strcmp(dev_name(dev), name);
}

struct device *bus_find_device_by_name(const struct bus_type *bus, struct device *start, const char *name)
{
    return bus_find_device(bus, start, name, match_name);
}

/* ---- devres: resources freed when their device goes away ---- */

struct lkpi_devres {
    struct list_head node;
    struct device *dev;
    dr_release_t release;
    size_t size;
    unsigned long long data[]; /* aligned for any payload */
};

static LIST_HEAD(g_devres);
static DEFINE_SPINLOCK(g_devres_lock);

void *__devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp, int nid, const char *name)
{
    struct lkpi_devres *dr = kzalloc(sizeof(*dr) + size, gfp);
    if (!dr)
        return NULL;
    INIT_LIST_HEAD(&dr->node);
    dr->release = release;
    dr->size = size;
    return dr->data;
}

static struct lkpi_devres *to_dr(void *res) { return container_of((unsigned long long *)res, struct lkpi_devres, data[0]); }

void devres_add(struct device *dev, void *res)
{
    struct lkpi_devres *dr = to_dr(res);
    dr->dev = dev;
    spin_lock(&g_devres_lock);
    list_add_tail(&dr->node, &g_devres);
    spin_unlock(&g_devres_lock);
}

void devres_free(void *res)
{
    if (res)
        kfree(to_dr(res));
}

void *devres_find(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
    struct lkpi_devres *dr;
    void *found = NULL;
    spin_lock(&g_devres_lock);
    list_for_each_entry(dr, &g_devres, node) {
        if (dr->dev == dev && dr->release == release && (!match || match(dev, dr->data, match_data))) {
            found = dr->data;
            break;
        }
    }
    spin_unlock(&g_devres_lock);
    return found;
}

void *devres_remove(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
    void *res = devres_find(dev, release, match, match_data);
    if (res) {
        spin_lock(&g_devres_lock);
        list_del_init(&to_dr(res)->node);
        spin_unlock(&g_devres_lock);
    }
    return res;
}

int devres_destroy(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
    void *res = devres_remove(dev, release, match, match_data);
    if (!res)
        return -ENOENT;
    devres_free(res);
    return 0;
}

int devres_release(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
    void *res = devres_remove(dev, release, match, match_data);
    if (!res)
        return -ENOENT;
    release(dev, res);
    devres_free(res);
    return 0;
}

/* The device is gone: release its resources, newest first. */
void devres_release_all(struct device *dev)
{
    for (;;) {
        struct lkpi_devres *dr, *last = NULL;
        spin_lock(&g_devres_lock);
        list_for_each_entry(dr, &g_devres, node)
            if (dr->dev == dev)
                last = dr;
        if (last)
            list_del_init(&last->node);
        spin_unlock(&g_devres_lock);
        if (!last)
            break;
        if (last->release)
            last->release(dev, last->data);
        kfree(last);
    }
}
