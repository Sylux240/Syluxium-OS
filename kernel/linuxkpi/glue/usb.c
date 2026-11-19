// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI: USB drivers (struct usb_driver, usb_device, usb_interface, URBs)
 * on Syluxium's USB core.
 *
 * Syluxium's usb::Device offers synchronous transfers (one at a time per
 * endpoint). Linux drivers submit URBs asynchronously and get a completion
 * callback; so every endpoint a driver uses gets a worker thread that takes
 * the endpoint's queued URBs one by one, runs the transfer (waiting as long
 * as it takes for IN data) and calls urb->complete() afterwards - from that
 * thread, which is a superset of what Linux's interrupt-context
 * completions allow. usb_kill_urb() cancels the transfer in progress
 * (usb::Device::cancel) and waits for the completion with -ENOENT.
 */
#include <linux/lkpi_all.h>
#include <linux/kthread.h>
#include <linux/usb.h>

/* ---- Syluxium side (usb.cpp) ---- */
long lkpi_sylux_usb_control(void* d, u8 request_type, u8 request, u16 value, u16 index, void* data, u16 length);
long lkpi_sylux_usb_transfer(void* d, u8 ep_address, void* data, u32 length, int wait_forever);
long lkpi_sylux_usb_open_endpoint(void* d, u8 address, u8 attributes, u16 max_packet, u8 interval);
void lkpi_sylux_usb_cancel(void* d, u8 ep_address);
void lkpi_sylux_usb_add_binding(void* d, struct usb_device* udev);

static struct usb_driver* drivers;
static DEFINE_MUTEX(usb_glue_lock);

/* ---- endpoint workers ---- */

struct ep_worker {
	struct usb_device* udev;
	u8 address;            /* bit 7: IN; 0 = control endpoint */
	spinlock_t lock;
	struct urb* head;      /* queue */
	struct urb** tail;
	struct urb* running;
	wait_queue_head_t wait; /* new work */
	wait_queue_head_t idle; /* an URB finished */
};

static int ep_index(unsigned int pipe)
{
	return usb_pipeendpoint(pipe) | (usb_pipein(pipe) && !usb_pipecontrol(pipe) ? 16 : 0);
}

static void run_urb(struct ep_worker* w, struct urb* urb)
{
	struct usb_device* udev = w->udev;
	long r;

	if (udev->lkpi_gone) {
		urb->status = -ENODEV;
		return;
	}
	if (usb_pipecontrol(urb->pipe)) {
		struct usb_ctrlrequest* setup = (struct usb_ctrlrequest*)urb->setup_packet;
		r = lkpi_sylux_usb_control(udev->lkpi_sylux, setup->bRequestType, setup->bRequest,
					   le16_to_cpu(setup->wValue), le16_to_cpu(setup->wIndex), urb->transfer_buffer,
					   le16_to_cpu(setup->wLength));
	} else {
		bool in = usb_pipein(urb->pipe);
		u8 ep = usb_pipeendpoint(urb->pipe) | (in ? USB_DIR_IN : 0);
		u32 len = urb->transfer_buffer_length, done = 0;
		r = 0;
		if (in) {
			r = lkpi_sylux_usb_transfer(udev->lkpi_sylux, ep, urb->transfer_buffer, len, 1);
			done = r > 0 ? r : 0;
		} else {
			/* Syluxium's bounce buffers hold 64 KiB per transfer. */
			while (done < len) {
				u32 n = min_t(u32, len - done, 65536);
				r = lkpi_sylux_usb_transfer(udev->lkpi_sylux, ep, (u8*)urb->transfer_buffer + done, n, 0);
				if (r < 0)
					break;
				done += r;
			}
			if (r >= 0 && (urb->transfer_flags & URB_ZERO_PACKET) && len) {
				u16 maxp = usb_maxpacket(udev, urb->pipe);
				if (maxp && len % maxp == 0)
					lkpi_sylux_usb_transfer(udev->lkpi_sylux, ep, urb->transfer_buffer, 0, 0);
			}
		}
		if (r >= 0)
			r = done;
	}
	if (urb->unlinked) {
		urb->status = urb->unlinked;
	} else if (r < 0) {
		urb->status = r == -ENODEV ? -ENODEV : r == -ETIMEDOUT ? -ETIMEDOUT : -EPROTO;
		if (udev->lkpi_gone)
			urb->status = -ENODEV;
	} else {
		urb->status = 0;
		urb->actual_length = r;
		if ((urb->transfer_flags & URB_SHORT_NOT_OK) && (u32)r < urb->transfer_buffer_length && usb_pipein(urb->pipe))
			urb->status = -EREMOTEIO;
	}
}

