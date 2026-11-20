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
