/* LinuxKPI: resources (memory / port ranges of devices). */
#ifndef _LKPI_LINUX_IOPORT_H
#define _LKPI_LINUX_IOPORT_H
#include <linux/types.h>

#ifndef _LKPI_RESOURCE_SIZE_T
#define _LKPI_RESOURCE_SIZE_T
typedef u64 resource_size_t;
#endif
struct resource { resource_size_t start, end; const char *name; unsigned long flags; struct resource *parent, *sibling, *child; unsigned long desc; };
#define resource_size(r) ((r)->end - (r)->start + 1)
#define DEFINE_RES_MEM(_start, _size) ((struct resource){ .start = (_start), .end = (_start) + (_size) - 1, .flags = 0x200 })
#define DEFINE_RES_NAMED(_start, _size, _name, _flags) ((struct resource){ .start = (_start), .end = (_start) + (_size) - 1, .name = (_name), .flags = (_flags) })
#ifndef IORESOURCE_IO
#define IORESOURCE_IO 0x00000100
#define IORESOURCE_MEM 0x00000200
#define IORESOURCE_PREFETCH 0x00002000
#define IORESOURCE_MEM_64 0x00100000
#endif
#define IORESOURCE_BITS 0x000000ff
#define IORESOURCE_TYPE_BITS 0x00001f00
#define IORESOURCE_REG 0x00000300
#define IORESOURCE_IRQ 0x00000400
#define IORESOURCE_DMA 0x00000800
#define IORESOURCE_BUS 0x00001000
#define IORESOURCE_READONLY 0x00004000
#define IORESOURCE_CACHEABLE 0x00008000
#define IORESOURCE_SIZEALIGN 0x00040000
#define IORESOURCE_DISABLED 0x10000000
#define IORESOURCE_UNSET 0x20000000
#define IORESOURCE_BUSY 0x80000000
#define IORESOURCE_ROM_SHADOW (1 << 1)
#define resource_type(r) ((r)->flags & IORESOURCE_TYPE_BITS)
#define resource_contains(r1, r2) ((r1)->start <= (r2)->start && (r1)->end >= (r2)->end)
#define resource_overlaps(r1, r2) ((r1)->start <= (r2)->end && (r1)->end >= (r2)->start)

/* Region reservations: nothing else claims device memory here; callers get
 * a placeholder resource (simpledrm's devm_request_mem_region gets NULL and
 * uses the device's own resource, see lkpi_extras.h). */
extern struct resource lkpi_dummy_resource;
#define request_region(start, n, name) (&lkpi_dummy_resource)
#define release_region(start, n) do { } while (0)
#define request_mem_region(start, n, name) (&lkpi_dummy_resource)
#define release_mem_region(start, n) do { } while (0)
#define request_resource(root, new) 0
#define release_resource(r) 0
#define insert_resource(parent, new) 0
#define remove_resource(r) 0
#define iomem_is_exclusive(addr) false
#define walk_iomem_res_desc(desc, flags, start, end, arg, func) (-ENOSYS)
extern struct resource iomem_resource, ioport_resource;
#endif