static int ep_thread(void* arg)
{
	struct ep_worker* w = arg;
	for (;;) {
		struct urb* urb;
		unsigned long flags;

		wait_event(w->wait, READ_ONCE(w->head) != NULL);
		spin_lock_irqsave(&w->lock, flags);
		urb = w->head;
		if (urb) {
			w->head = urb->lkpi_next;
			if (!w->head)
				w->tail = &w->head;
			urb->lkpi_next = NULL;
			w->running = urb;
			urb->lkpi_state = 2;
		}
		spin_unlock_irqrestore(&w->lock, flags);
		if (!urb)
			continue;

		run_urb(w, urb);

		if (urb->anchor)
			usb_unanchor_urb(urb);
		urb->lkpi_state = 0;
		spin_lock_irqsave(&w->lock, flags);
		w->running = NULL;
		spin_unlock_irqrestore(&w->lock, flags);
		atomic_dec(&urb->use_count);
		if (urb->complete)
			urb->complete(urb);
		wake_up(&w->idle);
		usb_free_urb(urb); /* the reference usb_submit_urb() took */
	}
	return 0;
}

static struct ep_worker* get_worker(struct usb_device* udev, unsigned int pipe)
{
	int idx = ep_index(pipe);
	struct ep_worker* w = udev->lkpi_eps[idx];
	if (w)
		return w;
	mutex_lock(&usb_glue_lock);
	w = udev->lkpi_eps[idx];
	if (!w) {
		char name[24];
		w = kzalloc(sizeof(*w), GFP_KERNEL);
		if (w) {
			w->udev = udev;
			w->address = usb_pipeendpoint(pipe) | (usb_pipein(pipe) ? USB_DIR_IN : 0);
			spin_lock_init(&w->lock);
			w->tail = &w->head;
			init_waitqueue_head(&w->wait);
			init_waitqueue_head(&w->idle);
			snprintf(name, sizeof(name), "usb-ep%02x", w->address);
			kthread_run(ep_thread, w, name);
			WRITE_ONCE(udev->lkpi_eps[idx], w);
		}
	}
	mutex_unlock(&usb_glue_lock);
	return w;
}

/* ---- URBs ---- */

struct urb* usb_alloc_urb(int iso_packets, gfp_t mem_flags)
{
	struct urb* urb = kzalloc(sizeof(*urb) + iso_packets * sizeof(struct usb_iso_packet_descriptor), mem_flags);
	if (urb)
		usb_init_urb(urb);
	return urb;
}

void usb_init_urb(struct urb* urb)
{
	memset(urb, 0, sizeof(*urb));
	kref_init(&urb->kref);
	INIT_LIST_HEAD(&urb->urb_list);
	INIT_LIST_HEAD(&urb->anchor_list);
}

static void urb_destroy(struct kref* kref)
{
	struct urb* urb = container_of(kref, struct urb, kref);
	if (urb->transfer_flags & URB_FREE_BUFFER)
		kfree(urb->transfer_buffer);
	kfree(urb);
}

void usb_free_urb(struct urb* urb)
{
	if (urb)
		kref_put(&urb->kref, urb_destroy);
}

int usb_submit_urb(struct urb* urb, gfp_t mem_flags)
{
	struct ep_worker* w;
	unsigned long flags;

	if (!urb || !urb->complete)
		return -EINVAL;
	if (!urb->dev || urb->dev->lkpi_gone)
		return -ENODEV;
	if (atomic_read(&urb->reject))
		return -EPERM;
	if (urb->lkpi_state)
		return -EBUSY;
	w = get_worker(urb->dev, urb->pipe);
	if (!w)
		return -ENOMEM;
	usb_get_urb(urb);
	urb->status = -EINPROGRESS;
	urb->actual_length = 0;
	urb->unlinked = 0;
	urb->lkpi_state = 1;
	atomic_inc(&urb->use_count);
	spin_lock_irqsave(&w->lock, flags);
	urb->lkpi_next = NULL;
	*w->tail = urb;
	w->tail = &urb->lkpi_next;
	spin_unlock_irqrestore(&w->lock, flags);
	wake_up(&w->wait);
	return 0;
}

