// LinuxKPI core: the Linux kernel API (memory, strings, printk, locks, wait
// queues, completions, time, timers, workqueues) implemented on Syluxium's
// own primitives, so Linux drivers compile and run unchanged.
//
// Layout of this file: Syluxium headers and thin wrappers first; then the
// Linux headers (whose macros, like min/max, must not leak into the C++
// code above); then the C-linkage implementations.

#include <fs/poll.hpp>
#include <fs/genfs.hpp>
#include <fs/vfs.hpp>
#include <proc/process.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/fpu.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <cxx/runtime.hpp>
#include <lib/format.hpp>
#include <lib/spinlock.hpp>
#include <linuxkpi/linuxkpi.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <mm/vmalloc.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>
#include <lib/random.hpp>
#include <arch/x86_64/percpu.hpp>
#include <boot/limine_requests.hpp>
#include <video/console.hpp>
#include <video/framebuffer.hpp>
#include <video/splash.hpp>
#include <arch/x86_64/cpu_local.hpp>

// Linux's jiffies is a variable (struct fields share the name, so it can't
// be a macro in C): refreshed every millisecond by the timer thread.
extern "C" volatile unsigned long lkpi_jiffies_var asm("jiffies");
volatile unsigned long lkpi_jiffies_var = 0;

namespace {

void* sylux_kmalloc(usize n) { return kmalloc(n); }
void sylux_kfree(void* p) { kfree(p); }
int sylux_vsnprintf(char* b, usize n, const char* f, va_list a) { return vsnprintf(b, n, f, a); }
int sylux_snprintf_int(char* b, usize n, int v) { return snprintf(b, n, "%d", v); }
int sylux_snprintf_long(char* b, usize n, const char* f, long v) { return snprintf(b, n, f, v); }
int sylux_snprintf_str(char* b, usize n, const char* f, const char* v) { return snprintf(b, n, f, v); }
int sylux_snprintf_ip4(char* b, usize n, const u8* p) { return snprintf(b, n, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]); }
void* sylux_valloc(usize n) { return vmalloc::alloc(n ? n : 1); }
void sylux_vfree(void* p) { vmalloc::free(p); }

// Lazily constructed kernel object stored in a Linux struct's opaque words.
// State word: 0 = untouched (zero-filled static initializer), 1 = being
// constructed, 2 = ready. Lets DEFINE_MUTEX & co. work without constructors.
template <typename T>
T* lazy(unsigned long* storage, volatile u32* state) {
    static_assert(alignof(T) <= alignof(unsigned long));
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2) {
        u32 expected = 0;
        if (__atomic_compare_exchange_n(state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            new (storage) T();
            __atomic_store_n(state, 2, __ATOMIC_RELEASE);
        } else {
            while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2)
                arch::pause();
        }
    }
    return reinterpret_cast<T*>(storage);
}

} // namespace

// ---- Linux headers from here on ----
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#undef min
#undef max
#undef abs
#undef swap
#undef clamp

// Each wrapped object keeps its lazy-init state word right after the object.
static_assert(sizeof(IrqSpinlock) <= sizeof(spinlock_t), "spinlock_t too small");
static_assert(sizeof(sched::Mutex) + sizeof(u32) <= sizeof(struct mutex), "struct mutex too small");
static_assert(sizeof(sched::WaitQueue) + sizeof(u32) <= sizeof(((wait_queue_head_t*)nullptr)->opaque),
              "wait queue too small");
static_assert(sizeof(sched::Semaphore) + sizeof(u32) <= sizeof(((struct semaphore*)nullptr)->opaque),
              "semaphore too small");

namespace {

IrqSpinlock* spin_of(spinlock_t* l) { return reinterpret_cast<IrqSpinlock*>(l->opaque); }

sched::Mutex* mutex_of(struct mutex* m) {
    auto* state = reinterpret_cast<volatile u32*>(reinterpret_cast<u8*>(m->opaque) + sizeof(sched::Mutex));
    return lazy<sched::Mutex>(m->opaque, state);
}

sched::WaitQueue* wq_of(wait_queue_head_t* w) {
    auto* state = reinterpret_cast<volatile u32*>(reinterpret_cast<u8*>(w->opaque) + sizeof(sched::WaitQueue));
    return lazy<sched::WaitQueue>(w->opaque, state);
}

sched::Semaphore* sem_of(struct semaphore* s) {
    auto* state = reinterpret_cast<volatile u32*>(reinterpret_cast<u8*>(s->opaque) + sizeof(sched::Semaphore));
    sched::Semaphore* sem = lazy<sched::Semaphore>(s->opaque, state);
    return sem;
}

} // namespace

