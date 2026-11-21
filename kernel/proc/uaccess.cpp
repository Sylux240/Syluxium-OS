#include <arch/x86_64/protect.hpp>
#include <lib/errno.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <proc/process.hpp>
#include <proc/uaccess.hpp>

namespace uaccess {

long check(const void* ptr, usize len, bool write) {
    if (len == 0)
        return 0;
    auto addr = reinterpret_cast<uptr>(ptr);
    if (addr < proc::USER_MIN || addr + len < addr || addr + len > mm::USER_END)
        return -err::FAULT;
    proc::Process* p = proc::current();
    if (!p)
        return -err::FAULT;
    return p->mem->prepare(addr, len, write);
}

long copy_from(void* dst, const void* user_src, usize len) {
    if (long r = check(user_src, len, false); r < 0)
        return r;
    protect::UserAccess window;
    memcpy(dst, user_src, len);
    return 0;
}

long copy_to(void* user_dst, const void* src, usize len) {
    if (long r = check(user_dst, len, true); r < 0)
        return r;
    protect::UserAccess window;
    memcpy(user_dst, src, len);
    return 0;
}

long string_from(const char* user_src, char* dst, usize max_len) {
    auto addr = reinterpret_cast<uptr>(user_src);
    usize n = 0;
    while (n < max_len) {
        // Validate one page at a time: the string may end before an unmapped page.
        uptr page_end = align_down<uptr>(addr + n, mm::PAGE_SIZE) + mm::PAGE_SIZE;
        usize chunk = min<usize>(page_end - (addr + n), max_len - n);
        if (long r = check(user_src + n, chunk, false); r < 0)
            return r;
        protect::UserAccess window;
        for (usize i = 0; i < chunk; i++) {
            dst[n] = user_src[n];
            if (dst[n] == 0)
                return static_cast<long>(n);
            n++;
        }
    }
    return -err::NAMETOOLONG;
}

} // namespace uaccess
