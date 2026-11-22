// LinuxKPI USB: the Syluxium side of struct usb_device (glue/usb.c).
// usb.cpp's offer() asks usb_probe() whether a Linux USB driver wants an
// interface; transfers from Linux drivers end up in the usb::Device API.

#include <dev/device.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/format.hpp>
#include <linuxkpi/linuxkpi.hpp>

extern "C" {

int lkpi_usb_offer(void* sylux, const u8* config, unsigned int config_len, u8 ifnum, int speed, const char* path,
                   const char* product, void* registry, const char** driver_name);
void lkpi_usb_disconnect(void* udev);

long lkpi_sylux_usb_control(void* d, u8 request_type, u8 request, u16 value, u16 index, void* data, u16 length) {
    return static_cast<usb::Device*>(d)->control(request_type, request, value, index, data, length);
}

long lkpi_sylux_usb_transfer(void* d, u8 ep_address, void* data, u32 length, int wait_forever) {
    auto* dev = static_cast<usb::Device*>(d);
    return wait_forever ? dev->transfer_wait(ep_address, data, length) : dev->transfer(ep_address, data, length);
}

long lkpi_sylux_usb_open_endpoint(void* d, u8 address, u8 attributes, u16 max_packet, u8 interval) {
    usb::Endpoint e{address, attributes, max_packet, interval};
    return static_cast<usb::Device*>(d)->open_endpoint(e);
}

void lkpi_sylux_usb_cancel(void* d, u8 ep_address) {
    static_cast<usb::Device*>(d)->cancel(ep_address);
}

// A deferred probe bound later: the device list shows the driver.
void lkpi_sylux_usb_bound(void* registry, const char* driver_name) {
    if (auto* e = static_cast<dev::Device*>(registry)) {
        e->category = dev::Category::Network;
        e->driver_name = driver_name;
    }
}

// A Linux driver matched an interface and its probe failed.
void lkpi_sylux_usb_failed(void* registry, const char* driver_name, int error) {
    if (auto* e = static_cast<dev::Device*>(registry)) {
        if (e->driver_name)
            return; // another interface is bound
        e->status = dev::Status::Error;
        snprintf(e->status_text, sizeof(e->status_text), "%s failed to start (error %d)", driver_name, -error);
    }
}

void lkpi_sylux_usb_add_binding(void* d, void* udev) {
    static_cast<usb::Device*>(d)->add_binding([](void* ctx) { lkpi_usb_disconnect(ctx); }, udev);
}

} // extern "C"

extern "C" void lkpi_usb_retry_deferred(void);

namespace linuxkpi {

void usb_retry_deferred() { lkpi_usb_retry_deferred(); }

bool usb_probe(usb::Device* d, const usb::Interface& intf, const char** driver_name) {
    return lkpi_usb_offer(d, d->config_desc, d->config_len, intf.number, d->speed, d->path, d->product_name, d->entry,
                          driver_name) != 0;
}

} // namespace linuxkpi