extern "C" {

// ======================= memory =======================

// Every block carries a 16-byte header: small blocks come from the slab
// heap (direct-mapped), large ones are whole physically contiguous pages,
// so all of them are valid for dma_map_single().
struct LkpiAllocHeader {
    u32 magic;
    u32 order_plus_one; // 0 = slab
    u64 size;
};
constexpr u32 ALLOC_MAGIC = 0x4C4B5049; // "LKPI"
constexpr usize SLAB_LIMIT = 2048;

void* lkpi_kmalloc(size_t size, gfp_t flags) {
    usize total = size + sizeof(LkpiAllocHeader);
    LkpiAllocHeader* h;
    if (total <= SLAB_LIMIT) {
        h = static_cast<LkpiAllocHeader*>(sylux_kmalloc(total));
        if (!h)
            return nullptr;
        h->order_plus_one = 0;
    } else {
        unsigned order = pmm::order_for_size(total);
        u64 phys = pmm::alloc_pages(order);
        if (!phys)
            return nullptr;
        h = mm::phys_to_virt<LkpiAllocHeader>(phys);
        h->order_plus_one = order + 1;
    }
    h->magic = ALLOC_MAGIC;
    h->size = size;
    void* p = h + 1;
    if (flags & __GFP_ZERO)
        memset(p, 0, size);
    return p;
}

void lkpi_kfree(const void* p) {
    if (!p || IS_ERR(p))
        return;
    auto* h = const_cast<LkpiAllocHeader*>(static_cast<const LkpiAllocHeader*>(p) - 1);
    if (h->magic != ALLOC_MAGIC) {
        klog::error("linuxkpi: kfree of a pointer kmalloc didn't return (%p)", p);
        return;
    }
    h->magic = 0;
    if (h->order_plus_one)
        pmm::free_pages(mm::direct_virt_to_phys(h), h->order_plus_one - 1);
    else
        sylux_kfree(h);
}

void* lkpi_kzalloc(size_t size, gfp_t flags) { return lkpi_kmalloc(size, flags | __GFP_ZERO); }

void* lkpi_kcalloc(size_t n, size_t size, gfp_t flags) {
    if (size && n > ~0ul / size)
        return nullptr;
    return lkpi_kmalloc(n * size, flags | __GFP_ZERO);
}

void* lkpi_kmalloc_array(size_t n, size_t size, gfp_t flags) {
    if (size && n > ~0ul / size)
        return nullptr;
    return lkpi_kmalloc(n * size, flags);
}

size_t lkpi_ksize(const void* p) { return p ? (static_cast<const LkpiAllocHeader*>(p) - 1)->size : 0; }

void* lkpi_krealloc(const void* p, size_t size, gfp_t flags) {
    if (!size) {
        lkpi_kfree(p);
        return ZERO_SIZE_PTR;
    }
    if (p && lkpi_ksize(p) >= size)
        return const_cast<void*>(p);
    void* n = lkpi_kmalloc(size, flags);
    if (n && p) {
        memcpy(n, p, lkpi_ksize(p));
        lkpi_kfree(p);
    }
    return n;
}

void lkpi_kfree_sensitive(const void* p) {
    if (p && !IS_ERR(p))
        memset(const_cast<void*>(p), 0, lkpi_ksize(p));
    lkpi_kfree(p);
}

void* lkpi_vmalloc(size_t size) { return sylux_valloc(size); }
void* lkpi_vzalloc(size_t size) { return sylux_valloc(size); } // vmalloc memory is zeroed
extern "C" bool lkpi_vfree_vmap(const void* addr); // glue/mm.c
extern "C" bool lkpi_vmalloc_is_mapping(const void* addr);
extern "C" void lkpi_vunmap(const void* virt);
void lkpi_vfree(const void* p) {
    // Linux's vfree() also takes vmap() mappings (VM_MAP_PUT_PAGES ones free
    // their pages; plain ones are just unmapped).
    if (p && lkpi_vfree_vmap(p))
        return;
    if (p && lkpi_vmalloc_is_mapping(p)) {
        lkpi_vunmap(p);
        return;
    }
    if (p)
        sylux_vfree(const_cast<void*>(p));
}

// Object caches: kmalloc underneath. SLAB_TYPESAFE_BY_RCU caches (i915
// requests, dma-fences found by lockless lookups) keep freed memory out of
// reuse until an RCU grace period has passed: an rcu_head trails each object.
namespace {
constexpr unsigned long LKPI_SLAB_TYPESAFE_BY_RCU = 0x00080000UL;
struct SlabTrailer {
    struct rcu_head head;
    size_t offset; // of the trailer from the object start
};
size_t slab_object_size(const struct kmem_cache* c) { return (c->size + 15) & ~size_t(15); }
void slab_rcu_free(struct rcu_head* h) {
    auto* t = reinterpret_cast<SlabTrailer*>(h);
    lkpi_kfree(reinterpret_cast<char*>(t) - t->offset);
}
} // namespace

struct kmem_cache* kmem_cache_create(const char* name, size_t size, size_t, unsigned long flags, void (*ctor)(void*)) {
    auto* c = static_cast<struct kmem_cache*>(lkpi_kzalloc(sizeof(struct kmem_cache), GFP_KERNEL));
    if (c) {
        c->size = size;
        c->name = name;
        c->flags = flags;
        c->ctor = ctor;
    }
    return c;
}
void kmem_cache_destroy(struct kmem_cache* c) { lkpi_kfree(c); }
void* kmem_cache_alloc(struct kmem_cache* c, gfp_t flags) {
    size_t n = c->flags & LKPI_SLAB_TYPESAFE_BY_RCU ? slab_object_size(c) + sizeof(SlabTrailer) : c->size;
    void* p = lkpi_kmalloc(n, flags);
    if (p && c->ctor)
        c->ctor(p);
    return p;
}
void* kmem_cache_zalloc(struct kmem_cache* c, gfp_t flags) {
    size_t n = c->flags & LKPI_SLAB_TYPESAFE_BY_RCU ? slab_object_size(c) + sizeof(SlabTrailer) : c->size;
    void* p = lkpi_kzalloc(n, flags);
    if (p && c->ctor)
        c->ctor(p);
    return p;
}
void kmem_cache_free(struct kmem_cache* c, void* p) {
    if (!p)
        return;
    if (c && (c->flags & LKPI_SLAB_TYPESAFE_BY_RCU)) {
        auto* t = reinterpret_cast<SlabTrailer*>(static_cast<char*>(p) + slab_object_size(c));
        t->offset = slab_object_size(c);
        call_rcu(&t->head, slab_rcu_free);
        return;
    }
    lkpi_kfree(p);
}

// ======================= strings =======================

size_t strnlen(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int strcasecmp(const char* a, const char* b) {
    while (*a && lower(*a) == lower(*b))
        a++, b++;
    return lower(static_cast<u8>(*a)) - lower(static_cast<u8>(*b));
}

char* strcpy(char* d, const char* s) {
    char* r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

char* strncpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

size_t strlcpy(char* d, const char* s, size_t size) {
    size_t len = strlen(s);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(d, s, n);
        d[n] = 0;
    }
    return len;
}

ssize_t strscpy(char* d, const char* s, size_t size) {
    if (!size)
        return -E2BIG;
    size_t len = strnlen(s, size);
    if (len == size) {
        memcpy(d, s, size - 1);
        d[size - 1] = 0;
        return -E2BIG;
    }
    memcpy(d, s, len + 1);
    return static_cast<ssize_t>(len);
}

char* strcat(char* d, const char* s) {
    strcpy(d + strlen(d), s);
    return d;
}

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == static_cast<char>(c))
            return const_cast<char*>(s);
        if (!*s)
            return nullptr;
    }
}

char* strrchr(const char* s, int c) {
    const char* last = nullptr;
    for (;; s++) {
        if (*s == static_cast<char>(c))
            last = s;
        if (!*s)
            return const_cast<char*>(last);
    }
}

char* strstr(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    if (!n)
        return const_cast<char*>(hay);
    for (; *hay; hay++)
        if (strncmp(hay, needle, n) == 0)
            return const_cast<char*>(hay);
    return nullptr;
}

void* memchr(const void* s, int c, size_t n) {
    auto* p = static_cast<const u8*>(s);
    for (size_t i = 0; i < n; i++)
        if (p[i] == static_cast<u8>(c))
            return const_cast<u8*>(p + i);
    return nullptr;
}

char* kstrdup(const char* s, gfp_t gfp) {
    if (!s)
        return nullptr;
    size_t n = strlen(s) + 1;
    char* d = static_cast<char*>(lkpi_kmalloc(n, gfp));
    if (d)
        memcpy(d, s, n);
    return d;
}

void* kmemdup(const void* src, size_t len, gfp_t gfp) {
    void* d = lkpi_kmalloc(len, gfp);
    if (d)
        memcpy(d, src, len);
    return d;
}

unsigned long simple_strtoul(const char* s, char** end, unsigned int base) {
    while (*s == ' ')
        s++;
    if (!base) {
        base = 10;
        if (s[0] == '0') {
            base = 8;
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            }
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    unsigned long v = 0;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (lower(*s) >= 'a' && lower(*s) <= 'z') d = lower(*s) - 'a' + 10;
        else break;
        if (d >= static_cast<int>(base))
            break;
        v = v * base + static_cast<unsigned long>(d);
    }
    if (end)
        *end = const_cast<char*>(s);
    return v;
}

int kstrtoul(const char* s, unsigned int base, unsigned long* res) {
    char* end;
    if (!*s)
        return -EINVAL;
    unsigned long v = simple_strtoul(s, &end, base);
    if (*end == '\n')
        end++;
    if (*end)
        return -EINVAL;
    *res = v;
    return 0;
}

int kstrtouint(const char* s, unsigned int base, unsigned int* res) {
    unsigned long v;
    int r = kstrtoul(s, base, &v);
    if (r)
        return r;
    if (v > 0xFFFFFFFFul)
        return -ERANGE;
    *res = static_cast<unsigned int>(v);
    return 0;
}

int kstrtoint(const char* s, unsigned int base, int* res) {
    bool neg = *s == '-';
    unsigned long v;
    int r = kstrtoul(neg ? s + 1 : s, base, &v);
    if (r)
        return r;
    if (v > 0x7FFFFFFFul)
        return -ERANGE;
    *res = neg ? -static_cast<int>(v) : static_cast<int>(v);
    return 0;
}

// ======================= printing =======================

// Linux-compatible vsnprintf: standard conversions go through the kernel's
// formatter one at a time; Linux's %p extensions are handled here
// (%pM/%pm MAC, %pI4/%pi4 IPv4, %pI6c IPv6, %*ph hex dump, %pe error, %pS
// symbol). Plain %p prints the bare hex address, as Linux does.
struct Out {
    char* buf;
    size_t size, pos;
    void put(char c) {
        if (pos + 1 < size)
            buf[pos] = c;
        pos++;
    }
    void puts(const char* s) {
        while (*s)
            put(*s++);
    }
};

static void hex2(Out& o, u8 v, bool upper) {
    const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    o.put(d[v >> 4]);
    o.put(d[v & 15]);
}