/* Take a queued URB out, or abort the running one. Returns true if the
 * caller must complete it (it was still queued). */
static bool unlink(struct urb* urb, int status)
{
	struct ep_worker* w;
	unsigned long flags;
	struct urb** pp;
	bool dequeued = false, running = false;

	if (!urb->dev || !urb->lkpi_state)
		return false;
	w = urb->dev->lkpi_eps[ep_index(urb->pipe)];
	if (!w)
		return false;
	spin_lock_irqsave(&w->lock, flags);
	for (pp = &w->head; *pp; pp = &(*pp)->lkpi_next) {
		if (*pp == urb) {
			*pp = urb->lkpi_next;
			if (!*pp)
				w->tail = pp;
			urb->lkpi_next = NULL;
			dequeued = true;
			break;
		}
	}
	if (!dequeued && w->running == urb) {
		urb->unlinked = status;
		running = true;
	}
	spin_unlock_irqrestore(&w->lock, flags);
	if (running && !usb_pipecontrol(urb->pipe))
		lkpi_sylux_usb_cancel(urb->dev->lkpi_sylux, w->address);
	return dequeued;
}

static void complete_unlinked(struct urb* urb, int status)
{
	urb->status = status;
	urb->lkpi_state = 0;
	atomic_dec(&urb->use_count);
	if (urb->anchor)
		usb_unanchor_urb(urb);
	if (urb->complete)
		urb->complete(urb);
	usb_free_urb(urb);
}

int usb_unlink_urb(struct urb* urb)
{
	if (!urb)
		return -EINVAL;
	if (unlink(urb, -ECONNRESET))
		complete_unlinked(urb, -ECONNRESET);
	return -EINPROGRESS;
}

void usb_kill_urb(struct urb* urb)
{
	struct ep_worker* w;
	if (!urb || !urb->dev)
		return;
	atomic_inc(&urb->reject);
	if (unlink(urb, -ENOENT))
		complete_unlinked(urb, -ENOENT);
	w = urb->dev->lkpi_eps[ep_index(urb->pipe)];
	if (w)
		wait_event(w->idle, READ_ONCE(urb->lkpi_state) == 0);
	atomic_dec(&urb->reject);
}

void usb_poison_urb(struct urb* urb)
{
	if (!urb)
		return;
	usb_kill_urb(urb);
	atomic_inc(&urb->reject);
}

void usb_unpoison_urb(struct urb* urb)
{
	if (urb && atomic_read(&urb->reject) > 0)
		atomic_dec(&urb->reject);
}

/* ---- anchors ---- */

void usb_anchor_urb(struct urb* urb, struct usb_anchor* anchor)
{
	unsigned long flags;
	spin_lock_irqsave(&anchor->lock, flags);
	usb_get_urb(urb);
	list_add_tail(&urb->anchor_list, &anchor->urb_list);
	urb->anchor = anchor;
	if (anchor->poisoned)
		atomic_inc(&urb->reject);
	spin_unlock_irqrestore(&anchor->lock, flags);
}

void usb_unanchor_urb(struct urb* urb)
{
	struct usb_anchor* anchor = urb->anchor;
	unsigned long flags;
	if (!anchor)
		return;
	spin_lock_irqsave(&anchor->lock, flags);
	if (urb->anchor == anchor) {
		list_del_init(&urb->anchor_list);
		urb->anchor = NULL;
		spin_unlock_irqrestore(&anchor->lock, flags);
		wake_up(&anchor->wait);
		usb_free_urb(urb);
		return;
	}
	spin_unlock_irqrestore(&anchor->lock, flags);
}

struct urb* usb_get_from_anchor(struct usb_anchor* anchor)
{
	struct urb* urb = NULL;
	unsigned long flags;
	spin_lock_irqsave(&anchor->lock, flags);
	if (!list_empty(&anchor->urb_list)) {
		urb = list_first_entry(&anchor->urb_list, struct urb, anchor_list);
		usb_get_urb(urb);
	}
	spin_unlock_irqrestore(&anchor->lock, flags);
	if (urb) {
		usb_unanchor_urb(urb);
		/* the anchor's reference is gone; the caller owns ours */
	}
	return urb;
}

