/*
 * LinuxKPI: the Linux side of files. struct file objects are allocated and
 * reference counted here and their f_op methods called; file.cpp wraps them
 * as Syluxium inodes/files (descriptors, character devices).
 */
#include <linux/lkpi_all.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/anon_inodes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

/* ---- Syluxium side (file.cpp) ---- */
int lkpi_vfs_fd_reserve(unsigned int flags);
void lkpi_vfs_fd_install(unsigned int fd, struct file *f);
void lkpi_vfs_fd_release(unsigned int fd);
struct file *lkpi_vfs_fget(unsigned int fd);

struct file *lkpi_file_alloc(const struct file_operations *fops, void *priv, unsigned int flags, struct inode *inode)
{
	struct file *f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	if (!inode) {
		inode = kzalloc(sizeof(*inode), GFP_KERNEL);
		if (!inode) {
			kfree(f);
			return NULL;
		}
		inode->i_mapping = &inode->i_data;
		inode->i_data.host = inode;
	}
	f->f_op = fops;
	f->private_data = priv;
	f->f_flags = flags;
	f->f_mode = ((flags & O_ACCMODE) != O_WRONLY ? FMODE_READ : 0) | ((flags & O_ACCMODE) != O_RDONLY ? FMODE_WRITE : 0);
	f->f_inode = inode;
	f->f_mapping = inode->i_mapping;
	atomic_long_set(&f->f_count, 1);
	return f;
}

struct file *get_file(struct file *f)
{
	atomic_long_inc(&f->f_count);
	return f;
}

/* Last reference: the driver's release, then the memory. */
void fput(struct file *f)
{
	if (!f || atomic_long_dec_return(&f->f_count) != 0)
		return;
	if (f->f_op && f->f_op->release)
		f->f_op->release(f->f_inode, f);
	kfree(f);
}

struct file *anon_inode_getfile(const char *name, const struct file_operations *fops, void *priv, int flags)
{
	struct file *f = lkpi_file_alloc(fops, priv, flags, NULL);
	return f ? f : ERR_PTR(-ENOMEM);
}

int anon_inode_getfd(const char *name, const struct file_operations *fops, void *priv, int flags)
{
	int fd = get_unused_fd_flags(flags);
	struct file *f;
	if (fd < 0)
		return fd;
	f = anon_inode_getfile(name, fops, priv, flags);
	if (IS_ERR(f)) {
		put_unused_fd(fd);
		return PTR_ERR(f);
	}
	fd_install(fd, f);
	return fd;
}

int get_unused_fd_flags(unsigned int flags) { return lkpi_vfs_fd_reserve(flags); }
void put_unused_fd(unsigned int fd) { lkpi_vfs_fd_release(fd); }
void fd_install(unsigned int fd, struct file *f) { lkpi_vfs_fd_install(fd, f); }
struct file *fget(unsigned int fd) { return lkpi_vfs_fget(fd); }

loff_t noop_llseek(struct file *f, loff_t offset, int whence) { return f->f_pos; }
loff_t no_llseek(struct file *f, loff_t offset, int whence) { return -ESPIPE; }
loff_t default_llseek(struct file *f, loff_t offset, int whence)
{
	loff_t pos = whence == SEEK_SET ? offset : whence == SEEK_CUR ? f->f_pos + offset : f->f_inode->i_size + offset;
	if (pos < 0)
		return -EINVAL;
	f->f_pos = pos;
	return pos;
}
int nonseekable_open(struct inode *inode, struct file *f) { return 0; }
int stream_open(struct inode *inode, struct file *f) { return 0; }

/* Called by file.cpp for system calls on bridged files. */
long lkpi_file_read(struct file *f, char __user *buf, size_t n, long long *pos)
{
	return f->f_op && f->f_op->read ? f->f_op->read(f, buf, n, pos) : -EINVAL;
}
long lkpi_file_write(struct file *f, const char __user *buf, size_t n, long long *pos)
{
	return f->f_op && f->f_op->write ? f->f_op->write(f, buf, n, pos) : -EINVAL;
}
long lkpi_file_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	return f->f_op && f->f_op->unlocked_ioctl ? f->f_op->unlocked_ioctl(f, cmd, arg) : -ENOTTY;
}
unsigned int lkpi_file_poll(struct file *f)
{
	poll_table pt;
	if (!f->f_op || !f->f_op->poll)
		return EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM;
	return f->f_op->poll(f, &pt);
}

