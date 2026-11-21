/* LinuxKPI: what Linux's USB network drivers (usbnet, r8152, ax88179_178a)
 * need beyond the PHY/Ethernet compat set. Included first (Makefile). */
#pragma once

#include <net/lkpi_phy.h>
#include <linux/usb.h>
#include <linux/scatterlist.h>

/* Runtime power management of USB interfaces: devices stay powered. */
#ifndef PMSG_IS_AUTO
#define PMSG_IS_AUTO(msg) (((msg).event & PM_EVENT_AUTO) != 0)
#endif
#ifndef PM_EVENT_AUTO
#define PM_EVENT_AUTO 0x0400
#endif
static inline int usb_autopm_get_interface_async(struct usb_interface *intf) { return 0; }
static inline void usb_autopm_put_interface_no_suspend(struct usb_interface *intf) { }

static inline void usb_put_urb(struct urb *urb) { usb_free_urb(urb); }

/* Does the interface's current setting have these endpoints? */
static inline bool usb_check_bulk_endpoints(const struct usb_interface *intf, const u8 *ep_addrs)
{
    const struct usb_host_interface *alt = intf->cur_altsetting;
    for (; *ep_addrs; ep_addrs++) {
        bool found = false;
        for (int i = 0; i < alt->desc.bNumEndpoints; i++) {
            const struct usb_endpoint_descriptor *d = &alt->endpoint[i].desc;
            if (d->bEndpointAddress == *ep_addrs && (d->bmAttributes & 3) == USB_ENDPOINT_XFER_BULK)
                found = true;
        }
        if (!found)
            return false;
    }
    return true;
}
static inline bool usb_check_int_endpoints(const struct usb_interface *intf, const u8 *ep_addrs)
{
    const struct usb_host_interface *alt = intf->cur_altsetting;
    for (; *ep_addrs; ep_addrs++) {
        bool found = false;
        for (int i = 0; i < alt->desc.bNumEndpoints; i++) {
            const struct usb_endpoint_descriptor *d = &alt->endpoint[i].desc;
            if (d->bEndpointAddress == *ep_addrs && (d->bmAttributes & 3) == USB_ENDPOINT_XFER_INT)
                found = true;
        }
        if (!found)
            return false;
    }
    return true;
}

/* Device-level USB drivers (r8152's configuration selector): the USB core
 * here chooses configurations itself (CDC Ethernet first, else the first),
 * so they register and are never called. */
struct usb_device_driver {
    const char *name;
    int (*choose_configuration)(struct usb_device *udev);
    int (*probe)(struct usb_device *udev);
    void (*disconnect)(struct usb_device *udev);
    const struct usb_device_id *id_table;
    unsigned int supports_autosuspend : 1;
    unsigned int generic_subclass : 1;
};
static inline int usb_register_device_driver(struct usb_device_driver *d, struct module *m) { return 0; }
static inline void usb_deregister_device_driver(struct usb_device_driver *d) { }
#ifndef NETIF_F_FRAGLIST
#define NETIF_F_FRAGLIST 0
#endif

/* PTP receive timestamps: none. */
static inline bool skb_defer_rx_timestamp(struct sk_buff *skb) { return false; }

#include <linux/capability.h>

#ifndef NET_ADDR_STOLEN
#define NET_ADDR_STOLEN 2
#endif
#define netif_notice(p, type, d, fmt, ...) do { if (netif_msg_##type(p)) netdev_info(d, fmt, ##__VA_ARGS__); } while (0)
#ifndef NETIF_F_GSO_MASK
#define NETIF_F_GSO_MASK (NETIF_F_GSO_SOFTWARE)
#endif
#ifndef WORK_BUSY_PENDING
#define WORK_BUSY_PENDING 1
#endif

/* USB 3 link power management: left as the hardware starts. */
static inline int usb_enable_lpm(struct usb_device *udev) { return 0; }
static inline int usb_disable_lpm(struct usb_device *udev) { return 0; }

static inline void pm_wakeup_event(struct device *dev, unsigned int msec) { }

/* The first bulk in/out and interrupt in/out endpoints of an altsetting. */
static inline int usb_find_common_endpoints(struct usb_host_interface *alt,
                                            struct usb_endpoint_descriptor **bulk_in,
                                            struct usb_endpoint_descriptor **bulk_out,
                                            struct usb_endpoint_descriptor **int_in,
                                            struct usb_endpoint_descriptor **int_out)
{
    struct usb_endpoint_descriptor **want[4] = {bulk_in, bulk_out, int_in, int_out};
    for (int k = 0; k < 4; k++)
        if (want[k])
            *want[k] = NULL;
    for (int i = 0; i < alt->desc.bNumEndpoints; i++) {
        struct usb_endpoint_descriptor *d = &alt->endpoint[i].desc;
        int type = d->bmAttributes & 3, in = (d->bEndpointAddress & 0x80) != 0;
        int k = type == USB_ENDPOINT_XFER_BULK ? (in ? 0 : 1) : type == USB_ENDPOINT_XFER_INT ? (in ? 2 : 3) : -1;
        if (k >= 0 && want[k] && !*want[k])
            *want[k] = d;
    }
    for (int k = 0; k < 4; k++)
        if (want[k] && !*want[k])
            return -ENXIO;
    return 0;
}

/* GRO from page fragments: the packet is put together in napi->skb
 * (skb_add_rx_frag copies into the linear area here), then handed on. */
static inline struct sk_buff *napi_get_frags(struct napi_struct *napi)
{
    if (!napi->skb)
        napi->skb = napi_alloc_skb(napi, 0);
    return napi->skb;
}
static inline int napi_gro_frags(struct napi_struct *napi)
{
    struct sk_buff *skb = napi->skb;
    napi->skb = NULL;
    if (!skb)
        return 0;
    skb->protocol = eth_type_trans(skb, napi->dev);
    netif_receive_skb(skb);
    return 0;
}
