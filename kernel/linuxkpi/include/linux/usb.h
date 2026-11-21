/* LinuxKPI: USB drivers. struct usb_device / usb_interface / urb / usb_driver
 * as in Linux, on top of Syluxium's USB core (kernel/drivers/usb): the glue
 * (glue/usb.c + usb.cpp) creates a usb_device + usb_interface when a device
 * matching a registered driver's id_table is plugged in, and runs each URB
 * on a per-endpoint worker thread with Syluxium's synchronous transfers,
 * calling urb->complete() from that thread afterwards. */
#ifndef _LKPI_LINUX_USB_H
#define _LKPI_LINUX_USB_H

#include <linux/lkpi_all.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/completion.h>
#include <uapi/linux/usb/ch9.h>

LKPI_EXTERN_C_BEGIN

struct usb_device;
struct urb;
struct usb_driver;

struct usb_host_endpoint {
    struct usb_endpoint_descriptor desc;
    struct usb_ss_ep_comp_descriptor ss_ep_comp;
    unsigned char* extra;
    int extralen;
    int enabled;
    int streams;
};

struct usb_host_interface {
    struct usb_interface_descriptor desc;
    int extralen;
    unsigned char* extra;
    struct usb_host_endpoint* endpoint;
    char* string;
};

enum usb_interface_condition {
    USB_INTERFACE_UNBOUND = 0,
    USB_INTERFACE_BINDING,
    USB_INTERFACE_BOUND,
    USB_INTERFACE_UNBINDING,
};

struct usb_interface {
    struct usb_host_interface* altsetting;
    struct usb_host_interface* cur_altsetting;
    unsigned num_altsetting;
    int minor;
    enum usb_interface_condition condition;
    unsigned needs_remote_wakeup : 1;
    struct device dev;
    struct usb_device* lkpi_udev;
    struct usb_driver* lkpi_driver;
};
#define to_usb_interface(d) container_of(d, struct usb_interface, dev)

struct usb_host_config {
    struct usb_config_descriptor desc;
    char* string;
    struct usb_interface* interface[32];
    unsigned char* extra;
    int extralen;
};

struct usb_bus {
    int busnum;
    const char* bus_name;
};

struct usb_device {
    int devnum;
    char devpath[16];
    u32 route;
    enum usb_device_state state;
    enum usb_device_speed speed;
    unsigned int rx_lanes, tx_lanes;
    struct usb_bus* bus;
    struct usb_host_endpoint ep0;
    struct device dev;
    struct usb_device_descriptor descriptor;
    struct usb_host_config* config;
    struct usb_host_config* actconfig;
    struct usb_host_endpoint* ep_in[16];
    struct usb_host_endpoint* ep_out[16];
    unsigned short bus_mA;
    u8 portnum;
    u8 level;
    unsigned can_submit : 1, persist_enabled : 1, reset_resume : 1, removable : 2;
    char* product;
    char* manufacturer;
    char* serial;
    int maxchild;
    u32 quirks;
    atomic_t urbnum;
    /* LinuxKPI private */
    void* lkpi_sylux;    /* Syluxium usb::Device */
    void* lkpi_eps[32];  /* per-endpoint URB worker state */
    volatile int lkpi_gone;
    struct usb_host_config lkpi_config;
    struct usb_host_interface lkpi_altsettings[8];
    struct usb_host_endpoint lkpi_endpoints[8][16];
    struct usb_interface lkpi_intfs[8];
    int lkpi_nintf;
    char lkpi_product[64];
};
#define to_usb_device(d) container_of(d, struct usb_device, dev)

static inline struct usb_device* interface_to_usbdev(struct usb_interface* intf) { return intf->lkpi_udev; }
static inline void* usb_get_intfdata(struct usb_interface* intf) { return dev_get_drvdata(&intf->dev); }
static inline void usb_set_intfdata(struct usb_interface* intf, void* data) { dev_set_drvdata(&intf->dev, data); }
static inline struct usb_device* usb_get_dev(struct usb_device* dev) { return dev; }
static inline void usb_put_dev(struct usb_device* dev) {}
static inline struct usb_interface* usb_get_intf(struct usb_interface* intf) { return intf; }
static inline void usb_put_intf(struct usb_interface* intf) {}
static inline int usb_make_path(struct usb_device* dev, char* buf, size_t size) {
    return snprintf(buf, size, "usb-%s", dev->devpath);
}
struct usb_interface* usb_ifnum_to_if(const struct usb_device* dev, unsigned ifnum);