/* Character devices: one file_operations per major number. */
#define LKPI_MAX_CHRDEV 8
static struct { unsigned int major; const struct file_operations *fops; } chrdevs[LKPI_MAX_CHRDEV];

int register_chrdev(unsigned int major, const char *name, const struct file_operations *fops)
{
	for (int i = 0; i < LKPI_MAX_CHRDEV; i++)
		if (!chrdevs[i].fops) {
			chrdevs[i].major = major;
			chrdevs[i].fops = fops;
			return 0;
		}
	return -EBUSY;
}
void unregister_chrdev(unsigned int major, const char *name)
{
	for (int i = 0; i < LKPI_MAX_CHRDEV; i++)
		if (chrdevs[i].major == major)
			chrdevs[i].fops = NULL;
}

/* open() of /dev/<node> for (major, minor): NULL + *err on failure. */
struct file *lkpi_chrdev_open(unsigned int major, unsigned int minor, unsigned int flags, int *err)
{
	const struct file_operations *fops = NULL;
	struct inode *inode;
	struct file *f;
	for (int i = 0; i < LKPI_MAX_CHRDEV; i++)
		if (chrdevs[i].fops && chrdevs[i].major == major)
			fops = chrdevs[i].fops;
	if (!fops) {
		*err = -ENODEV;
		return NULL;
	}
	inode = kzalloc(sizeof(*inode), GFP_KERNEL);
	if (!inode) {
		*err = -ENOMEM;
		return NULL;
	}
	inode->i_rdev = (major << 20) | minor;
	inode->i_mode = 0020000; /* S_IFCHR */
	inode->i_mapping = &inode->i_data;
	inode->i_data.host = inode;
	f = lkpi_file_alloc(fops, NULL, flags, inode);
	if (!f) {
		kfree(inode);
		*err = -ENOMEM;
		return NULL;
	}
	if (fops->open) {
		*err = fops->open(inode, f);
		if (*err) {
			kfree(f);
			kfree(inode);
			return NULL;
		}
	}
	*err = 0;
	return f;
}

/* ---- pseudo file systems (dma-buf's internal "dmabuf" mount) ---- */
#include <linux/mount.h>
#include <linux/pseudo_fs.h>

struct vfsmount *kern_mount(struct file_system_type *type)
{
	struct vfsmount *m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return ERR_PTR(-ENOMEM);
	m->mnt_sb = kzalloc(sizeof(*m->mnt_sb), GFP_KERNEL);
	return m;
}
struct pseudo_fs_context *init_pseudo(struct fs_context *fc, unsigned long magic)
{
	struct pseudo_fs_context *c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (c)
		c->magic = magic;
	return c;
}
struct inode *alloc_anon_inode(struct super_block *sb)
{
	struct inode *i = kzalloc(sizeof(*i), GFP_KERNEL);
	if (!i)
		return ERR_PTR(-ENOMEM);
	i->i_mapping = &i->i_data;
	i->i_data.host = i;
	return i;
}
struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt, const char *name, int flags,
			       const struct file_operations *fops)
{
	struct file *f = lkpi_file_alloc(fops, NULL, flags, inode);
	struct dentry *d;
	if (!f)
		return ERR_PTR(-ENOMEM);
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (d) {
		d->d_name.name = name;
		d->d_name.len = strlen(name);
		d->d_inode = inode;
	}
	f->f_path.dentry = d;
	return f;
}
char *dynamic_dname(char *buf, int buflen, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, buflen, fmt, args);
	va_end(args);
	return buf;
}
char *strndup_user(const char __user *s, long n)
{
	char *p = kmalloc(n, GFP_KERNEL);
	long i;
	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(p, s, n)) {
		/* shorter than n near the end of user memory: copy byte by byte */
		for (i = 0; i < n; i++) {
			if (copy_from_user(p + i, s + i, 1)) {
				kfree(p);
				return ERR_PTR(-EFAULT);
			}
			if (!p[i])
				return p;
		}
	}
	p[n - 1] = 0;
	return p;
}

/* ---- mmap: driver mappings (f_op->mmap) as Syluxium device mappings ----
 * Each mmap() of a Linux file gets a vm_area_struct the driver sets up.
 * The Syluxium mapping then uses a substitute file offset naming that VMA
 * ((id + 1) << LKPI_VMA_SHIFT + offset within it), and page faults ask
 * lkpi_vma_phys() for the physical page: the range a remap_pfn_range()
 * gave, or whatever the driver's fault handler inserts. */