static struct urb* anchor_last(struct usb_anchor* anchor)
{
	struct urb* urb = NULL;
	unsigned long flags;
	spin_lock_irqsave(&anchor->lock, flags);
	if (!list_empty(&anchor->urb_list)) {
		urb = list_last_entry(&anchor->urb_list, struct urb, anchor_list);
		usb_get_urb(urb);
	}
	spin_unlock_irqrestore(&anchor->lock, flags);
	return urb;
}

void usb_kill_anchored_urbs(struct usb_anchor* anchor)
{
	struct urb* urb;
	int guard = 0;
	while ((urb = anchor_last(anchor)) != NULL && guard++ < 10000) {
		usb_kill_urb(urb);
		if (urb->anchor == anchor) /* never submitted: just drop it */
			usb_unanchor_urb(urb);
		usb_free_urb(urb);
	}
}

void usb_poison_anchored_urbs(struct usb_anchor* anchor)
{
	anchor->poisoned = 1;
	usb_kill_anchored_urbs(anchor);
}

void usb_unpoison_anchored_urbs(struct usb_anchor* anchor)
{
	anchor->poisoned = 0;
}

void usb_unlink_anchored_urbs(struct usb_anchor* anchor)
{
	usb_kill_anchored_urbs(anchor);
}

void usb_scuttle_anchored_urbs(struct usb_anchor* anchor)
{
	struct urb* urb;
	while ((urb = usb_get_from_anchor(anchor)) != NULL)
		usb_free_urb(urb);
}

int usb_anchor_empty(struct usb_anchor* anchor)
{
	return list_empty(&anchor->urb_list);
}

int usb_wait_anchor_empty_timeout(struct usb_anchor* anchor, unsigned int timeout)
{
	return wait_event_timeout(anchor->wait, usb_anchor_empty(anchor), msecs_to_jiffies(timeout));
}

/* ---- synchronous transfers ---- */

int usb_control_msg(struct usb_device* dev, unsigned int pipe, __u8 request, __u8 requesttype, __u16 value,
		    __u16 index, void* data, __u16 size, int timeout)
{
	long r;
	if (dev->lkpi_gone)
		return -ENODEV;
	r = lkpi_sylux_usb_control(dev->lkpi_sylux, requesttype, request, value, index, data, size);
	if (r < 0)
		return r == -ENODEV ? -ENODEV : r == -ETIMEDOUT ? -ETIMEDOUT : -EPIPE;
	return (int)r;
}

int usb_control_msg_recv(struct usb_device* dev, __u8 endpoint, __u8 request, __u8 requesttype, __u16 value,
			 __u16 index, void* data, __u16 size, int timeout, gfp_t memflags)
{
	int r = usb_control_msg(dev, usb_rcvctrlpipe(dev, endpoint), request, requesttype, value, index, data, size, timeout);
	if (r < 0)
		return r;
	return r == size ? 0 : -EREMOTEIO;
}

int usb_control_msg_send(struct usb_device* dev, __u8 endpoint, __u8 request, __u8 requesttype, __u16 value,
			 __u16 index, const void* data, __u16 size, int timeout, gfp_t memflags)
{
	int r = usb_control_msg(dev, usb_sndctrlpipe(dev, endpoint), request, requesttype, value, index, (void*)data,
				size, timeout);
	return r < 0 ? r : 0;
}

static void sync_complete(struct urb* urb)
{
	complete((struct completion*)urb->context);
}

int usb_bulk_msg(struct usb_device* usb_dev, unsigned int pipe, void* data, int len, int* actual_length, int timeout)
{
	struct completion done;
	struct urb* urb;
	int r;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;
	init_completion(&done);
	usb_fill_bulk_urb(urb, usb_dev, pipe, data, len, sync_complete, &done);
	r = usb_submit_urb(urb, GFP_KERNEL);
	if (r) {
		usb_free_urb(urb);
		return r;
	}
	if (!wait_for_completion_timeout(&done, timeout ? msecs_to_jiffies(timeout) : MAX_SCHEDULE_TIMEOUT)) {
		usb_kill_urb(urb);
		r = -ETIMEDOUT;
	} else {
		r = urb->status;
	}
	if (actual_length)
		*actual_length = urb->actual_length;
	usb_free_urb(urb);
	return r;
}