/* Pipes: Linux's encoding. */
#define PIPE_ISOCHRONOUS 0
#define PIPE_INTERRUPT 1
#define PIPE_CONTROL 2
#define PIPE_BULK 3
#define usb_pipein(pipe) ((pipe) & USB_DIR_IN)
#define usb_pipeout(pipe) (!usb_pipein(pipe))
#define usb_pipedevice(pipe) (((pipe) >> 8) & 0x7f)
#define usb_pipeendpoint(pipe) (((pipe) >> 15) & 0xf)
#define usb_pipetype(pipe) (((pipe) >> 30) & 3)
#define usb_pipeisoc(pipe) (usb_pipetype((pipe)) == PIPE_ISOCHRONOUS)
#define usb_pipeint(pipe) (usb_pipetype((pipe)) == PIPE_INTERRUPT)
#define usb_pipecontrol(pipe) (usb_pipetype((pipe)) == PIPE_CONTROL)
#define usb_pipebulk(pipe) (usb_pipetype((pipe)) == PIPE_BULK)
static inline unsigned int __create_pipe(struct usb_device* dev, unsigned int endpoint) {
    return (dev->devnum << 8) | (endpoint << 15);
}
#define usb_sndctrlpipe(dev, endpoint) ((PIPE_CONTROL << 30) | __create_pipe(dev, endpoint))
#define usb_rcvctrlpipe(dev, endpoint) ((PIPE_CONTROL << 30) | __create_pipe(dev, endpoint) | USB_DIR_IN)
#define usb_sndisocpipe(dev, endpoint) ((PIPE_ISOCHRONOUS << 30) | __create_pipe(dev, endpoint))
#define usb_rcvisocpipe(dev, endpoint) ((PIPE_ISOCHRONOUS << 30) | __create_pipe(dev, endpoint) | USB_DIR_IN)
#define usb_sndbulkpipe(dev, endpoint) ((PIPE_BULK << 30) | __create_pipe(dev, endpoint))
#define usb_rcvbulkpipe(dev, endpoint) ((PIPE_BULK << 30) | __create_pipe(dev, endpoint) | USB_DIR_IN)
#define usb_sndintpipe(dev, endpoint) ((PIPE_INTERRUPT << 30) | __create_pipe(dev, endpoint))
#define usb_rcvintpipe(dev, endpoint) ((PIPE_INTERRUPT << 30) | __create_pipe(dev, endpoint) | USB_DIR_IN)
static inline struct usb_host_endpoint* usb_pipe_endpoint(struct usb_device* dev, unsigned int pipe) {
    struct usb_host_endpoint** eps = usb_pipein(pipe) ? dev->ep_in : dev->ep_out;
    return eps[usb_pipeendpoint(pipe)];
}
static inline u16 usb_maxpacket(struct usb_device* udev, int pipe) {
    struct usb_host_endpoint* ep = usb_pipe_endpoint(udev, pipe);
    return ep ? usb_endpoint_maxp(&ep->desc) : 0;
}

/* URBs */
typedef void (*usb_complete_t)(struct urb*);

#define URB_SHORT_NOT_OK 0x0001
#define URB_ISO_ASAP 0x0002
#define URB_NO_TRANSFER_DMA_MAP 0x0004
#define URB_ZERO_PACKET 0x0040
#define URB_NO_INTERRUPT 0x0080
#define URB_FREE_BUFFER 0x0100
#define URB_DIR_IN 0x0200
#define URB_DIR_OUT 0
#define URB_DIR_MASK URB_DIR_IN

struct usb_iso_packet_descriptor {
    unsigned int offset;
    unsigned int length;
    unsigned int actual_length;
    int status;
};

struct usb_anchor {
    struct list_head urb_list;
    wait_queue_head_t wait;
    spinlock_t lock;
    atomic_t suspend_wakeups;
    unsigned int poisoned : 1;
};
#define DEFINE_USB_ANCHOR(name) struct usb_anchor name = { .urb_list = LIST_HEAD_INIT(name.urb_list) }
static inline void init_usb_anchor(struct usb_anchor* anchor) {
    memset(anchor, 0, sizeof(*anchor));
    INIT_LIST_HEAD(&anchor->urb_list);
    init_waitqueue_head(&anchor->wait);
    spin_lock_init(&anchor->lock);
}

