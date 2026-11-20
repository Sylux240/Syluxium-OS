/* LinuxKPI: memory mappings as drivers see them. file.cpp builds a
 * vm_area_struct when a bridged file is mmap()ed; the driver's f_op->mmap
 * either maps a physical range (remap_pfn_range / io_remap_pfn_range) or
 * installs vm_ops->fault, which supplies pages (vmf_insert_pfn / page). */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/fs.h>

typedef struct { unsigned long pgprot; } pgprot_t;
#define __pgprot(x) ((pgprot_t){ (x) })
#define pgprot_val(x) ((x).pgprot)
#define LKPI_PROT_WC 0x10UL /* write-combining */
#define LKPI_PROT_UC 0x20UL /* uncached */
#define pgprot_writecombine(p) __pgprot((p).pgprot | LKPI_PROT_WC)
#define pgprot_noncached(p) __pgprot((p).pgprot | LKPI_PROT_UC)
#define pgprot_val(p) ((p).pgprot)
#ifndef PAGE_KERNEL
#define PAGE_KERNEL __pgprot(0)
#define PAGE_KERNEL_IO __pgprot(LKPI_PROT_UC)
#endif
#define pgprot_decrypted(p) (p)

typedef unsigned int vm_fault_t;
#define VM_FAULT_OOM 0x0001
#define VM_FAULT_SIGBUS 0x0002
#define VM_FAULT_NOPAGE 0x0100
#define VM_FAULT_LOCKED 0x0200
#define VM_FAULT_RETRY 0x0400
#define VM_FAULT_ERROR (VM_FAULT_OOM | VM_FAULT_SIGBUS)

typedef unsigned long vm_flags_t;
#define VM_READ 0x00000001
#define VM_WRITE 0x00000002
#define VM_EXEC 0x00000004
#define VM_SHARED 0x00000008
#define VM_MAYREAD 0x00000010
#define VM_MAYSHARE 0x00000080
#define VM_MAYWRITE 0x00000020
#define VM_IO 0x00004000
#define VM_DONTCOPY 0x00020000
#define VM_DONTEXPAND 0x00040000
#define VM_NORESERVE 0x00200000
#define VM_PFNMAP 0x00000400
#define VM_MIXEDMAP 0x10000000
#define VM_DONTDUMP 0x04000000

struct vm_area_struct;
struct vm_fault {
	struct vm_area_struct *vma;
	unsigned long address;
	unsigned long pgoff;
	unsigned int flags;
	struct page *page;
};
struct vm_operations_struct {
	void (*open)(struct vm_area_struct *);
	void (*close)(struct vm_area_struct *);
	vm_fault_t (*fault)(struct vm_fault *);
	vm_fault_t (*page_mkwrite)(struct vm_fault *);
	int (*access)(struct vm_area_struct *, unsigned long, void *, int, int);
};
struct vm_area_struct {
	unsigned long vm_start, vm_end;
	unsigned long vm_pgoff;
	vm_flags_t vm_flags;
	pgprot_t vm_page_prot;
	const struct vm_operations_struct *vm_ops;
	void *vm_private_data;
	struct file *vm_file;
	void *lkpi; /* glue/file.c: struct lkpi_vma */
	struct mm_struct *vm_mm;
};
#define vma_pages(vma) (((vma)->vm_end - (vma)->vm_start) >> PAGE_SHIFT)
#define vm_flags_set(vma, f) ((vma)->vm_flags |= (f))
#define vm_flags_clear(vma, f) ((vma)->vm_flags &= ~(f))
#define vm_flags_mod(vma, set, clr) ((vma)->vm_flags = ((vma)->vm_flags | (set)) & ~(clr))
#define vm_get_page_prot(f) __pgprot(0)
struct file *get_file(struct file *f);
void fput(struct file *f);
static inline void vma_set_file(struct vm_area_struct *vma, struct file *f)
{
	if (f)
		get_file(f);
	if (vma->vm_file)
		fput(vma->vm_file);
	vma->vm_file = f;
}
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot);
#define io_remap_pfn_range remap_pfn_range
vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn);
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, pgprot_t prot);
#define vmf_insert_mixed(vma, addr, pfn) vmf_insert_pfn(vma, addr, pfn)
