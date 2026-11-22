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
#include <linux/uio.h>
#include <linux/lkpi_binder.h>

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
	f->f_cred = lkpi_current_cred();
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
	put_cred(f->f_cred);
	kfree(f);
}

/* A Syluxium file (memfd, socket, pipe, ...) carried through Linux code:
 * binder passes descriptors between processes. The wrapper holds a
 * reference to the vfs::File; fd_install() puts the file itself back. */
void lkpi_native_put(void *vfs_file);
static int native_release(struct inode *inode, struct file *f)
{
	lkpi_native_put(f->lkpi_native);
	kfree(inode);
	return 0;
}
static const struct file_operations native_fops = { .release = native_release };
struct file *lkpi_native_wrap(void *vfs_file)
{
	struct file *f = lkpi_file_alloc(&native_fops, NULL, O_RDWR, NULL);
	if (f)
		f->lkpi_native = vfs_file;
	else
		lkpi_native_put(vfs_file);
	return f;
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
/* read_iter/write_iter: the program's data goes through a kernel buffer
 * (iterators here are kernel vectors only). */
static long file_iter(struct file *f, char __user *buf, size_t n, long long *pos, bool write)
{
	struct kiocb iocb = { .ki_filp = f, .ki_pos = *pos, .ki_flags = (f->f_flags & O_NONBLOCK) ? IOCB_NOWAIT : 0 };
	struct kvec v;
	struct iov_iter iter;
	long r;
	void *kbuf = kvmalloc(n ? n : 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (write && copy_from_user(kbuf, buf, n)) {
		kvfree(kbuf);
		return -EFAULT;
	}
	v.iov_base = kbuf;
	v.iov_len = n;
	iov_iter_kvec(&iter, write ? ITER_SOURCE : ITER_DEST, &v, 1, n);
	r = write ? f->f_op->write_iter(&iocb, &iter) : f->f_op->read_iter(&iocb, &iter);
	if (!write && r > 0 && copy_to_user(buf, kbuf, r))
		r = -EFAULT;
	*pos = iocb.ki_pos;
	kvfree(kbuf);
	return r;
}

long lkpi_file_read(struct file *f, char __user *buf, size_t n, long long *pos)
{
	if (f->f_op && !f->f_op->read && f->f_op->read_iter)
		return file_iter(f, buf, n, pos, false);
	return f->f_op && f->f_op->read ? f->f_op->read(f, buf, n, pos) : -EINVAL;
}
long lkpi_file_write(struct file *f, const char __user *buf, size_t n, long long *pos)
{
	if (f->f_op && !f->f_op->write && f->f_op->write_iter)
		return file_iter(f, (char __user *)buf, n, pos, true);
	return f->f_op && f->f_op->write ? f->f_op->write(f, buf, n, pos) : -EINVAL;
}
bool lkpi_cmdline_word(const char *w); /* pci.cpp */

/* Boot option "drmtrace": every driver ioctl on a DRM node (amdgpu CS/BO/
 * INFO, nouveau NVIF/GEM/channels, ...) with its result and time, the first
 * 6000, to find where a GPU program stops (Mesa gives up on several paths
 * without a word). nouveau's NVIF calls also show the NVIF operation, the
 * object class created or the method called and the NVIF status. */
const char *lkpi_drm_file_driver(struct file *filp); /* drm_file.c */

static void drm_trace(struct file *f, unsigned int cmd, unsigned long arg, long r, unsigned long ms)
{
	static int traced;
	char extra[96] = "";
	const char *drv = lkpi_drm_file_driver(f);

	if (traced >= 6000)
		return;
	traced++;
	if (drv && !strcmp(drv, "nouveau") && _IOC_NR(cmd) == 0x47 && arg) { /* DRM_NOUVEAU_NVIF */
		/* struct nvif_ioctl_v0 {version, type, pad[4], owner, route,
		 * token, object} (24 bytes), then the operation's arguments:
		 * new_v0 {version, pad[6], route, token, object, handle, oclass},
		 * mthd_v0 {version, method, ...} */
		u8 head[56] = {0};
		size_t n = min_t(size_t, _IOC_SIZE(cmd), sizeof(head));
		if (n >= 26 && !copy_from_user(head, (void __user *)arg, n)) {
			static const char *const types[] = {"nop", "sclass", "new", "del", "mthd", "rd", "wr",
							    "map", "unmap"};
			u8 type = head[1];
			const char *tn = type < ARRAY_SIZE(types) ? types[type] : "?";
			if (type == 2 && n >= 56)
				snprintf(extra, sizeof(extra), " nvif %s oclass %#x", tn, *(s32 *)&head[52]);
			else if (type == 4)
				snprintf(extra, sizeof(extra), " nvif %s %#x", tn, head[25]);
			else
				snprintf(extra, sizeof(extra), " nvif %s", tn);
		}
	}
	pr_info("drmtrace: %s[%d] %s ioctl 0x%02x size %u -> %ld (%lu ms)%s%s\n", current->comm, current->pid,
		drv ? drv : "?", _IOC_NR(cmd), _IOC_SIZE(cmd), r, ms, extra, traced == 6000 ? " (no more)" : "");
}

long lkpi_file_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	static int logged, slow_logged, trace = -1;
	u64 start = ktime_get_ns();
	long r = f->f_op && f->f_op->unlocked_ioctl ? f->f_op->unlocked_ioctl(f, cmd, arg) : -ENOTTY;
	unsigned long ms = (unsigned long)((ktime_get_ns() - start) / 1000000);

	if (_IOC_TYPE(cmd) == 'd') {
		if (trace < 0)
			trace = lkpi_cmdline_word("drmtrace");
		if (trace && _IOC_NR(cmd) >= 0x40)
			drm_trace(f, cmd, arg, r, ms);
		/* A GPU ioctl that takes seconds (a fence or the GPU not
		 * answering: nouveau gives up on a channel after 15 s) is
		 * always worth a line. */
		if (ms >= 2000 && slow_logged < 100) {
			slow_logged++;
			pr_warn("drm: %s[%d]: ioctl 0x%02x (cmd %#x) took %lu ms, result %ld\n", current->comm,
				current->pid, _IOC_NR(cmd), cmd, ms, r);
		}
	}
	/* Failing GPU ioctls (type 'd'; not the mode-setting ones, whose probing
	 * failures are routine), the first 60: Mesa aborts the X server
	 * when command submission fails, often before its log reaches disk. */
	if (r < 0 && _IOC_TYPE(cmd) == 'd' && (_IOC_NR(cmd) < 0xa0 || _IOC_NR(cmd) > 0xbe) && r != -EINTR && r != -EAGAIN && r != -ERESTARTSYS &&
	    r != -ETIME && logged < 60) {
		logged++;
		pr_warn("drm: %s[%d]: ioctl 0x%02x (cmd %#x) failed: %ld%s\n", current->comm, current->pid,
			_IOC_NR(cmd), cmd, r, logged == 60 ? " (no more of these)" : "");
	}
	return r;
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
#define LKPI_MAX_VMAS 16384 /* Mesa keeps most buffer objects mapped */

struct lkpi_vma {
	struct vm_area_struct vma;
	struct mutex lock;
	int refs;               /* Syluxium mappings (VMAs, split parts, fork copies) + the mmap() call */
	unsigned long key;      /* the substitute file offset Syluxium maps it by ((id + 1) << LKPI_VMA_SHIFT) */
	unsigned long base_pfn; /* remap_pfn_range: pfn at vm_start */
	bool remapped;
	unsigned long npages;
	u64 *pages;             /* per page: phys | LKPI_PTE_* (vmf_insert_pfn & co., prefaulted pages) */
};
#define LKPI_PTE_VALID 0x1ULL
#define LKPI_PTE_WC 0x2ULL
#define LKPI_PTE_UC 0x4ULL
static struct lkpi_vma *lkpi_vmas[LKPI_MAX_VMAS];
static DEFINE_SPINLOCK(lkpi_vmas_lock);
static int lkpi_vmas_next; /* where the search for a free slot starts */
static int lkpi_vmas_used;

static u64 prot_bits(pgprot_t prot)
{
	if (pgprot_val(prot) & LKPI_PROT_WC)
		return LKPI_PTE_WC;
	if (pgprot_val(prot) & LKPI_PROT_UC)
		return LKPI_PTE_UC;
	return 0;
}

static struct lkpi_vma *vma_of(unsigned long off)
{
	unsigned long id = (off >> LKPI_VMA_SHIFT) - 1;
	return id < LKPI_MAX_VMAS ? lkpi_vmas[id] : NULL;
}

/* The mapping starts with one reference, which sys_mmap drops once the
 * address-space mapping (which takes its own) exists. */
struct mm_struct *lkpi_current_mm(void);
/* user_addr: where the mapping will be (0: not known yet). Drivers that ask
 * for it (lkpi_user_address: binder hands out pointers into its buffers) see
 * it as vm_start; for the others vm_start is the substitute offset. */
long lkpi_file_mmap(struct file *f, unsigned long offset, unsigned long len, bool writable, unsigned long user_addr,
		    unsigned long *handle)
{
	struct lkpi_vma *m;
	int id, r;
	if (!f || !f->f_op || !f->f_op->mmap)
		return -ENODEV;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->npages = PAGE_ALIGN(len) >> PAGE_SHIFT;
	m->pages = kvzalloc(m->npages * sizeof(u64), GFP_KERNEL);
	if (!m->pages) {
		kfree(m);
		return -ENOMEM;
	}
	spin_lock(&lkpi_vmas_lock);
	for (int n = 0; n < LKPI_MAX_VMAS; n++) {
		id = (lkpi_vmas_next + n) % LKPI_MAX_VMAS;
		if (!lkpi_vmas[id])
			break;
	}
	if (lkpi_vmas[id]) {
		id = LKPI_MAX_VMAS;
	} else {
		lkpi_vmas[id] = m;
		lkpi_vmas_next = id + 1;
		lkpi_vmas_used++;
	}
	spin_unlock(&lkpi_vmas_lock);
	if (id == LKPI_MAX_VMAS) {
		static bool told;
		if (!told) {
			told = true;
			pr_err("mmap: all %d driver mappings in use (%s[%d]); GPU programs will report out of memory\n",
			       LKPI_MAX_VMAS, current->comm, current->pid);
		}
		kvfree(m->pages);
		kfree(m);
		return -ENOMEM;
	}
	mutex_init(&m->lock);
	m->refs = 1;
	m->key = (unsigned long)(id + 1) << LKPI_VMA_SHIFT;
	m->vma.vm_start = f->f_op->lkpi_user_address && user_addr ? user_addr : m->key;
	m->vma.vm_end = m->vma.vm_start + PAGE_ALIGN(len);
	if (f->f_op->lkpi_user_address)
		m->vma.vm_mm = lkpi_current_mm();
	m->vma.vm_pgoff = offset >> PAGE_SHIFT;
	m->vma.vm_flags = VM_SHARED | VM_MAYSHARE | VM_READ | VM_MAYREAD | (writable ? VM_WRITE | VM_MAYWRITE : 0);
	m->vma.vm_page_prot = PAGE_KERNEL;
	m->vma.vm_file = get_file(f);
	m->vma.lkpi = m;
	r = f->f_op->mmap(f, &m->vma);
	if (r) {
		static int logged;
		if (logged++ < 30)
			pr_warn("mmap: %s[%d]: driver refused a mapping (offset %#lx, %lu bytes): %d\n", current->comm,
				current->pid, offset, len, r);
		fput(m->vma.vm_file);
		spin_lock(&lkpi_vmas_lock);
		lkpi_vmas[id] = NULL;
		lkpi_vmas_used--;
		spin_unlock(&lkpi_vmas_lock);
		kvfree(m->pages);
		kfree(m);
		return r;
	}
	*handle = m->key;
	return 0;
}

/* A Syluxium mapping of this VMA appeared (+1) or went away (-1). The last
 * one closes it: vm_ops->close (GEM drops the object reference) and the
 * file reference the mapping held. */
void lkpi_vma_ref(unsigned long off, int delta)
{
	struct lkpi_vma *m;
	unsigned long id = (off >> LKPI_VMA_SHIFT) - 1;
	bool last = false;
	spin_lock(&lkpi_vmas_lock);
	m = id < LKPI_MAX_VMAS ? lkpi_vmas[id] : NULL;
	if (m) {
		m->refs += delta;
		if (m->refs <= 0) {
			lkpi_vmas[id] = NULL;
			lkpi_vmas_used--;
			last = true;
		}
	}
	spin_unlock(&lkpi_vmas_lock);
	if (!last)
		return;
	if (m->vma.vm_ops && m->vma.vm_ops->close)
		m->vma.vm_ops->close(&m->vma);
	fput(m->vma.vm_file);
	kvfree(m->pages);
	kfree(m);
}

/* Cache attributes for the page at `off`: 0 (write-back), 1 (write-combining), 2 (uncached). */
int lkpi_vma_cache(unsigned long off)
{
	struct lkpi_vma *m = vma_of(off);
	unsigned long pg;
	u64 bits;
	if (!m)
		return 0;
	pg = ((off & ~(PAGE_SIZE - 1)) - m->key) >> PAGE_SHIFT;
	bits = pg < m->npages && (m->pages[pg] & LKPI_PTE_VALID) ? m->pages[pg] : prot_bits(m->vma.vm_page_prot);
	return (bits & LKPI_PTE_WC) ? 1 : (bits & LKPI_PTE_UC) ? 2 : 0;
}

long lkpi_vma_phys(unsigned long off, u64 *phys)
{
	struct lkpi_vma *m = vma_of(off);
	unsigned long addr = off & ~(PAGE_SIZE - 1);
	struct vm_fault vmf = { 0 };
	vm_fault_t ret;
	unsigned long pg;
	if (!m || addr >= m->key + (m->npages << PAGE_SHIFT))
		return -ENXIO;
	pg = (addr - m->key) >> PAGE_SHIFT;
	if (m->remapped) {
		*phys = PFN_PHYS(m->base_pfn) + (addr - m->key);
		return 0;
	}
	mutex_lock(&m->lock);
	if (m->pages[pg] & LKPI_PTE_VALID) {
		*phys = m->pages[pg] & PAGE_MASK;
		mutex_unlock(&m->lock);
		return 0;
	}
	if (!m->vma.vm_ops || !m->vma.vm_ops->fault) {
		mutex_unlock(&m->lock);
		return -ENXIO;
	}
	vmf.vma = &m->vma;
	vmf.address = m->vma.vm_start + (pg << PAGE_SHIFT);
	vmf.pgoff = m->vma.vm_pgoff + pg;
	ret = m->vma.vm_ops->fault(&vmf);
	if (m->pages[pg] & LKPI_PTE_VALID)
		*phys = m->pages[pg] & PAGE_MASK;
	else if (vmf.page && !(ret & (VM_FAULT_SIGBUS | VM_FAULT_OOM))) {
		*phys = page_to_phys(vmf.page);
		m->pages[pg] = *phys | LKPI_PTE_VALID | prot_bits(m->vma.vm_page_prot);
		/* The fault handler's page reference is kept (as a PTE would). */
	} else
		ret |= VM_FAULT_SIGBUS;
	mutex_unlock(&m->lock);
	if (ret & (VM_FAULT_SIGBUS | VM_FAULT_OOM)) {
		static int logged;
		if (logged++ < 30)
			pr_warn("mmap: %s[%d]: driver fault failed at page %lu of a %lu-page mapping (%#x)\n", current->comm,
				current->pid, pg, m->npages, ret);
	}
	return (ret & (VM_FAULT_SIGBUS | VM_FAULT_OOM)) ? -EFAULT : 0;
}

int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot)
{
	struct lkpi_vma *m = vma->lkpi;
	/* Whole-VMA remaps (the common case) need no table. */
	if (addr == vma->vm_start && size >= vma->vm_end - vma->vm_start && !m->remapped) {
		m->base_pfn = pfn;
		m->remapped = true;
		vma->vm_page_prot = prot;
		return 0;
	}
	for (unsigned long a = addr; a < addr + size && a < vma->vm_end; a += PAGE_SIZE, pfn++) {
		unsigned long pg = (a - vma->vm_start) >> PAGE_SHIFT;
		if (pg < m->npages)
			m->pages[pg] = PFN_PHYS(pfn) | LKPI_PTE_VALID | prot_bits(prot);
	}
	return 0;
}

vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, pgprot_t prot)
{
	struct lkpi_vma *m = vma->lkpi;
	unsigned long pg = (addr - vma->vm_start) >> PAGE_SHIFT;
	if (addr < vma->vm_start || pg >= m->npages)
		return VM_FAULT_SIGBUS;
	m->pages[pg] = PFN_PHYS(pfn) | LKPI_PTE_VALID | prot_bits(prot);
	return VM_FAULT_NOPAGE;
}

vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn)
{
	return vmf_insert_pfn_prot(vma, addr, pfn, vma->vm_page_prot);
}

/* Revoke user mappings of [holebegin, holebegin + holelen) of a file (GEM /
 * TTM buffers moving, i915 fences being stolen): forget the pages the driver
 * supplied for those offsets and unmap them in every process; the next
 * access faults into the driver again. holelen 0 = to the end. */
void lkpi_zap_user_range(u64 offset, u64 len);
void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin, loff_t const holelen, int even_cows)
{
	unsigned long first = holebegin >> PAGE_SHIFT;
	unsigned long last = holelen ? (unsigned long)((holebegin + holelen + PAGE_SIZE - 1) >> PAGE_SHIFT) : ~0UL;
	for (int id = 0; id < LKPI_MAX_VMAS; id++) {
		struct lkpi_vma *m;
		unsigned long beg, end, zbeg, zend;
		spin_lock(&lkpi_vmas_lock);
		m = lkpi_vmas[id];
		if (m)
			m->refs++; /* keep it while we work on it */
		spin_unlock(&lkpi_vmas_lock);
		if (!m)
			continue;
		beg = m->vma.vm_pgoff;
		end = m->vma.vm_pgoff + m->npages;
		zbeg = max(beg, first);
		zend = min(end, last);
		if (m->vma.vm_file && m->vma.vm_file->f_mapping == mapping && zbeg < zend) {
			mutex_lock(&m->lock);
			if (m->remapped) { /* a whole-VMA remap_pfn_range: fall back to the per-page table */
				for (unsigned long i = 0; i < m->npages; i++)
					m->pages[i] = PFN_PHYS(m->base_pfn + i) | LKPI_PTE_VALID | prot_bits(m->vma.vm_page_prot);
				m->remapped = false;
			}
			for (unsigned long pg = zbeg; pg < zend; pg++)
				m->pages[pg - beg] = 0;
			mutex_unlock(&m->lock);
			lkpi_zap_user_range(m->key + ((zbeg - beg) << PAGE_SHIFT), (zend - zbeg) << PAGE_SHIFT);
		}
		lkpi_vma_ref(m->key, -1);
	}
}