int usb_interrupt_msg(struct usb_device* usb_dev, unsigned int pipe, void* data, int len, int* actual_length, int timeout)
{
	return usb_bulk_msg(usb_dev, pipe, data, len, actual_length, timeout);
}

int usb_get_descriptor(struct usb_device* dev, unsigned char desctype, unsigned char descindex, void* buf, int size)
{
	return usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
			       (desctype << 8) + descindex, 0, buf, size, USB_CTRL_GET_TIMEOUT);
}

int usb_string(struct usb_device* dev, int index, char* buf, size_t size)
{
	u8 raw[256];
	int r, i, n = 0;
	if (!index || size == 0)
		return -EINVAL;
	r = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), USB_REQ_GET_DESCRIPTOR, USB_DIR_IN, (USB_DT_STRING << 8) | index,
			    0x0409, raw, sizeof(raw), USB_CTRL_GET_TIMEOUT);
	if (r < 2)
		return r < 0 ? r : -EINVAL;
	for (i = 2; i + 1 < r && i < raw[0] && n + 1 < (int)size; i += 2)
		buf[n++] = raw[i + 1] ? '?' : raw[i];
	buf[n] = 0;
	return n;
}

int usb_set_interface(struct usb_device* dev, int ifnum, int alternate)
{
	if (alternate == 0)
		return 0;
	return usb_control_msg(dev, usb_sndctrlpipe(dev, 0), USB_REQ_SET_INTERFACE, USB_RECIP_INTERFACE, alternate, ifnum,
			       NULL, 0, 5000);
}

int usb_clear_halt(struct usb_device* dev, int pipe)
{
	int endp = usb_pipeendpoint(pipe) | (usb_pipein(pipe) ? USB_DIR_IN : 0);
	return usb_control_msg(dev, usb_sndctrlpipe(dev, 0), USB_REQ_CLEAR_FEATURE, USB_RECIP_ENDPOINT, USB_ENDPOINT_HALT,
			       endp, NULL, 0, 5000);
}

int usb_reset_device(struct usb_device* dev)
{
	return 0; /* freshly enumerated by Syluxium's USB core */
}

void usb_queue_reset_device(struct usb_interface* intf)
{
}

void* usb_alloc_coherent(struct usb_device* dev, size_t size, gfp_t mem_flags, dma_addr_t* dma)
{
	void* p = kzalloc(size, mem_flags);
	if (p && dma)
		*dma = 0;
	return p;
}

void usb_free_coherent(struct usb_device* dev, size_t size, void* addr, dma_addr_t dma)
{
	kfree(addr);
}

struct usb_interface* usb_ifnum_to_if(const struct usb_device* dev, unsigned ifnum)
{
	for (int i = 0; i < dev->lkpi_nintf; i++)
		if (dev->lkpi_intfs[i].cur_altsetting->desc.bInterfaceNumber == ifnum)
			return (struct usb_interface*)&dev->lkpi_intfs[i];
	return NULL;
}

/* ---- drivers and matching ---- */

int usb_register_driver(struct usb_driver* driver, struct module* owner, const char* mod_name)
{
	mutex_lock(&usb_glue_lock);
	driver->lkpi_next = drivers;
	drivers = driver;
	driver->drvwrap.driver.name = driver->name;
	mutex_unlock(&usb_glue_lock);
	return 0;
}

void usb_deregister(struct usb_driver* driver)
{
	struct usb_driver** pp;
	mutex_lock(&usb_glue_lock);
	for (pp = &drivers; *pp; pp = &(*pp)->lkpi_next)
		if (*pp == driver) {
			*pp = driver->lkpi_next;
			break;
		}
	mutex_unlock(&usb_glue_lock);
}

