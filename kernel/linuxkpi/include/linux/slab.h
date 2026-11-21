#ifndef _LKPI_LINUX_SLAB_H
#define _LKPI_LINUX_SLAB_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* GFP flags: Syluxium allocations never fail "softly" by zone, so most
 * flags are hints; __GFP_ZERO is honoured. */
#define __GFP_ZERO 0x100u
#define __GFP_NOWARN 0x200u
#define __GFP_DMA32 0x400u
#define __GFP_NORETRY 0x800u
#define GFP_KERNEL 0x1u
#define GFP_ATOMIC 0x2u
#define GFP_NOWAIT 0x4u
#define GFP_NOIO 0x8u
#define GFP_NOFS 0x10u
#define GFP_DMA 0x20u
#define GFP_DMA32 __GFP_DMA32
#define GFP_USER GFP_KERNEL

/* The kernel has its own (C++) kmalloc family; Linux code gets these. */
#define kmalloc lkpi_kmalloc
#define kzalloc lkpi_kzalloc
#define kcalloc lkpi_kcalloc
#define kmalloc_array lkpi_kmalloc_array
#define krealloc lkpi_krealloc
#define kfree lkpi_kfree
#define kfree_sensitive lkpi_kfree_sensitive
#define ksize lkpi_ksize
#define vmalloc lkpi_vmalloc
#define vzalloc lkpi_vzalloc
#define vfree lkpi_vfree
#define ZERO_SIZE_PTR ((void*)16)
#define ZERO_OR_NULL_PTR(x) ((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)

/* Memory from kmalloc is physically contiguous and direct-mapped, so it can
 * be handed to dma_map_single(), as on Linux. */
void* kmalloc(size_t size, gfp_t flags);
void* kzalloc(size_t size, gfp_t flags);
void* kcalloc(size_t n, size_t size, gfp_t flags);
void* kmalloc_array(size_t n, size_t size, gfp_t flags);
void* krealloc(const void* p, size_t size, gfp_t flags);
void kfree(const void* p);
void kfree_sensitive(const void* p);
size_t ksize(const void* p);
#define kvmalloc kmalloc
#define kvmemdup(src, len, gfp) kmemdup(src, len, gfp)
#define kvzalloc kzalloc
#define kvcalloc kcalloc
#define kvmalloc_array kmalloc_array
#define kvfree kfree
#define kzalloc_node(s, f, n) kzalloc(s, f)
#define kmalloc_node(s, f, n) kmalloc(s, f)
#define devm_kzalloc(dev, s, f) kzalloc(s, f)
#define devm_kmalloc(dev, s, f) kmalloc(s, f)
#define devm_kcalloc(dev, n, s, f) kcalloc(n, s, f)
#define devm_kfree(dev, p) kfree(p)
#define devm_kmemdup(dev, src, len, gfp) kmemdup(src, len, gfp)
#define devm_kstrdup(dev, s, gfp) kstrdup(s, gfp)
#define alloc_hooks(x) (x)
#define kmemdup_noprof(src, len, gfp) kmemdup(src, len, gfp)
bool lkpi_is_vmalloc_addr(const void* p);
#define is_vmalloc_addr(p) lkpi_is_vmalloc_addr(p)
#define kmalloc_obj(p, gfp) kmalloc(sizeof(*(p)), gfp)
#define kmalloc_track_caller kmalloc
#define kmemdup_nul(s, len, gfp) kstrndup(s, len, gfp)
#define kvrealloc(p, n, gfp) krealloc(p, n, gfp)
#define krealloc_array(p, n, s, gfp) krealloc(p, (n) * (s), gfp)
#define kmem_cache_alloc_node(c, f, n) kmem_cache_alloc(c, f)
#define KMALLOC_MAX_SIZE (1UL << 22)
#define __GFP_COMP 0
#define __GFP_NOFAIL 0
#define __GFP_MEMALLOC 0
#define __GFP_RETRY_MAYFAIL 0
#define __GFP_HIGHMEM 0
#define GFP_HIGHUSER 0x1u
#define gfpflags_allow_blocking(f) (((f) & GFP_ATOMIC) == 0)

/* Virtually contiguous (large) allocations. */
void* vmalloc(size_t size);
void* vzalloc(size_t size);
void vfree(const void* p);

/* Object caches: plain fixed-size kmalloc underneath. */
struct kmem_cache {
    size_t size;
    const char* name;
    unsigned long flags;   /* appended: SLAB_TYPESAFE_BY_RCU defers frees past an RCU grace period */
    void (*ctor)(void*);   /* run on every allocation */
};
struct kmem_cache* kmem_cache_create(const char* name, size_t size, size_t align, unsigned long flags,
                                     void (*ctor)(void*));
void kmem_cache_destroy(struct kmem_cache* c);
void* kmem_cache_alloc(struct kmem_cache* c, gfp_t flags);
void* kmem_cache_zalloc(struct kmem_cache* c, gfp_t flags);
void kmem_cache_free(struct kmem_cache* c, void* p);
#define SLAB_HWCACHE_ALIGN 0x2000UL

LKPI_EXTERN_C_END

#endif