int lkpi_vsnprintf(char* buf, size_t size, const char* fmt, va_list args) {
    Out o{buf, size, 0};
    va_list ap;
    va_copy(ap, args);
    while (*fmt) {
        if (*fmt != '%') {
            o.put(*fmt++);
            continue;
        }
        const char* start = fmt++;
        if (*fmt == '%') {
            o.put('%');
            fmt++;
            continue;
        }
        // flags, width, precision
        char spec[32];
        size_t sp = 0;
        spec[sp++] = '%';
        while (*fmt && strchr("-+ #0", *fmt) && sp < 8)
            spec[sp++] = *fmt++;
        int width = -1;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
            sp += static_cast<size_t>(sylux_snprintf_int(spec + sp, sizeof(spec) - sp, width));
        } else {
            while (*fmt >= '0' && *fmt <= '9' && sp < 20)
                spec[sp++] = *fmt++;
        }
        if (*fmt == '.') {
            spec[sp++] = *fmt++;
            if (*fmt == '*') {
                int prec = va_arg(ap, int);
                fmt++;
                sp += static_cast<size_t>(sylux_snprintf_int(spec + sp, sizeof(spec) - sp, prec));
            } else {
                while (*fmt >= '0' && *fmt <= '9' && sp < 28)
                    spec[sp++] = *fmt++;
            }
        }
        // length
        int longs = 0;
        bool size_t_len = false;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 't' || *fmt == 'j' || *fmt == 'L' || *fmt == 'q') {
            if (*fmt == 'l' || *fmt == 'L' || *fmt == 'q') longs++;
            if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') size_t_len = true;
            fmt++;
        }
        char conv = *fmt ? *fmt++ : 0;
        char piece[128];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            spec[sp++] = 'l';
            spec[sp++] = conv;
            spec[sp] = 0;
            long v;
            if (longs || size_t_len)
                v = va_arg(ap, long);
            else
                v = (conv == 'd' || conv == 'i') ? static_cast<long>(va_arg(ap, int))
                                                 : static_cast<long>(va_arg(ap, unsigned int));
            sylux_snprintf_long(piece, sizeof(piece), spec, v);
            o.puts(piece);
            break;
        }
        case 'c':
            o.put(static_cast<char>(va_arg(ap, int)));
            break;
        case 's': {
            const char* str = va_arg(ap, const char*);
            spec[sp++] = 's';
            spec[sp] = 0;
            sylux_snprintf_str(piece, sizeof(piece), spec, str ? str : "(null)");
            o.puts(piece);
            break;
        }
        case 'p': {
            const u8* p = static_cast<const u8*>(va_arg(ap, const void*));
            char ext = *fmt;
            if ((ext == 'M' || ext == 'm') && p) {
                fmt++;
                for (int i = 0; i < 6; i++) {
                    if (i && ext == 'M')
                        o.put(':');
                    hex2(o, p[i], false);
                }
            } else if ((ext == 'I' || ext == 'i') && fmt[1] == '4' && p) {
                fmt += 2;
                sylux_snprintf_ip4(piece, sizeof(piece), p);
                o.puts(piece);
            } else if ((ext == 'I' || ext == 'i') && fmt[1] == '6' && p) {
                fmt += 2;
                if (*fmt == 'c')
                    fmt++;
                for (int i = 0; i < 16; i += 2) {
                    if (i)
                        o.put(':');
                    hex2(o, p[i], false);
                    hex2(o, p[i + 1], false);
                }
            } else if (ext == 'h' && p) {
                fmt++;
                char sep = ' ';
                if (*fmt == 'C' || *fmt == 'D' || *fmt == 'N') {
                    sep = *fmt == 'C' ? ':' : *fmt == 'D' ? '-' : 0;
                    fmt++;
                }
                int n = width > 0 ? width : 1;
                for (int i = 0; i < n && i < 64; i++) {
                    if (i && sep)
                        o.put(sep);
                    hex2(o, p[i], false);
                }
            } else if (ext == 'V' && p) {
                // %pV: a nested format + its arguments (struct va_format)
                fmt++;
                struct VaFormat { const char* fmt; va_list* va; }; // Linux struct va_format
                auto* vf = reinterpret_cast<const VaFormat*>(p);
                char nested[480];
                va_list copy;
                va_copy(copy, *vf->va);
                lkpi_vsnprintf(nested, sizeof(nested), vf->fmt, copy);
                va_end(copy);
                o.puts(nested);
            } else if (ext == 'e') {
                fmt++;
                sylux_snprintf_long(piece, sizeof(piece), "error %ld", reinterpret_cast<long>(p));
                o.puts(piece);
            } else {
                // %pS, %pK, %px, plain %p, ...: skip any extension letters, print hex
                while ((*fmt >= 'a' && *fmt <= 'z') || (*fmt >= 'A' && *fmt <= 'Z'))
                    fmt++;
                sylux_snprintf_long(piece, sizeof(piece), "%lx", reinterpret_cast<long>(p));
                o.puts(piece);
            }
            break;
        }
        default:
            while (start < fmt)
                o.put(*start++);
            break;
        }
    }
    va_end(ap);
    if (size)
        buf[o.pos < size ? o.pos : size - 1] = 0;
    return static_cast<int>(o.pos);
}

int lkpi_snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int n = lkpi_vsnprintf(buf, size, fmt, a);
    va_end(a);
    return n;
}

int lkpi_scnprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int n = lkpi_vsnprintf(buf, size, fmt, a);
    va_end(a);
    if (!size)
        return 0;
    return n < static_cast<int>(size) ? n : static_cast<int>(size) - 1;
}

int lkpi_sprintf(char* buf, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int n = lkpi_vsnprintf(buf, 4096, fmt, a); // Linux's sprintf is unbounded too
    va_end(a);
    return n;
}

char* kasprintf(gfp_t gfp, const char* fmt, ...) {
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    int n = lkpi_vsnprintf(nullptr, 0, fmt, a);
    va_end(a);
    char* s = static_cast<char*>(lkpi_kmalloc(static_cast<usize>(n) + 1, gfp));
    if (s)
        lkpi_vsnprintf(s, static_cast<usize>(n) + 1, fmt, b);
    va_end(b);
    return s;
}

// Send one message to the kernel log at a Linux level ('0'..'7', 'c' = continuation).
static void log_at(char level, char* buf) {
    usize len = strlen(buf);
    while (len && buf[len - 1] == '\n')
        buf[--len] = 0;
    if (level == 'c')
        kprintf("%s", buf);
    else if (level <= '3')
        klog::error("%s", buf);
    else if (level == '4')
        klog::warn("%s", buf);
    else if (level == '7')
        klog::debug("%s", buf);
    else
        klog::info("%s", buf);
}

static char strip_level(const char** fmt) {
    if ((*fmt)[0] == '\001' && (*fmt)[1]) {
        char level = (*fmt)[1];
        *fmt += 2;
        return level;
    }
    return '6';
}

// printk: strip the KERN_<level> prefix and route to the kernel log.
int vprintk(const char* fmt, va_list args) {
    char level = strip_level(&fmt);
    char buf[512];
    int n = lkpi_vsnprintf(buf, sizeof(buf), fmt, args);
    // The level can also arrive as an argument (printk("%s...", KERN_DEBUG, ...)).
    const char* text = buf;
    if (buf[0] == '\001' && buf[1]) {
        level = buf[1];
        text = buf + 2;
    }
    log_at(level, const_cast<char*>(text));
    return n;
}

int printk(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int n = vprintk(fmt, a);
    va_end(a);
    return n;
}

int lkpi_log_line(const char* level, const char* text) {
    char line[512];
    lkpi_snprintf(line, sizeof(line), "%s", text);
    log_at(strip_level(&level), line);
    return static_cast<int>(strlen(line));
}

int lkpi_dev_printk(const char* level, const struct device* d, const char* fmt, ...) {
    char msg[448];
    va_list a;
    va_start(a, fmt);
    lkpi_vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    char line[512];
    int n = lkpi_snprintf(line, sizeof(line), "%s: %s", d ? dev_name(d) : "(no device)", msg);
    log_at(strip_level(&level), line);
    return n;
}

void lkpi_bug(const char* file, int line) { panic("Linux driver BUG() at %s:%d", file, line); }
void lkpi_warn(const char* file, int line, const char* cond) {
    klog::warn("Linux driver WARN_ON(%s) at %s:%d", cond, file, line);
}

// ======================= locking =======================

void spin_lock_init(spinlock_t* l) { new (l->opaque) IrqSpinlock(); }
void lkpi_spin_lock(spinlock_t* l) { spin_of(l)->lock(); }
void lkpi_spin_unlock(spinlock_t* l) { spin_of(l)->unlock(); }
int lkpi_spin_trylock(spinlock_t* l) { return spin_of(l)->try_lock(); }

void lkpi_local_irq_disable(void) { arch::cli(); }
void lkpi_local_irq_enable(void) { arch::sti(); }
unsigned long lkpi_local_irq_save(void) {
    unsigned long f = arch::read_rflags();
    arch::cli();
    return f;
}
void lkpi_local_irq_restore(unsigned long f) {
    if (f & 0x200)
        arch::sti();
}