#include <linux/mm.h>
#define LKPI_VMA_SHIFT 36
#define LKPI_MAX_VMAS 4096

struct lkpi_vma {
	struct vm_area_struct vma;
	struct mutex lock;
	unsigned long base_pfn; /* remap_pfn_range: pfn at vm_start */
	bool remapped;
	unsigned long fault_pfn;
	bool fault_set;
};
static struct lkpi_vma *lkpi_vmas[LKPI_MAX_VMAS];
static DEFINE_SPINLOCK(lkpi_vmas_lock);

long lkpi_file_mmap(struct file *f, unsigned long offset, unsigned long len, bool writable, unsigned long *handle)
{
	struct lkpi_vma *m;
	int id, r;
	if (!f || !f->f_op || !f->f_op->mmap)
		return -ENODEV;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	spin_lock(&lkpi_vmas_lock);
	for (id = 0; id < LKPI_MAX_VMAS && lkpi_vmas[id]; id++)
		;
	if (id < LKPI_MAX_VMAS)
		lkpi_vmas[id] = m;
	spin_unlock(&lkpi_vmas_lock);
	if (id == LKPI_MAX_VMAS) {
		kfree(m);
		return -ENOMEM;
	}
	mutex_init(&m->lock);
	m->vma.vm_start = (unsigned long)(id + 1) << LKPI_VMA_SHIFT;
	m->vma.vm_end = m->vma.vm_start + PAGE_ALIGN(len);
	m->vma.vm_pgoff = offset >> PAGE_SHIFT;
	m->vma.vm_flags = VM_SHARED | VM_MAYSHARE | VM_READ | VM_MAYREAD | (writable ? VM_WRITE | VM_MAYWRITE : 0);
	m->vma.vm_page_prot = PAGE_KERNEL;
	m->vma.vm_file = get_file(f);
	m->vma.lkpi = m;
	r = f->f_op->mmap(f, &m->vma);
	if (r) {
		fput(m->vma.vm_file);
		spin_lock(&lkpi_vmas_lock);
		lkpi_vmas[id] = NULL;
		spin_unlock(&lkpi_vmas_lock);
		kfree(m);
		return r;
	}
	*handle = m->vma.vm_start;
	return 0;
}

long lkpi_vma_phys(unsigned long off, u64 *phys)
{
	unsigned long id = (off >> LKPI_VMA_SHIFT) - 1;
	struct lkpi_vma *m = id < LKPI_MAX_VMAS ? lkpi_vmas[id] : NULL;
	unsigned long addr = off & ~(PAGE_SIZE - 1);
	struct vm_fault vmf = { 0 };
	vm_fault_t ret;
	if (!m || addr >= m->vma.vm_end)
		return -ENXIO;
	if (m->remapped) {
		*phys = PFN_PHYS(m->base_pfn) + (addr - m->vma.vm_start);
		return 0;
	}
	if (!m->vma.vm_ops || !m->vma.vm_ops->fault)
		return -ENXIO;
	mutex_lock(&m->lock);
	m->fault_set = false;
	vmf.vma = &m->vma;
	vmf.address = addr;
	vmf.pgoff = m->vma.vm_pgoff + ((addr - m->vma.vm_start) >> PAGE_SHIFT);
	ret = m->vma.vm_ops->fault(&vmf);
	if (m->fault_set)
		*phys = PFN_PHYS(m->fault_pfn);
	else if (vmf.page && !(ret & (VM_FAULT_SIGBUS | VM_FAULT_OOM)))
		*phys = page_to_phys(vmf.page);
	else
		ret |= VM_FAULT_SIGBUS;
	mutex_unlock(&m->lock);
	return (ret & (VM_FAULT_SIGBUS | VM_FAULT_OOM)) ? -EFAULT : 0;
}

int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot)
{
	struct lkpi_vma *m = vma->lkpi;
	m->base_pfn = pfn - ((addr - vma->vm_start) >> PAGE_SHIFT);
	m->remapped = true;
	return 0;
}

vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, pgprot_t prot)
{
	struct lkpi_vma *m = vma->lkpi;
	m->fault_pfn = pfn;
	m->fault_set = true;
	return VM_FAULT_NOPAGE;
}

vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn)
{
	return vmf_insert_pfn_prot(vma, addr, pfn, vma->vm_page_prot);
}