static bool id_matches(struct usb_device* dev, struct usb_interface* intf, const struct usb_device_id* id)
{
	const struct usb_device_descriptor* d = &dev->descriptor;
	const struct usb_interface_descriptor* i = &intf->cur_altsetting->desc;
	u16 bcd = le16_to_cpu(d->bcdDevice);

	if ((id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) && id->idVendor != le16_to_cpu(d->idVendor))
		return false;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT) && id->idProduct != le16_to_cpu(d->idProduct))
		return false;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_LO) && id->bcdDevice_lo > bcd)
		return false;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_HI) && id->bcdDevice_hi < bcd)
		return false;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_CLASS) && id->bDeviceClass != d->bDeviceClass)
		return false;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_SUBCLASS) && id->bDeviceSubClass != d->bDeviceSubClass)
		return false;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_PROTOCOL) && id->bDeviceProtocol != d->bDeviceProtocol)
		return false;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_CLASS) && id->bInterfaceClass != i->bInterfaceClass)
		return false;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_SUBCLASS) && id->bInterfaceSubClass != i->bInterfaceSubClass)
		return false;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_PROTOCOL) && id->bInterfaceProtocol != i->bInterfaceProtocol)
		return false;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_NUMBER) && id->bInterfaceNumber != i->bInterfaceNumber)
		return false;
	return true;
}

const struct usb_device_id* usb_match_id(struct usb_interface* interface, const struct usb_device_id* id)
{
	if (!id)
		return NULL;
	for (; id->idVendor || id->idProduct || id->bDeviceClass || id->bInterfaceClass || id->driver_info; id++)
		if (id_matches(interface->lkpi_udev, interface, id))
			return id;
	return NULL;
}

/* ---- device creation (from Syluxium's descriptors) ---- */

#define MAX_UDEVS 16
static struct usb_device* udevs[MAX_UDEVS];