// RCU readers run with preemption disabled (like Linux's non-preemptible
// RCU), so a CPU that has switched threads (or is idle) has left every
// read-side section it was in.
void lkpi_rcu_read_lock(void) { cpu_local::preempt_disable(); }
void lkpi_rcu_read_unlock(void) {
    if (cpu_local::preempt_dec() && cpu_local::need_resched() && arch::interrupts_enabled())
        sched_preempt_point();
}
void lkpi_preempt_disable(void) { cpu_local::preempt_disable(); }
void lkpi_preempt_enable(void) { lkpi_rcu_read_unlock(); }
int lkpi_preempt_count(void) { return sched::running() ? cpu_local::preempt_count() : 0; }
int lkpi_smp_processor_id(void) { return static_cast<int>(this_cpu()->index); }

void synchronize_rcu(void) {
    // A grace period: wait until no CPU can still be inside a read-side
    // section that started before this call. Sleeping is a context switch
    // here; every other CPU must switch or be idle. A short sleep also
    // covers CPUs we can't observe.
    sched::sleep_ms(2);
}

void mutex_init(struct mutex* m) { memset(m, 0, sizeof(*m)); }
void mutex_lock(struct mutex* m) { mutex_of(m)->lock(); }
int mutex_lock_interruptible(struct mutex* m) {
    mutex_of(m)->lock();
    return 0;
}
int mutex_trylock(struct mutex* m) { return mutex_of(m)->try_lock(); }
void mutex_unlock(struct mutex* m) { mutex_of(m)->unlock(); }
int mutex_is_locked(struct mutex* m) {
    sched::Mutex* mx = mutex_of(m);
    if (mx->try_lock()) {
        mx->unlock();
        return 0;
    }
    return 1;
}

void sema_init(struct semaphore* s, int count) {
    memset(s, 0, sizeof(*s));
    auto* state = reinterpret_cast<volatile u32*>(reinterpret_cast<u8*>(s->opaque) + sizeof(sched::Semaphore));
    new (s->opaque) sched::Semaphore(count);
    __atomic_store_n(state, 2, __ATOMIC_RELEASE);
}
void down(struct semaphore* s) { sem_of(s)->wait(); }
int down_interruptible(struct semaphore* s) {
    sem_of(s)->wait();
    return 0;
}
int down_trylock(struct semaphore* s) { return sem_of(s)->try_wait() ? 0 : 1; }
int down_timeout(struct semaphore* s, long j) {
    return sem_of(s)->wait_timeout(static_cast<u64>(j) * 1'000'000) ? 0 : -ETIME;
}
void up(struct semaphore* s) { sem_of(s)->signal(); }

// ======================= wait queues / completions =======================

void init_waitqueue_head(wait_queue_head_t* w) {
    memset(w, 0, sizeof(*w));
    w->head.next = w->head.prev = &w->head;
}

// wait_event() sleepers: they watch the counter.
void lkpi_wake_up_seq(wait_queue_head_t* w) {
    __atomic_add_fetch(&w->seq, 1, __ATOMIC_SEQ_CST);
    wq_of(w)->wake_all();
    ::poll::notify(); // Linux files bridged by file.cpp: pollers re-check
}

void lkpi_wake_entries(wait_queue_head_t* w, unsigned int mode, int nr, void* key); // glue/wait.c

void wake_up(wait_queue_head_t* w) {
    lkpi_wake_up_seq(w);
    lkpi_wake_entries(w, TASK_NORMAL, 0, nullptr); // Linux entries (callbacks, prepare_to_wait sleepers)
}

int lkpi_wait_sleep(wait_queue_head_t* w, unsigned int seen, unsigned long deadline, int interruptible) {
    sched::WaitQueue* q = wq_of(w);
    bool intr = false;
    auto changed = [&] {
        if (__atomic_load_n(&w->seq, __ATOMIC_SEQ_CST) != seen)
            return true;
        if (interruptible && signal::interrupted()) {
            intr = true;
            return true;
        }
        return false;
    };
    if (deadline) {
        unsigned long now = clock::now_ms();
        if (static_cast<long>(deadline - now) <= 0)
            return 0;
        q->wait_until_timeout(changed, (deadline - now) * 1'000'000);
    } else {
        q->wait_until(changed);
    }
    return intr ? -ERESTARTSYS : 0;
}

void init_completion(struct completion* c) { memset(c, 0, sizeof(*c)); }

void complete(struct completion* c) {
    if (c->done != 0x7FFFFFFF)
        __atomic_add_fetch(&c->done, 1, __ATOMIC_SEQ_CST);
    wake_up(&c->wait);
}

void complete_all(struct completion* c) {
    __atomic_store_n(&c->done, 0x7FFFFFFFu, __ATOMIC_SEQ_CST);
    wake_up(&c->wait);
}

