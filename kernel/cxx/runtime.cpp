// Minimal C++ runtime support for a freestanding kernel.

#include <core/panic.hpp>
#include <cxx/runtime.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>

extern "C" {

void* __dso_handle = nullptr;

// Destructors of global objects never run in a kernel.
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

[[noreturn]] void __cxa_pure_virtual() { panic("pure virtual function called"); }

using Constructor = void (*)();
extern Constructor __init_array_start[]; // from linker.ld
extern Constructor __init_array_end[];
}

// ---- new / delete ----------------------------------------------------------
// No exceptions in the kernel, so a failed `new` is fatal. Code that can
// cope with running out of memory calls kmalloc() and checks for nullptr.

namespace {

void* allocate(usize size) {
    void* p = kmalloc(size);
    if (!p)
        panic("out of memory: operator new(%zu)", size);
    return p;
}

void* allocate_aligned(usize size, std::align_val_t align) {
    auto a = static_cast<usize>(align);
    // Heap blocks are aligned to their size class (page-aligned above 2 KiB),
    // so rounding the size up to the alignment is enough.
    if (a > mm::PAGE_SIZE && size + a > 4 * MiB)
        panic("operator new: alignment %zu unsupported for %zu bytes", a, size);
    return allocate(size > a ? size : a);
}

} // namespace

void* operator new(usize size) { return allocate(size); }
void* operator new[](usize size) { return allocate(size); }
void* operator new(usize size, std::align_val_t align) { return allocate_aligned(size, align); }
void* operator new[](usize size, std::align_val_t align) { return allocate_aligned(size, align); }

void operator delete(void* p) noexcept { kfree(p); }
void operator delete[](void* p) noexcept { kfree(p); }
void operator delete(void* p, usize) noexcept { kfree(p); }
void operator delete[](void* p, usize) noexcept { kfree(p); }
void operator delete(void* p, std::align_val_t) noexcept { kfree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { kfree(p); }
void operator delete(void* p, usize, std::align_val_t) noexcept { kfree(p); }
void operator delete[](void* p, usize, std::align_val_t) noexcept { kfree(p); }

namespace cxx {

void run_global_constructors() {
    for (Constructor* ctor = __init_array_start; ctor != __init_array_end; ++ctor)
        (*ctor)();
}

} // namespace cxx