/* zap_vma_ptes(): the same for one VMA (i915 GGTT mmaps). */
void zap_vma_ptes(struct vm_area_struct *vma, unsigned long address, unsigned long size)
{
	struct lkpi_vma *m = vma->lkpi;
	unsigned long beg = (address - vma->vm_start) >> PAGE_SHIFT, n = size >> PAGE_SHIFT;
	mutex_lock(&m->lock);
	m->remapped = false;
	for (unsigned long i = beg; i < beg + n && i < m->npages; i++)
		m->pages[i] = 0;
	mutex_unlock(&m->lock);
	lkpi_zap_user_range(m->key + (beg << PAGE_SHIFT), size);
}

/* i915 (remap_io_mapping / remap_io_sg): pages [addr, addr + n pages) of a
 * VMA map pfns pfn.. with the given cache type. */
int lkpi_vma_insert_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn,
			  unsigned long npages, pgprot_t prot)
{
	struct lkpi_vma *m = vma->lkpi;
	unsigned long pg = (addr - vma->vm_start) >> PAGE_SHIFT;
	if (addr < vma->vm_start)
		return -EINVAL;
	for (unsigned long i = 0; i < npages && pg + i < m->npages; i++)
		m->pages[pg + i] = PFN_PHYS(pfn + i) | LKPI_PTE_VALID | prot_bits(prot);
	return 0;
}