static struct usb_device* udev_for(void* sylux, const u8* config, unsigned int config_len, int speed, const char* path,
				   const char* product, void* registry)
{
	struct usb_device* udev;
	int slot = -1, i, nintf = 0, nep = 0;
	unsigned int off;
	struct usb_host_interface* cur = NULL;
	long r;

	for (i = 0; i < MAX_UDEVS; i++) {
		if (udevs[i] && udevs[i]->lkpi_sylux == sylux)
			return udevs[i];
		if (!udevs[i] && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return NULL;
	udev = kzalloc(sizeof(*udev), GFP_KERNEL);
	if (!udev)
		return NULL;
	udev->lkpi_sylux = sylux;
	r = lkpi_sylux_usb_control(sylux, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &udev->descriptor,
				   sizeof(udev->descriptor));
	if (r < (long)sizeof(udev->descriptor)) {
		kfree(udev);
		return NULL;
	}
	udev->devnum = slot + 1;
	udev->speed = speed == 2 ? USB_SPEED_LOW : speed == 1 ? USB_SPEED_FULL : speed == 3 ? USB_SPEED_HIGH : USB_SPEED_SUPER;
	udev->state = USB_STATE_CONFIGURED;
	strscpy(udev->devpath, path, sizeof(udev->devpath));
	strscpy(udev->lkpi_product, product, sizeof(udev->lkpi_product));
	udev->product = udev->lkpi_product;
	device_initialize(&udev->dev);
	dev_set_name(&udev->dev, "%s", path);
	udev->dev.lkpi_registry = registry;
	udev->config = udev->actconfig = &udev->lkpi_config;
	udev->ep0.desc.bLength = USB_DT_ENDPOINT_SIZE;
	udev->ep0.desc.bDescriptorType = USB_DT_ENDPOINT;
	udev->ep0.desc.wMaxPacketSize = cpu_to_le16(udev->descriptor.bMaxPacketSize0);
	udev->ep_in[0] = udev->ep_out[0] = &udev->ep0;
	if (config_len >= USB_DT_CONFIG_SIZE)
		memcpy(&udev->lkpi_config.desc, config, USB_DT_CONFIG_SIZE);

	/* Interfaces (alternate setting 0) and their endpoints. */
	for (off = 0; off + 2 <= config_len && config[off] >= 2; off += config[off]) {
		u8 len = config[off], type = config[off + 1];
		if (type == USB_DT_INTERFACE && len >= USB_DT_INTERFACE_SIZE) {
			cur = NULL;
			if (config[off + 3] != 0 || nintf >= (int)ARRAY_SIZE(udev->lkpi_intfs))
				continue;
			cur = &udev->lkpi_altsettings[nintf];
			memcpy(&cur->desc, config + off, USB_DT_INTERFACE_SIZE);
			cur->endpoint = udev->lkpi_endpoints[nintf];
			cur->desc.bNumEndpoints = 0;
			struct usb_interface* intf = &udev->lkpi_intfs[nintf];
			intf->altsetting = intf->cur_altsetting = cur;
			intf->num_altsetting = 1;
			intf->lkpi_udev = udev;
			intf->minor = -1;
			device_initialize(&intf->dev);
			intf->dev.parent = &udev->dev;
			intf->dev.lkpi_registry = registry;
			dev_set_name(&intf->dev, "%s:1.%d", path, cur->desc.bInterfaceNumber);
			udev->lkpi_config.interface[nintf] = intf;
			nintf++;
			nep = 0;
		} else if (type == USB_DT_ENDPOINT && len >= USB_DT_ENDPOINT_SIZE && cur && nep < 16) {
			struct usb_host_endpoint* ep = &cur->endpoint[nep++];
			memcpy(&ep->desc, config + off, USB_DT_ENDPOINT_SIZE);
			cur->desc.bNumEndpoints = nep;
			if (usb_endpoint_dir_in(&ep->desc))
				udev->ep_in[usb_endpoint_num(&ep->desc)] = ep;
			else
				udev->ep_out[usb_endpoint_num(&ep->desc)] = ep;
		}
	}
	udev->lkpi_nintf = nintf;
	udev->lkpi_config.desc.bNumInterfaces = nintf;
	udevs[slot] = udev;
	return udev;
}

/* Called from usb.cpp for every interface of every new device. */
int lkpi_usb_offer(void* sylux, const u8* config, unsigned int config_len, u8 ifnum, int speed, const char* path,
		   const char* product, void* registry, const char** driver_name)
{
	struct usb_device* udev;
	struct usb_interface* intf;
	struct usb_driver* drv;
	const struct usb_device_id* id = NULL;
	int r, i;

	if (!drivers)
		return 0;

	udev = udev_for(sylux, config, config_len, speed, path, product, registry);
	if (!udev)
		return 0;
	intf = usb_ifnum_to_if(udev, ifnum);
	if (!intf || intf->lkpi_driver)
		return 0;
	mutex_lock(&usb_glue_lock);
	for (drv = drivers; drv; drv = drv->lkpi_next) {
		id = usb_match_id(intf, drv->id_table);
		if (id)
			break;
	}
	mutex_unlock(&usb_glue_lock);
	if (!drv)
		return 0;

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor* d = &intf->cur_altsetting->endpoint[i].desc;
		lkpi_sylux_usb_open_endpoint(sylux, d->bEndpointAddress, d->bmAttributes, usb_endpoint_maxp(d), d->bInterval);
	}
	printk(KERN_INFO "usb %s: %04x:%04x interface %u: Linux driver %s\n", path, le16_to_cpu(udev->descriptor.idVendor),
	       le16_to_cpu(udev->descriptor.idProduct), ifnum, drv->name);
	intf->lkpi_driver = drv;
	intf->condition = USB_INTERFACE_BINDING;
	r = drv->probe(intf, id);
	if (r) {
		printk(KERN_WARNING "usb %s: %s probe failed (%d)\n", path, drv->name, r);
		intf->lkpi_driver = NULL;
		intf->condition = USB_INTERFACE_UNBOUND;
		return 0;
	}
	intf->condition = USB_INTERFACE_BOUND;
	intf->dev.driver = &drv->drvwrap.driver;
	lkpi_sylux_usb_add_binding(sylux, udev);
	*driver_name = drv->name;
	return 1;
}

/* Called from usb.cpp when the device is unplugged (after it is disabled:
 * every transfer fails at once). */
void lkpi_usb_disconnect(struct usb_device* udev)
{
	int i;
	udev->lkpi_gone = 1;
	for (i = 0; i < udev->lkpi_nintf; i++) {
		struct usb_interface* intf = &udev->lkpi_intfs[i];
		if (intf->lkpi_driver && intf->condition == USB_INTERFACE_BOUND) {
			intf->condition = USB_INTERFACE_UNBINDING;
			intf->lkpi_driver->disconnect(intf);
			intf->condition = USB_INTERFACE_UNBOUND;
			intf->lkpi_driver = NULL;
		}
	}
	for (i = 0; i < MAX_UDEVS; i++)
		if (udevs[i] == udev)
			udevs[i] = NULL;
	/* The usb_device and its endpoint workers stay allocated: URBs may
	 * still reference them. */
}
