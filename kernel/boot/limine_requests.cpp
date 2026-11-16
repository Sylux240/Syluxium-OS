#include <lib/types.hpp>

#include <boot/limine_requests.hpp>

// Limine scans the kernel image for these structures and writes its
// responses into them before jumping to kmain. They must be `volatile`
// (the compiler can't see the bootloader writing them) and `used` (so the
// linker doesn't throw them away).

namespace {

#define LIMINE_REQUEST __attribute__((used, section(".limine_requests"))) volatile

__attribute__((used, section(".limine_requests_start")))
volatile u64 requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

LIMINE_REQUEST u64 base_revision[] = LIMINE_BASE_REVISION(6);

LIMINE_REQUEST limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0, .response = nullptr};

LIMINE_REQUEST limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0, .response = nullptr};

LIMINE_REQUEST limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0, .response = nullptr};

LIMINE_REQUEST limine_executable_address_request kernel_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0, .response = nullptr};

LIMINE_REQUEST limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID, .revision = 0, .response = nullptr};

LIMINE_REQUEST limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0, .response = nullptr,
    .internal_module_count = 0, .internal_modules = nullptr};

// Starting the other CPUs happens in phase 3; asking now lets us report the count.
LIMINE_REQUEST limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID, .revision = 0, .response = nullptr, .flags = 0};

__attribute__((used, section(".limine_requests_end")))
volatile u64 requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

#undef LIMINE_REQUEST

} // namespace

namespace boot {

namespace {
bool g_released = false;
u64 g_hhdm_offset_copy = 0; // the one value still needed afterwards
} // namespace

bool base_revision_supported() { return LIMINE_BASE_REVISION_SUPPORTED(base_revision); }

limine_framebuffer_response* framebuffers() { return g_released ? nullptr : framebuffer_request.response; }
limine_memmap_response* memory_map() { return g_released ? nullptr : memmap_request.response; }
u64 hhdm_offset() {
    if (!g_released && hhdm_request.response)
        g_hhdm_offset_copy = hhdm_request.response->offset;
    return g_hhdm_offset_copy;
}
limine_executable_address_response* kernel_address() { return g_released ? nullptr : kernel_address_request.response; }
void* rsdp() { return !g_released && rsdp_request.response ? rsdp_request.response->address : nullptr; }
limine_mp_response* mp() { return g_released ? nullptr : mp_request.response; }
limine_module_response* modules() { return g_released ? nullptr : module_request.response; }

void release() {
    hhdm_offset();
    g_released = true;
}
bool released() { return g_released; }

} // namespace boot