struct urb {
    struct kref kref;
    int unlinked;
    void* hcpriv;
    atomic_t use_count;
    atomic_t reject;
    struct list_head urb_list;
    struct list_head anchor_list;
    struct usb_anchor* anchor;
    struct usb_device* dev;
    struct usb_host_endpoint* ep;
    unsigned int pipe;
    unsigned int stream_id;
    int status;
    unsigned int transfer_flags;
    void* transfer_buffer;
    dma_addr_t transfer_dma;
    struct scatterlist* sg;
    int num_mapped_sgs;
    int num_sgs;
    u32 transfer_buffer_length;
    u32 actual_length;
    unsigned char* setup_packet;
    dma_addr_t setup_dma;
    int start_frame;
    int number_of_packets;
    int interval;
    int error_count;
    void* context;
    usb_complete_t complete;
    /* LinuxKPI private */
    struct urb* lkpi_next;     /* endpoint queue */
    volatile int lkpi_state;   /* 0 idle, 1 queued, 2 running */
    struct usb_iso_packet_descriptor iso_frame_desc[];
};

struct urb* usb_alloc_urb(int iso_packets, gfp_t mem_flags);
void usb_free_urb(struct urb* urb);
void usb_init_urb(struct urb* urb);
static inline struct urb* usb_get_urb(struct urb* urb) { if (urb) kref_get(&urb->kref); return urb; }
int usb_submit_urb(struct urb* urb, gfp_t mem_flags);
int usb_unlink_urb(struct urb* urb);
void usb_kill_urb(struct urb* urb);
void usb_poison_urb(struct urb* urb);
void usb_unpoison_urb(struct urb* urb);
void usb_anchor_urb(struct urb* urb, struct usb_anchor* anchor);
void usb_unanchor_urb(struct urb* urb);
void usb_kill_anchored_urbs(struct usb_anchor* anchor);
void usb_poison_anchored_urbs(struct usb_anchor* anchor);
void usb_unpoison_anchored_urbs(struct usb_anchor* anchor);
void usb_unlink_anchored_urbs(struct usb_anchor* anchor);
struct urb* usb_get_from_anchor(struct usb_anchor* anchor);
void usb_scuttle_anchored_urbs(struct usb_anchor* anchor);
int usb_anchor_empty(struct usb_anchor* anchor);
int usb_wait_anchor_empty_timeout(struct usb_anchor* anchor, unsigned int timeout);

static inline void usb_fill_control_urb(struct urb* urb, struct usb_device* dev, unsigned int pipe, unsigned char* setup_packet,
                                        void* transfer_buffer, int buffer_length, usb_complete_t complete_fn, void* context) {
    urb->dev = dev;
    urb->pipe = pipe;
    urb->setup_packet = setup_packet;
    urb->transfer_buffer = transfer_buffer;
    urb->transfer_buffer_length = buffer_length;
    urb->complete = complete_fn;
    urb->context = context;
}
static inline void usb_fill_bulk_urb(struct urb* urb, struct usb_device* dev, unsigned int pipe, void* transfer_buffer,
                                     int buffer_length, usb_complete_t complete_fn, void* context) {
    urb->dev = dev;
    urb->pipe = pipe;
    urb->transfer_buffer = transfer_buffer;
    urb->transfer_buffer_length = buffer_length;
    urb->complete = complete_fn;
    urb->context = context;
}
static inline void usb_fill_int_urb(struct urb* urb, struct usb_device* dev, unsigned int pipe, void* transfer_buffer,
                                    int buffer_length, usb_complete_t complete_fn, void* context, int interval) {
    usb_fill_bulk_urb(urb, dev, pipe, transfer_buffer, buffer_length, complete_fn, context);
    urb->interval = interval;
    urb->start_frame = -1;
}
static inline int usb_urb_dir_in(struct urb* urb) { return usb_pipein(urb->pipe); }
static inline int usb_urb_dir_out(struct urb* urb) { return !usb_pipein(urb->pipe); }

/* Synchronous transfers. timeout in ms (0 = default). */
#define USB_CTRL_GET_TIMEOUT 5000
#define USB_CTRL_SET_TIMEOUT 5000
int usb_control_msg(struct usb_device* dev, unsigned int pipe, __u8 request, __u8 requesttype, __u16 value, __u16 index,
                    void* data, __u16 size, int timeout);