bool try_wait_for_completion(struct completion* c) {
    unsigned int d = __atomic_load_n(&c->done, __ATOMIC_SEQ_CST);
    while (d) {
        if (d == 0x7FFFFFFF)
            return true; // complete_all: stays done
        if (__atomic_compare_exchange_n(&c->done, &d, d - 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return true;
    }
    return false;
}

bool completion_done(struct completion* c) { return __atomic_load_n(&c->done, __ATOMIC_SEQ_CST) != 0; }

void wait_for_completion(struct completion* c) {
    for (;;) {
        unsigned int seq = __atomic_load_n(&c->wait.seq, __ATOMIC_SEQ_CST);
        if (try_wait_for_completion(c))
            return;
        lkpi_wait_sleep(&c->wait, seq, 0, 0);
    }
}

unsigned long wait_for_completion_timeout(struct completion* c, unsigned long timeout) {
    unsigned long end = clock::now_ms() + timeout;
    for (;;) {
        unsigned int seq = __atomic_load_n(&c->wait.seq, __ATOMIC_SEQ_CST);
        if (try_wait_for_completion(c)) {
            long left = static_cast<long>(end - clock::now_ms());
            return left > 0 ? static_cast<unsigned long>(left) : 1;
        }
        if (static_cast<long>(end - clock::now_ms()) <= 0)
            return 0;
        lkpi_wait_sleep(&c->wait, seq, end, 0);
    }
}

int wait_for_completion_interruptible(struct completion* c) {
    for (;;) {
        unsigned int seq = __atomic_load_n(&c->wait.seq, __ATOMIC_SEQ_CST);
        if (try_wait_for_completion(c))
            return 0;
        if (lkpi_wait_sleep(&c->wait, seq, 0, 1) < 0)
            return -ERESTARTSYS;
    }
}

long wait_for_completion_interruptible_timeout(struct completion* c, unsigned long timeout) {
    unsigned long end = clock::now_ms() + timeout;
    for (;;) {
        unsigned int seq = __atomic_load_n(&c->wait.seq, __ATOMIC_SEQ_CST);
        if (try_wait_for_completion(c)) {
            long left = static_cast<long>(end - clock::now_ms());
            return left > 0 ? left : 1;
        }
        if (static_cast<long>(end - clock::now_ms()) <= 0)
            return 0;
        if (lkpi_wait_sleep(&c->wait, seq, end, 1) < 0)
            return -ERESTARTSYS;
    }
}

// ======================= time =======================

unsigned long lkpi_jiffies(void) { return clock::now_ms(); }
u64 lkpi_ktime_real_ns(void) { return rtc::realtime_ns(); }

void get_random_bytes(void* buf, size_t len) { random::fill(buf, len); }
u32 get_random_u32(void) { return static_cast<u32>(random::next_u64()); }
bool lkpi_is_vmalloc_addr(const void* p) { return mm::is_vmalloc_address(p); }

// ======================= RCU callbacks =======================
// call_rcu()/kfree_rcu() callbacks wait for a grace period on the
// "lkpi-rcu" thread. A callback whose function pointer is a small number
// is a kfree_rcu(): the number is the rcu_head's offset in the object.

static IrqSpinlock g_rcu_lock;
static struct callback_head* g_rcu_pending = nullptr;
static struct callback_head** g_rcu_tail = &g_rcu_pending;
static sched::WaitQueue g_rcu_wait;
static sched::WaitQueue g_rcu_done;
static volatile u64 g_rcu_queued = 0, g_rcu_completed = 0;

void call_rcu(struct callback_head* head, rcu_callback_t func) {
    head->func = func;
    head->next = nullptr;
    {
        LockGuard guard(g_rcu_lock);
        *g_rcu_tail = head;
        g_rcu_tail = &head->next;
        g_rcu_queued++;
    }
    g_rcu_wait.wake_all();
}

void lkpi_kfree_rcu(struct callback_head* head, unsigned long offset) {
    call_rcu(head, reinterpret_cast<rcu_callback_t>(offset));
}

void rcu_barrier(void) {
    u64 target = g_rcu_queued;
    g_rcu_done.wait_until([&] { return g_rcu_completed >= target; });
}

static void rcu_thread(void*) {
    for (;;) {
        g_rcu_wait.wait_until([] { return g_rcu_pending != nullptr; });
        struct callback_head* list;
        u64 count = 0;
        {
            LockGuard guard(g_rcu_lock);
            list = g_rcu_pending;
            g_rcu_pending = nullptr;
            g_rcu_tail = &g_rcu_pending;
        }
        synchronize_rcu();
        while (list) {
            struct callback_head* next = list->next;
            auto fn = reinterpret_cast<uptr>(list->func);
            if (fn < 4096)
                lkpi_kfree(reinterpret_cast<char*>(list) - fn);
            else
                list->func(list);
            list = next;
            count++;
        }
        __atomic_add_fetch(&g_rcu_completed, count, __ATOMIC_SEQ_CST);
        g_rcu_done.wake_all();
    }
}

// ======================= threads =======================

namespace {
// A kthread_run() thread: what kthread_stop/park/unpark talk to it with.
struct KthreadState {
    int (*fn)(void*);
    void* data;
    volatile bool stop = false, park = false, parked = false, exited = false;
    int result = 0;
    sched::WaitQueue ctl;
};
struct LkpiTask {
    struct task_struct t;
    sched::WaitQueue wq;
    KthreadState* kt = nullptr;
};
LkpiTask* task_of(struct task_struct* t) { return reinterpret_cast<LkpiTask*>(t); }

void kthread_entry(void* arg) {
    auto* lt = static_cast<LkpiTask*>(arg);
    KthreadState* k = lt->kt;
    k->result = k->fn(k->data);
    {
        LockGuard guard(k->ctl.lock());
        k->exited = true;
    }
    k->ctl.wake_all();
    sched::exit(0);
}

KthreadState* current_kthread() {
    sched::Thread* th = sched::current();
    return th && th->lkpi_task ? static_cast<LkpiTask*>(th->lkpi_task)->kt : nullptr;
}
} // namespace

// The thread gets its task before it runs, so the caller can stop, park
// and wake it through the task kthread_run() returns.
struct task_struct* lkpi_kthread_run(int (*fn)(void*), void* data, const char* name) {
    auto* lt = new LkpiTask{};
    lt->kt = new KthreadState{fn, data};
    snprintf(lt->t.comm, sizeof(lt->t.comm), "%s", name);
    lt->t.lkpi_usage = 1;
    lt->t.group_leader = &lt->t;
    sched::Thread* th = sched::create(name, kthread_entry, lt, true);
    if (!th) {
        delete lt->kt;
        delete lt;
        return reinterpret_cast<struct task_struct*>(ERR_PTR(-ENOMEM));
    }
    lt->t.lkpi_thread = th;
    th->lkpi_task = lt;
    sched::start(th);
    return &lt->t;
}

bool kthread_should_stop(void) {
    KthreadState* k = current_kthread();
    return k && k->stop;
}
bool kthread_should_park(void) {
    KthreadState* k = current_kthread();
    return k && k->park;
}
void kthread_parkme(void) {
    KthreadState* k = current_kthread();
    if (!k)
        return;
    {
        LockGuard guard(k->ctl.lock());
        k->parked = true;
    }
    k->ctl.wake_all();
    k->ctl.wait_until([&] { return !k->park || k->stop; });
    k->parked = false;
}
int kthread_park(struct task_struct* t) {
    KthreadState* k = t ? task_of(t)->kt : nullptr;
    if (!k)
        return -ENOSYS;
    {
        LockGuard guard(k->ctl.lock());
        k->park = true;
    }
    wake_up_process(t);
    k->ctl.wait_until([&] { return k->parked || k->exited; });
    return 0;
}
void kthread_unpark(struct task_struct* t) {
    KthreadState* k = t ? task_of(t)->kt : nullptr;
    if (!k)
        return;
    {
        LockGuard guard(k->ctl.lock());
        k->park = false;
    }
    k->ctl.wake_all();
}
int kthread_stop(struct task_struct* t) {
    KthreadState* k = t ? task_of(t)->kt : nullptr;
    if (!k)
        return -EINTR;
    {
        LockGuard guard(k->ctl.lock());
        k->stop = true;
        k->park = false;
    }
    k->ctl.wake_all();
    wake_up_process(t);
    k->ctl.wait_until([&] { return k->exited; });
    return k->result; // the task stays allocated (like other threads' tasks)
}

// The task of a whole process (group_leader of its threads' tasks): what
// binder identifies a process by. Reference counted; the process holds one.
static struct task_struct* group_task(proc::Process* p) {
    if (!p->lkpi_task) {
        auto* lt = new LkpiTask{};
        snprintf(lt->t.comm, sizeof(lt->t.comm), "%s", p->name);
        lt->t.pid = lt->t.tgid = p->pid;
        lt->t.group_leader = &lt->t;
        lt->t.lkpi_usage = 1;
        lt->t.lkpi_process = p;
        void* expected = nullptr;
        if (!__atomic_compare_exchange_n(&p->lkpi_task, &expected, static_cast<void*>(lt), false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            delete lt;
    }
    return &static_cast<LkpiTask*>(p->lkpi_task)->t;
}

extern "C" void lkpi_task_free(struct task_struct* t) { delete task_of(t); }

// Threads' tasks: pid is the thread id (as in Linux), tgid the process id.
// They live as long as their thread (never freed, like the thread's own).
struct task_struct* lkpi_current(void) {
    sched::Thread* th = sched::current();
    if (!th) {
        static LkpiTask boot;
        boot.t.group_leader = &boot.t;
        boot.t.lkpi_usage = 1;
        return &boot.t;
    }
    if (!th->lkpi_task) {
        auto* lt = new LkpiTask{};
        snprintf(lt->t.comm, sizeof(lt->t.comm), "%s", th->name);
        lt->t.lkpi_thread = th;
        lt->t.lkpi_usage = 1;
        lt->t.group_leader = &lt->t;
        if (th->process) {
            lt->t.tgid = th->process->pid;
            lt->t.pid = th->user_tid ? th->user_tid : th->process->pid;
            lt->t.group_leader = group_task(th->process);
        }
        th->lkpi_task = lt;
    }
    return &static_cast<LkpiTask*>(th->lkpi_task)->t;
}

int lkpi_kernel_uaccess_active(void) {
    sched::Thread* th = sched::current();
    return th && th->lkpi_task && static_cast<LkpiTask*>(th->lkpi_task)->t.lkpi_kernel_uaccess > 0;
}

void lkpi_set_current_state(long state) {
    __atomic_store_n(&lkpi_current()->__state, state, __ATOMIC_SEQ_CST);
}

int wake_up_state(struct task_struct* p, unsigned int state) {
    if (!p || !(__atomic_load_n(&p->__state, __ATOMIC_SEQ_CST) & state))
        return 0;
    __atomic_store_n(&p->__state, TASK_RUNNING, __ATOMIC_SEQ_CST);
    task_of(p)->wq.wake_all();
    return 1;
}
int wake_up_process(struct task_struct* p) { return wake_up_state(p, TASK_NORMAL); }
int lkpi_signal_pending(void) { return sched::current() && sched::current()->process && signal::interrupted(); }

u64 lkpi_ktime_ns(void) { return clock::now_ns(); }

void udelay(unsigned long us) { clock::busy_wait_ns(us * 1000); }
void ndelay(unsigned long ns) { clock::busy_wait_ns(ns); }
void msleep(unsigned int ms) { sched::sleep_ms(ms ? ms : 1); }
unsigned long msleep_interruptible(unsigned int ms) {
    sched::sleep_ms(ms);
    return 0;
}
void usleep_range(unsigned long min_us, unsigned long) {
    if (min_us < 50)
        clock::busy_wait_ns(min_us * 1000); // too short to be worth a context switch
    else
        sched::sleep_ns(min_us * 1000);
}
// Sleeps while the task is still in a sleeping state (set_current_state)
// and nobody woke it; returns the jiffies (ms) left.
long schedule_timeout(long timeout) {
    struct task_struct* t = lkpi_current();
    long state = __atomic_load_n(&t->__state, __ATOMIC_SEQ_CST);
    if (state == TASK_RUNNING) {
        sched::yield();
        return timeout;
    }
    bool interruptible = state & TASK_INTERRUPTIBLE;
    auto still = [&] {
        return __atomic_load_n(&t->__state, __ATOMIC_SEQ_CST) != TASK_RUNNING &&
               !(interruptible && lkpi_signal_pending());
    };
    u64 start = clock::now_ns();
    if (timeout == MAX_SCHEDULE_TIMEOUT) {
        task_of(t)->wq.wait_until([&] { return !still(); });
    } else if (timeout > 0) {
        task_of(t)->wq.wait_until_timeout([&] { return !still(); }, static_cast<u64>(timeout) * 1'000'000);
    }
    __atomic_store_n(&t->__state, TASK_RUNNING, __ATOMIC_SEQ_CST);
    if (timeout == MAX_SCHEDULE_TIMEOUT)
        return timeout;
    long used = static_cast<long>((clock::now_ns() - start) / 1'000'000);
    return used >= timeout ? 0 : timeout - used;
}
void schedule(void) { schedule_timeout(MAX_SCHEDULE_TIMEOUT); }

// ======================= bit searching =======================

unsigned long find_next_bit(const unsigned long* addr, unsigned long size, unsigned long offset) {
    for (unsigned long i = offset; i < size; i++)
        if (addr[i / BITS_PER_LONG] & (1ul << (i % BITS_PER_LONG)))
            return i;
    return size;
}
unsigned long find_next_zero_bit(const unsigned long* addr, unsigned long size, unsigned long offset) {
    for (unsigned long i = offset; i < size; i++)
        if (!(addr[i / BITS_PER_LONG] & (1ul << (i % BITS_PER_LONG))))
            return i;
    return size;
}
unsigned long find_first_bit(const unsigned long* addr, unsigned long size) { return find_next_bit(addr, size, 0); }
unsigned long find_first_zero_bit(const unsigned long* addr, unsigned long size) {
    return find_next_zero_bit(addr, size, 0);
}

} // extern "C"

// ======================= timers =======================

namespace {

IrqSpinlock g_timer_lock;
struct timer_list* g_timers = nullptr;   // pending timers (unsorted)
struct timer_list* g_running_timer = nullptr;
sched::WaitQueue g_timer_done;           // del_timer_sync waits here

bool unlink_timer(struct timer_list* t) {
    for (struct timer_list** link = &g_timers; *link; link = &(*link)->lkpi_next) {
        if (*link == t) {
            *link = t->lkpi_next;
            t->lkpi_pending = 0;
            return true;
        }
    }
    return false;
}

void timer_thread(void*) {
    for (;;) {
        sched::sleep_ms(1);
        unsigned long now = clock::now_ms();
        lkpi_jiffies_var = now;
        for (;;) {
            struct timer_list* due = nullptr;
            {
                LockGuard guard(g_timer_lock);
                for (struct timer_list* t = g_timers; t; t = t->lkpi_next)
                    if (static_cast<long>(now - t->expires) >= 0) {
                        due = t;
                        break;
                    }
                if (!due)
                    break;
                unlink_timer(due);
                g_running_timer = due;
            }
            due->function(due);
            {
                LockGuard guard(g_timer_lock);
                g_running_timer = nullptr;
            }
            g_timer_done.wake_all();
        }
    }
}

} // namespace

extern "C" {

void timer_setup(struct timer_list* t, void (*fn)(struct timer_list*), unsigned int flags) {
    t->function = fn;
    t->flags = flags;
    t->expires = 0;
    t->lkpi_next = nullptr;
    t->lkpi_pending = 0;
}

int mod_timer(struct timer_list* t, unsigned long expires) {
    LockGuard guard(g_timer_lock);
    bool was = unlink_timer(t);
    t->expires = expires;
    t->lkpi_next = g_timers;
    g_timers = t;
    t->lkpi_pending = 1;
    return was;
}

void add_timer(struct timer_list* t) { mod_timer(t, t->expires); }

int del_timer(struct timer_list* t) {
    LockGuard guard(g_timer_lock);
    return unlink_timer(t);
}

int del_timer_sync(struct timer_list* t) {
    int was = del_timer(t);
    g_timer_done.wait_until([&] { return g_running_timer != t; });
    return was;
}

int timer_pending(const struct timer_list* t) { return t->lkpi_pending; }

} // extern "C"

// ======================= workqueues =======================

// A workqueue: one worker thread (ordered queues, system_wq) or a few
// (unbound queues: a work item that sleeps waiting for another one on the
// same queue must not deadlock it, as Linux guarantees). A work item never
// runs on two workers at once.
constexpr int WQ_MAX_WORKERS = 4;
struct workqueue_struct {
    char name[32];
    IrqSpinlock lock;
    struct work_struct* head = nullptr;
    struct work_struct* tail = nullptr;
    struct work_struct* running[WQ_MAX_WORKERS] = {};
    int workers = 1;
    sched::Semaphore pending;
    sched::WaitQueue idle;
    bool stopping = false;
};

namespace {

constexpr int WORK_QUEUED = 1;

bool is_running(workqueue_struct* wq, const struct work_struct* w) {
    for (int i = 0; i < wq->workers; i++)
        if (wq->running[i] == w)
            return true;
    return false;
}

bool any_running(workqueue_struct* wq) {
    for (int i = 0; i < wq->workers; i++)
        if (wq->running[i])
            return true;
    return false;
}

struct WorkerArg {
    workqueue_struct* wq;
    int slot;
};

void worker(void* arg) {
    auto* wa = static_cast<WorkerArg*>(arg);
    workqueue_struct* wq = wa->wq;
    int slot = wa->slot;
    delete wa;
    for (;;) {
        wq->pending.wait();
        struct work_struct* w = nullptr;
        {
            LockGuard guard(wq->lock);
            // The first queued item not already running on another worker.
            struct work_struct* prev = nullptr;
            for (struct work_struct* c = wq->head; c; prev = c, c = c->lkpi_next) {
                if (is_running(wq, c))
                    continue;
                w = c;
                if (prev)
                    prev->lkpi_next = c->lkpi_next;
                else
                    wq->head = c->lkpi_next;
                if (wq->tail == c)
                    wq->tail = prev;
                break;
            }
            if (!w) {
                if (wq->stopping && !wq->head)
                    break;
                continue; // nothing runnable: the worker running it re-signals when done
            }
            w->lkpi_next = nullptr;
            wq->running[slot] = w;
            __atomic_fetch_and(&w->lkpi_state, ~WORK_QUEUED, __ATOMIC_SEQ_CST);
        }
        // The function may free or requeue w: it is not touched afterwards
        // ("running" is tracked by address, as in Linux).
        w->func(w);
        bool more;
        {
            LockGuard guard(wq->lock);
            wq->running[slot] = nullptr;
            more = wq->head != nullptr;
        }
        if (more && wq->workers > 1)
            wq->pending.signal(); // an item skipped because it was running here may run now
        wq->idle.wake_all();
    }
}

workqueue_struct* make_wq_workers(const char* name, int workers) {
    auto* wq = new workqueue_struct;
    snprintf(wq->name, sizeof(wq->name), "%s", name);
    wq->workers = workers < 1 ? 1 : workers > WQ_MAX_WORKERS ? WQ_MAX_WORKERS : workers;
    for (int i = 0; i < wq->workers; i++)
        sched::spawn(wq->name, worker, new WorkerArg{wq, i}, -1, true);
    return wq;
}

workqueue_struct* make_wq(const char* name) { return make_wq_workers(name, 1); }

extern "C" void lkpi_delayed_work_timer(struct timer_list* t);
void delayed_work_timer(struct timer_list* t) { lkpi_delayed_work_timer(t); }
} // namespace
extern "C" void lkpi_delayed_work_timer(struct timer_list* t) {
    auto* d = container_of(t, struct delayed_work, timer);
    queue_work(d->wq ? d->wq : system_wq, &d->work);
}


extern "C" {

struct workqueue_struct* system_wq = nullptr;
struct workqueue_struct* system_highpri_wq = nullptr;
struct workqueue_struct* system_long_wq = nullptr;
struct workqueue_struct* system_unbound_wq = nullptr;
struct workqueue_struct* system_freezable_wq = nullptr;
struct workqueue_struct* system_power_efficient_wq = nullptr;

void lkpi_init_work(struct work_struct* w, work_func_t fn) {
    w->func = fn;
    w->lkpi_next = nullptr;
    w->lkpi_wq = nullptr;
    w->lkpi_state = 0;
}

void lkpi_init_delayed_work(struct delayed_work* d, work_func_t fn) {
    lkpi_init_work(&d->work, fn);
    timer_setup(&d->timer, delayed_work_timer, 0);
    d->wq = nullptr;
}

struct workqueue_struct* alloc_workqueue(const char* fmt, unsigned int flags, int max_active, ...) {
    char name[32];
    va_list a;
    va_start(a, max_active);
    sylux_vsnprintf(name, sizeof(name), fmt, a);
    va_end(a);
    // Ordered queues (max_active 1) keep one worker; WQ_UNBOUND (0x2) queues
    // get several, like Linux's unbound pools.
    return make_wq_workers(name, (flags & 0x2) && max_active != 1 ? WQ_MAX_WORKERS : 1);
}

void flush_workqueue(struct workqueue_struct* wq) {
    wq->idle.wait_until([&] { return wq->head == nullptr && !any_running(wq); });
}

void destroy_workqueue(struct workqueue_struct* wq) {
    flush_workqueue(wq);
    {
        LockGuard guard(wq->lock);
        wq->stopping = true;
    }
    wq->pending.signal();
    // The worker exits on its own; the queue object is leaked on purpose
    // (a few bytes) so a late wake can't touch freed memory.
}

bool queue_work(struct workqueue_struct* wq, struct work_struct* w) {
    if (__atomic_fetch_or(&w->lkpi_state, WORK_QUEUED, __ATOMIC_SEQ_CST) & WORK_QUEUED)
        return false; // already queued
    {
        LockGuard guard(wq->lock);
        w->lkpi_wq = wq;
        w->lkpi_next = nullptr;
        if (wq->tail)
            wq->tail->lkpi_next = w;
        else
            wq->head = w;
        wq->tail = w;
    }
    wq->pending.signal();
    return true;
}

bool queue_delayed_work(struct workqueue_struct* wq, struct delayed_work* d, unsigned long delay) {
    if (work_pending(&d->work) || timer_pending(&d->timer))
        return false;
    d->wq = wq;
    if (!delay)
        return queue_work(wq, &d->work);
    mod_timer(&d->timer, clock::now_ms() + delay);
    return true;
}

bool mod_delayed_work(struct workqueue_struct* wq, struct delayed_work* d, unsigned long delay) {
    bool was = del_timer(&d->timer);
    d->wq = wq;
    if (!delay)
        return queue_work(wq, &d->work) || was;
    if (!work_pending(&d->work))
        mod_timer(&d->timer, clock::now_ms() + delay);
    return was;
}

bool work_pending(const struct work_struct* w) { return __atomic_load_n(&w->lkpi_state, __ATOMIC_SEQ_CST) & WORK_QUEUED; }

// Remove a queued (not yet running) item. Returns whether it was queued.
static bool dequeue(struct work_struct* w) {
    workqueue_struct* wq = w->lkpi_wq;
    if (!wq)
        return false;
    LockGuard guard(wq->lock);
    struct work_struct* prev = nullptr;
    for (struct work_struct* c = wq->head; c; prev = c, c = c->lkpi_next) {
        if (c != w)
            continue;
        if (prev)
            prev->lkpi_next = c->lkpi_next;
        else
            wq->head = c->lkpi_next;
        if (wq->tail == c)
            wq->tail = prev;
        c->lkpi_next = nullptr;
        __atomic_fetch_and(&w->lkpi_state, ~WORK_QUEUED, __ATOMIC_SEQ_CST);
        return true;
    }
    return false;
}

bool flush_work(struct work_struct* w) {
    workqueue_struct* wq = w->lkpi_wq;
    if (!wq)
        return false;
    bool busy = w->lkpi_state != 0 || is_running(wq, w);
    wq->idle.wait_until([&] { return !(__atomic_load_n(&w->lkpi_state, __ATOMIC_SEQ_CST) & WORK_QUEUED) && !is_running(wq, w); });
    return busy;
}

bool cancel_work(struct work_struct* w) { return dequeue(w); }

bool cancel_work_sync(struct work_struct* w) {
    bool was = dequeue(w);
    workqueue_struct* wq = w->lkpi_wq;
    if (wq)
        wq->idle.wait_until([&] { return !is_running(wq, w); });
    return was;
}

bool cancel_delayed_work(struct delayed_work* d) { return del_timer(&d->timer) | dequeue(&d->work); }

bool cancel_delayed_work_sync(struct delayed_work* d) {
    bool was = del_timer_sync(&d->timer);
    return cancel_work_sync(&d->work) || was;
}

bool flush_delayed_work(struct delayed_work* d) {
    if (del_timer_sync(&d->timer))
        queue_work(d->wq ? d->wq : system_wq, &d->work);
    return flush_work(&d->work);
}

} // extern "C"

// ======================= startup =======================

extern "C" const struct lkpi_param __lkpi_params_start[];
extern "C" const struct lkpi_param __lkpi_params_end[];
extern "C" long lkpi_drm_debug_budget; // drm_print.c

namespace {

// <module>.<param>=<value> on the kernel command line.
namespace {
bool gen_param(genfs::TextBuffer& o, const void* ctx) {
    auto* p = static_cast<const struct lkpi_param*>(ctx);
    switch (p->type) {
    case 'i': o.printf("%d\n", *static_cast<int*>(p->var)); break;
    case 'u': o.printf("%u\n", *static_cast<unsigned*>(p->var)); break;
    case 'l': o.printf("%ld\n", *static_cast<long*>(p->var)); break;
    case 'L': o.printf("%lu\n", *static_cast<unsigned long*>(p->var)); break;
    case 's': o.printf("%d\n", *static_cast<short*>(p->var)); break;
    case 'S': o.printf("%u\n", *static_cast<unsigned short*>(p->var)); break;
    case 'b': o.printf("%u\n", *static_cast<unsigned char*>(p->var)); break;
    case 'B': o.printf("%c\n", *static_cast<bool*>(p->var) ? 'Y' : 'N'); break;
    case 'I': o.printf("%c\n", *static_cast<bool*>(p->var) ? 'N' : 'Y'); break;
    case 'c': {
        const char* v = *static_cast<char**>(p->var);
        o.printf("%s\n", v ? v : "(null)");
        break;
    }
    }
    return true;
}
isize write_param(const char* text, usize len, const void* ctx) {
    auto* p = static_cast<const struct lkpi_param*>(ctx);
    char buf[32];
    usize n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        buf[--n] = 0;
    bool yes = buf[0] == 'y' || buf[0] == 'Y' || buf[0] == '1';
    long value = static_cast<long>(simple_strtoul(buf[0] == '-' ? buf + 1 : buf, nullptr, 0));
    if (buf[0] == '-')
        value = -value;
    switch (p->type) {
    case 'i': case 'u': *static_cast<int*>(p->var) = static_cast<int>(value); break;
    case 'l': case 'L': *static_cast<long*>(p->var) = value; break;
    case 's': case 'S': *static_cast<short*>(p->var) = static_cast<short>(value); break;
    case 'b': *static_cast<unsigned char*>(p->var) = static_cast<unsigned char>(value); break;
    case 'B': *static_cast<bool*>(p->var) = yes; break;
    case 'I': *static_cast<bool*>(p->var) = !yes; break;
    default: return -EINVAL; // strings: boot option only
    }
    // Set by hand: drm.debug isn't cut off after its first 30000 messages.
    if (!strcmp(p->name, "drm.debug"))
        lkpi_drm_debug_budget = -1;
    klog::info("linuxkpi: %s = %s (set at run time)", p->name, buf);
    return static_cast<isize>(len);
}
} // namespace

void publish_module_params() {
    for (const struct lkpi_param* p = __lkpi_params_start; p < __lkpi_params_end; p++) {
        const char* dot = strchr(p->name, '.');
        if (!dot)
            continue;
        char dir[128], path[192];
        snprintf(dir, sizeof(dir), "/sys/module/%.*s/parameters", static_cast<int>(dot - p->name), p->name);
        vfs::mkdir_p(dir, 0755);
        snprintf(path, sizeof(path), "%s/%s", dir, dot + 1);
        vfs::add_node(path, new genfs::ControlFile(gen_param, write_param, p, p->type == 'c' ? 0444 : 0644));
    }
}

void apply_module_params() {
    const char* cmd = boot::cmdline();
    for (const struct lkpi_param* p = __lkpi_params_start; p < __lkpi_params_end; p++) {
        usize n = strlen(p->name);
        for (const char* s = cmd; (s = strstr(s, p->name)) != nullptr; s += n) {
            if ((s != cmd && s[-1] != ' ') || s[n] != '=')
                continue;
            const char* v = s + n + 1;
            char buf[256];
            usize len = 0;
            while (v[len] && v[len] != ' ' && len + 1 < sizeof(buf)) {
                buf[len] = v[len];
                len++;
            }
            buf[len] = 0;
            long value = static_cast<long>(simple_strtoul(buf[0] == '-' ? buf + 1 : buf, nullptr, 0));
            if (buf[0] == '-')
                value = -value;
            bool yes = buf[0] == 'y' || buf[0] == 'Y' || buf[0] == '1';
            switch (p->type) {
            case 'i': case 'u': *static_cast<int*>(p->var) = static_cast<int>(value); break;
            case 'l': case 'L': *static_cast<long*>(p->var) = value; break;
            case 's': case 'S': *static_cast<short*>(p->var) = static_cast<short>(value); break;
            case 'b': *static_cast<unsigned char*>(p->var) = static_cast<unsigned char>(value); break;
            case 'B': *static_cast<bool*>(p->var) = yes; break;
            case 'I': *static_cast<bool*>(p->var) = !yes; break;
            case 'c': {
                // charp (nouveau.debug=gr=debug,fifo=trace, nouveau.config=...):
                // a copy that lives as long as the kernel.
                auto* copy = static_cast<char*>(sylux_kmalloc(len + 1));
                if (!copy)
                    continue;
                memcpy(copy, buf, len + 1);
                *static_cast<char**>(p->var) = copy;
                break;
            }
            default: continue;
            }
            klog::info("linuxkpi: %s = %s", p->name, buf);
        }
    }
}

} // namespace

extern "C" void lkpi_register_boot_framebuffer(u64 phys, u32 width, u32 height, u32 pitch, u32 red_shift);
extern "C" unsigned long __drm_debug;
extern "C" long lkpi_drm_debug_budget; // drm_print.c
extern "C" void lkpi_boot_fb_set(u64 base, u32 width, u32 height, u32 pitch, u32 red_shift);

// A GPU driver took the display over (glue/gpu.c): the kernel must stop
// drawing into the firmware framebuffer, which may be GPU memory the driver
// now uses for something else. Boot messages continue on serial and in the log.
extern "C" void lkpi_display_released(void) {
    klog::info("linuxkpi: a GPU driver drives the display now; kernel console continues on serial/log only");
    splash::handover();
    splash::finish();
    console::set_graphics(true);
}

// glue/fbdev.c: the GPU driver's console framebuffer (DRM fbdev emulation)
// is up: the kernel console moves there and shows again.
namespace {
video::Framebuffer g_gpu_console_fb;
void* g_gpu_console_vaddr = nullptr;
} // namespace
extern "C" void lkpi_console_attach(void* vaddr, u32 width, u32 height, u32 pitch, u32 red_shift, u32 green_shift,
                                    u32 blue_shift) {
    if (!g_gpu_console_fb.init(vaddr, width, height, pitch, 32, static_cast<u8>(red_shift),
                               static_cast<u8>(green_shift), static_cast<u8>(blue_shift)))
        return;
    g_gpu_console_vaddr = vaddr;
    console::rebind(g_gpu_console_fb);
    console::set_graphics(false);
    klog::info("linuxkpi: kernel console on the GPU driver's framebuffer (%ux%u)", width, height);
}
extern "C" void lkpi_console_detach(void* vaddr) {
    if (vaddr != g_gpu_console_vaddr)
        return;
    console::set_graphics(true); // nowhere safe to draw until another framebuffer comes
    g_gpu_console_vaddr = nullptr;
}
extern "C" bool lkpi_console_text_mode(void) { return !console::graphics(); }
extern "C" unsigned long long lkpi_console_changes(void) { return console::changes(); }

// The GPU driver failed and the firmware framebuffer is back: show the
// console again (it reports what happened).
extern "C" void lkpi_display_restored(void) {
    klog::warn("linuxkpi: display back on the firmware framebuffer");
    console::set_graphics(false);
}
extern "C" lkpi_initcall_t __lkpi_initcalls_start[];
extern "C" lkpi_initcall_t __lkpi_initcalls_end[];

namespace linuxkpi {

void publish_params() { publish_module_params(); }

void init() {
    lkpi_jiffies_var = clock::now_ms();
    sched::spawn("lkpi-timers", timer_thread, nullptr, -1, true);
    sched::spawn("lkpi-rcu", rcu_thread, nullptr, -1, true);
    system_wq = make_wq("lkpi-events");
    // Unbound/high-priority/long work (GPU drivers, tasklets) runs on its
    // own pool, so it isn't stuck behind items that sleep on system_wq.
    system_highpri_wq = system_long_wq = system_unbound_wq = make_wq_workers("lkpi-unbound", WQ_MAX_WORKERS);
    system_freezable_wq = system_power_efficient_wq = system_wq;
    init_pci();
    // GPU acceleration test boots ("gpu"): modesetting debug messages go to
    // the boot log unless drm.debug= says otherwise. Not driver debug
    // (DRM_UT_DRIVER): nouveau logs every GPU fence event there, thousands a
    // second, and writing them out made a GTX 1650 desktop crawl and freeze.
    // Only the first 30000 messages, unless drm.debug= asks for them: the
    // desktop's page flips log several a frame (drm_print.c).
    if (strstr(boot::cmdline(), " gpu") || !strncmp(boot::cmdline(), "gpu", 3)) {
        __drm_debug = 0x04; // DRM_UT_KMS
        if (!strstr(boot::cmdline(), "drm.debug="))
            lkpi_drm_debug_budget = 30000;
    }
    apply_module_params();
    // GPU drivers (probed later) take the display over from the firmware
    // framebuffer when it lies in their memory (glue/gpu.c).
    video::Framebuffer& fb = video::primary_framebuffer();
    if (fb.valid())
        lkpi_boot_fb_set(mm::direct_virt_to_phys(fb.base()), fb.width(), fb.height(), fb.pitch(), fb.red_shift());
    int count = 0, failed = 0;
    for (lkpi_initcall_t* call = __lkpi_initcalls_start; call < __lkpi_initcalls_end; call++) {
        count++;
        if (int r = (*call)(); r != 0) {
            failed++;
            klog::warn("linuxkpi: a driver's module_init failed (%d)", r);
        }
    }
    klog::ok("Linux driver layer (LinuxKPI): %d driver%s registered%s", count, count == 1 ? "" : "s",
             failed ? " (some failed)" : "");
    // The firmware framebuffer for simpledrm (/dev/dri/card0), unless "nodrm".
    if (fb.valid() && !strstr(boot::cmdline(), "nodrm"))
        lkpi_register_boot_framebuffer(mm::direct_virt_to_phys(fb.base()), fb.width(), fb.height(), fb.pitch(),
                                       fb.red_shift());
}

} // namespace linuxkpi

// ---- DRM-core helpers ----
extern "C" bool lkpi_capable(int) { return proc::capable(); }
extern "C" int wbinvd_on_all_cpus(void) { asm volatile("wbinvd" ::: "memory"); return 0; }


// ---- GPU drivers ----
extern "C" bool lkpi_access_ok(const void* addr, unsigned long size) {
    return uaccess::check(addr, size, false) == 0;
}
extern "C" u64 lkpi_local_clock(void) { return clock::now_ns(); }
extern "C" unsigned long lkpi_tsc_khz(void) { return clock::tsc_frequency_hz() / 1000; }

// Kernel FPU sections for Linux drivers (AMD display bandwidth maths).
extern "C" int lkpi_in_interrupt(void);
extern "C" void kernel_fpu_begin(void) {
    if (lkpi_in_interrupt())
        klog::warn("linuxkpi: kernel_fpu_begin() in an interrupt handler");
    fpu::kernel_begin();
}
extern "C" void kernel_fpu_end(void) { fpu::kernel_end(); }
extern "C" bool lkpi_irq_fpu_usable(void) { return !lkpi_in_interrupt(); }
extern "C" bool lkpi_kernel_fpu_active(void) { return fpu::kernel_active(); }