/* get_file_active(): a reference to *f unless its last one is going away. */
struct file *lkpi_get_file_active(struct file **f)
{
	struct file *file = READ_ONCE(*f);
	long c;
	if (!file)
		return NULL;
	c = atomic_long_read(&file->f_count);
	do {
		if (!c)
			return NULL;
	} while (!__atomic_compare_exchange_n(&file->f_count.counter, &c, c + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
	return file;
}

/* ---- binder's view of a process's mappings ---- */
/* The VMA of mm containing addr (a real user address). */
struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *found = NULL;
	spin_lock(&lkpi_vmas_lock);
	for (int id = 0; id < LKPI_MAX_VMAS && !found; id++) {
		struct lkpi_vma *m = lkpi_vmas[id];
		if (m && m->vma.vm_mm == mm && addr >= m->vma.vm_start && addr < m->vma.vm_end)
			found = &m->vma;
	}
	spin_unlock(&lkpi_vmas_lock);
	return found;
}

int vm_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page)
{
	struct lkpi_vma *m = vma->lkpi;
	int r;
	mutex_lock(&m->lock);
	r = lkpi_vma_insert_range(vma, addr, page_to_pfn(page), 1, vma->vm_page_prot);
	mutex_unlock(&m->lock);
	return r;
}

void zap_page_range_single(struct vm_area_struct *vma, unsigned long address, unsigned long size, void *details)
{
	zap_vma_ptes(vma, address, size);
}

/* The size of a Linux file's inode (a dma-buf's buffer size): fstat and
 * lseek(SEEK_END) report it, as on Linux - Mesa sizes imported dma-bufs so. */
long long lkpi_file_size(struct file *f)
{
	return f && f->f_inode ? f->f_inode->i_size : 0;
}
