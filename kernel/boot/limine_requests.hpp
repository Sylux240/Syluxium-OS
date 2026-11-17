#pragma once

#include <lib/types.hpp>
#include <limine.h>

// Information the bootloader filled in for us. Pointers are nullptr if the
// bootloader didn't answer a request.
namespace boot {

bool base_revision_supported();

limine_framebuffer_response* framebuffers();
limine_memmap_response* memory_map();
u64 hhdm_offset(); // virtual = physical + hhdm_offset (higher-half direct map)
limine_executable_address_response* kernel_address();
void* rsdp();      // ACPI root table pointer (virtual)
limine_mp_response* mp();
limine_module_response* modules(); // files loaded next to the kernel (initramfs)

// Called once bootloader-reclaimable memory is handed to the page allocator:
// every response lives there, so all accessors return nullptr afterwards.
const char* cmdline(); // kernel command line ("" if none); valid after release()
void release();
bool released();

} // namespace boot