int usb_control_msg_recv(struct usb_device* dev, __u8 endpoint, __u8 request, __u8 requesttype, __u16 value, __u16 index,
                         void* data, __u16 size, int timeout, gfp_t memflags);
int usb_control_msg_send(struct usb_device* dev, __u8 endpoint, __u8 request, __u8 requesttype, __u16 value, __u16 index,
                         const void* data, __u16 size, int timeout, gfp_t memflags);
int usb_bulk_msg(struct usb_device* usb_dev, unsigned int pipe, void* data, int len, int* actual_length, int timeout);
int usb_interrupt_msg(struct usb_device* usb_dev, unsigned int pipe, void* data, int len, int* actual_length, int timeout);
int usb_string(struct usb_device* dev, int index, char* buf, size_t size);
int usb_get_descriptor(struct usb_device* dev, unsigned char desctype, unsigned char descindex, void* buf, int size);
int usb_set_interface(struct usb_device* dev, int ifnum, int alternate);
int usb_clear_halt(struct usb_device* dev, int pipe);
int usb_reset_device(struct usb_device* dev);
#define usb_lock_device_for_reset(udev, iface) 0
#define usb_unlock_device(udev) do { } while (0)
void* usb_alloc_coherent(struct usb_device* dev, size_t size, gfp_t mem_flags, dma_addr_t* dma);
void usb_free_coherent(struct usb_device* dev, size_t size, void* addr, dma_addr_t dma);
static inline int usb_autopm_get_interface(struct usb_interface* intf) { return 0; }
static inline void usb_autopm_put_interface(struct usb_interface* intf) {}
static inline void usb_autopm_get_interface_no_resume(struct usb_interface* intf) {}
static inline void usb_autopm_put_interface_async(struct usb_interface* intf) {}
static inline void usb_enable_autosuspend(struct usb_device* udev) {}
static inline void usb_disable_autosuspend(struct usb_device* udev) {}
static inline void usb_mark_last_busy(struct usb_device* udev) {}
static inline int usb_driver_claim_interface(struct usb_driver* driver, struct usb_interface* iface, void* data) { return 0; }
static inline void usb_driver_release_interface(struct usb_driver* driver, struct usb_interface* iface) {}
void usb_queue_reset_device(struct usb_interface* dev);

/* Drivers */
struct usb_dynids { struct list_head list; };
struct usbdrv_wrap { struct device_driver driver; int for_devices; };

struct usb_driver {
    const char* name;
    int (*probe)(struct usb_interface* intf, const struct usb_device_id* id);
    void (*disconnect)(struct usb_interface* intf);
    int (*unlocked_ioctl)(struct usb_interface* intf, unsigned int code, void* buf);
    int (*suspend)(struct usb_interface* intf, pm_message_t message);
    int (*resume)(struct usb_interface* intf);
    int (*reset_resume)(struct usb_interface* intf);
    int (*pre_reset)(struct usb_interface* intf);
    int (*post_reset)(struct usb_interface* intf);
    const struct usb_device_id* id_table;
    const struct attribute_group** dev_groups;
    struct usb_dynids dynids;
    struct usbdrv_wrap drvwrap;
    unsigned int no_dynamic_id : 1;
    unsigned int supports_autosuspend : 1;
    unsigned int disable_hub_initiated_lpm : 1;
    unsigned int soft_unbind : 1;
    struct usb_driver* lkpi_next;
    struct device_driver driver; /* appended: Linux 6.8+ name of drvwrap.driver */
};
#define to_usb_driver(d) container_of(d, struct usb_driver, drvwrap.driver)

int usb_register_driver(struct usb_driver* driver, struct module* owner, const char* mod_name);
#define usb_register(driver) usb_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)
void usb_deregister(struct usb_driver* driver);
#define module_usb_driver(__usb_driver) \
    static int __init __usb_driver##_lkpi_init(void) { return usb_register(&(__usb_driver)); } \
    module_init(__usb_driver##_lkpi_init)

const struct usb_device_id* usb_match_id(struct usb_interface* interface, const struct usb_device_id* id);

/* Device id tables */
#define USB_DEVICE_ID_MATCH_VENDOR 0x0001
#define USB_DEVICE_ID_MATCH_PRODUCT 0x0002
#define USB_DEVICE_ID_MATCH_DEV_LO 0x0004
#define USB_DEVICE_ID_MATCH_DEV_HI 0x0008
#define USB_DEVICE_ID_MATCH_DEV_CLASS 0x0010
#define USB_DEVICE_ID_MATCH_DEV_SUBCLASS 0x0020
#define USB_DEVICE_ID_MATCH_DEV_PROTOCOL 0x0040
#define USB_DEVICE_ID_MATCH_INT_CLASS 0x0080
#define USB_DEVICE_ID_MATCH_INT_SUBCLASS 0x0100
#define USB_DEVICE_ID_MATCH_INT_PROTOCOL 0x0200
#define USB_DEVICE_ID_MATCH_INT_NUMBER 0x0400
#define USB_DEVICE_ID_MATCH_DEVICE (USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT)
#define USB_DEVICE_ID_MATCH_DEV_RANGE (USB_DEVICE_ID_MATCH_DEV_LO | USB_DEVICE_ID_MATCH_DEV_HI)
#define USB_DEVICE_ID_MATCH_DEVICE_AND_VERSION (USB_DEVICE_ID_MATCH_DEVICE | USB_DEVICE_ID_MATCH_DEV_RANGE)
#define USB_DEVICE_ID_MATCH_DEV_INFO (USB_DEVICE_ID_MATCH_DEV_CLASS | USB_DEVICE_ID_MATCH_DEV_SUBCLASS | USB_DEVICE_ID_MATCH_DEV_PROTOCOL)
#define USB_DEVICE_ID_MATCH_INT_INFO (USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS | USB_DEVICE_ID_MATCH_INT_PROTOCOL)
#define USB_DEVICE(vend, prod) .match_flags = USB_DEVICE_ID_MATCH_DEVICE, .idVendor = (vend), .idProduct = (prod)
#define USB_DEVICE_VER(vend, prod, lo, hi) .match_flags = USB_DEVICE_ID_MATCH_DEVICE_AND_VERSION, .idVendor = (vend), \
    .idProduct = (prod), .bcdDevice_lo = (lo), .bcdDevice_hi = (hi)
#define USB_DEVICE_INTERFACE_CLASS(vend, prod, cl) .match_flags = USB_DEVICE_ID_MATCH_DEVICE | USB_DEVICE_ID_MATCH_INT_CLASS, \
    .idVendor = (vend), .idProduct = (prod), .bInterfaceClass = (cl)
#define USB_DEVICE_INTERFACE_PROTOCOL(vend, prod, pr) .match_flags = USB_DEVICE_ID_MATCH_DEVICE | USB_DEVICE_ID_MATCH_INT_PROTOCOL, \
    .idVendor = (vend), .idProduct = (prod), .bInterfaceProtocol = (pr)
#define USB_DEVICE_INTERFACE_NUMBER(vend, prod, num) .match_flags = USB_DEVICE_ID_MATCH_DEVICE | USB_DEVICE_ID_MATCH_INT_NUMBER, \
    .idVendor = (vend), .idProduct = (prod), .bInterfaceNumber = (num)
#define USB_DEVICE_INFO(cl, sc, pr) .match_flags = USB_DEVICE_ID_MATCH_DEV_INFO, .bDeviceClass = (cl), \
    .bDeviceSubClass = (sc), .bDeviceProtocol = (pr)
#define USB_INTERFACE_INFO(cl, sc, pr) .match_flags = USB_DEVICE_ID_MATCH_INT_INFO, .bInterfaceClass = (cl), \
    .bInterfaceSubClass = (sc), .bInterfaceProtocol = (pr)
#define USB_DEVICE_AND_INTERFACE_INFO(vend, prod, cl, sc, pr) \
    .match_flags = USB_DEVICE_ID_MATCH_INT_INFO | USB_DEVICE_ID_MATCH_DEVICE, .idVendor = (vend), .idProduct = (prod), \
    .bInterfaceClass = (cl), .bInterfaceSubClass = (sc), .bInterfaceProtocol = (pr)
#define USB_VENDOR_AND_INTERFACE_INFO(vend, cl, sc, pr) \
    .match_flags = USB_DEVICE_ID_MATCH_INT_INFO | USB_DEVICE_ID_MATCH_VENDOR, .idVendor = (vend), \
    .bInterfaceClass = (cl), .bInterfaceSubClass = (sc), .bInterfaceProtocol = (pr)

LKPI_EXTERN_C_END

#endif
