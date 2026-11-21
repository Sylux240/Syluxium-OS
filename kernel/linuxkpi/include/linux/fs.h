/* LinuxKPI: files. A Linux `struct file` (with its file_operations) is
 * bridged to a Syluxium inode/file by file.cpp: system calls on the
 * descriptor go to f_op->read/write/unlocked_ioctl/poll/mmap/release. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/uaccess.h>

struct file;
struct inode;
struct vm_area_struct;
struct poll_table_struct;
struct address_space_operations;
struct page;
struct folio;
struct address_space { struct inode *host; void *lkpi; const struct address_space_operations *a_ops; };
enum writeback_sync_modes { WB_SYNC_NONE, WB_SYNC_ALL };
struct writeback_control { long nr_to_write; enum writeback_sync_modes sync_mode; unsigned for_reclaim:1; loff_t range_start, range_end; };
struct address_space_operations {
	int (*writepage)(struct page *page, struct writeback_control *wbc);
	int (*read_folio)(struct file *, struct folio *);
	int (*writepages)(struct address_space *, struct writeback_control *);
	int (*write_begin)(struct file *, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata);
	int (*write_end)(struct file *, struct address_space *mapping, loff_t pos, unsigned len, unsigned copied, struct folio *folio, void *fsdata);
};
#define SB_KERNMOUNT (1 << 22)

struct qstr { const char *name; unsigned int len; };
struct dentry {
	struct dentry *d_parent;
	struct qstr d_name;
	struct inode *d_inode;
	void *d_fsdata;
};
struct path { struct dentry *dentry; void *mnt; };

struct inode {
	umode_t i_mode;
	dev_t i_rdev;
	unsigned long i_ino;
	loff_t i_size;
	void *i_private;
	struct address_space i_data;
	struct address_space *i_mapping;
	struct cdev *i_cdev;
};

struct file_operations {
	struct module *owner;
	loff_t (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
	unsigned int (*poll)(struct file *, struct poll_table_struct *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	int (*mmap)(struct file *, struct vm_area_struct *);
	int (*open)(struct inode *, struct file *);
	int (*flush)(struct file *, void *);
	int (*release)(struct inode *, struct file *);
	int (*fasync)(int, struct file *, int);
	void (*show_fdinfo)(struct seq_file *, struct file *);
	unsigned long fop_flags;
	bool lkpi_user_address; /* LinuxKPI: mmap() sees the real user address in vm_start (binder) */
};
#define FOP_UNSIGNED_OFFSET 0x1

struct file {
	const struct file_operations *f_op;
	void *private_data;
	unsigned int f_flags;
	fmode_t f_mode;
	loff_t f_pos;
	struct inode *f_inode;
	struct address_space *f_mapping;
	struct path f_path;
	atomic_long_t f_count;
	void *lkpi_vfs; /* the Syluxium vfs::File */
	const struct cred *f_cred; /* the opener's ids */
	void *lkpi_native; /* a Syluxium file carried through Linux code (binder fd passing): its vfs::File */
};
struct file *lkpi_file_alloc(const struct file_operations *fops, void *priv, unsigned int flags, struct inode *inode);
#define file_inode(f) ((f)->f_inode)
#define file_count(f) atomic_long_read(&(f)->f_count)
#define iput(inode) do { } while (0)
#define inode_set_bytes(inode, n) do { } while (0)
#define FMODE_READ 0x1
#define FMODE_WRITE 0x2
#define FMODE_LSEEK 0x4
#define FMODE_PREAD 0x8
#define FMODE_PWRITE 0x10
#define FMODE_UNSIGNED_OFFSET 0x2000

#ifndef O_ACCMODE
#define O_ACCMODE 00000003
#define O_RDONLY 00000000
#define O_WRONLY 00000001
#define O_RDWR 00000002
#define O_NONBLOCK 00004000
#define O_CLOEXEC 02000000
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

struct file *get_file(struct file *f);
void fput(struct file *f);
struct file *fget(unsigned int fd);
int get_unused_fd_flags(unsigned int flags);
void put_unused_fd(unsigned int fd);
void fd_install(unsigned int fd, struct file *f);
loff_t noop_llseek(struct file *f, loff_t offset, int whence);
loff_t no_llseek(struct file *f, loff_t offset, int whence);
loff_t default_llseek(struct file *f, loff_t offset, int whence);
int nonseekable_open(struct inode *inode, struct file *f);
int stream_open(struct inode *inode, struct file *f);
#define compat_ptr_ioctl NULL
#define iminor(inode) ((unsigned int)((inode)->i_rdev & 0xFFFFF))
#define imajor(inode) ((unsigned int)((inode)->i_rdev >> 20))

/* Character devices (DRM's /dev/dri/cardN): register_chrdev(major, name, fops). */
int register_chrdev(unsigned int major, const char *name, const struct file_operations *fops);
void unregister_chrdev(unsigned int major, const char *name);
/* Make /dev/<path> a node for (major, minor). */
int lkpi_chrdev_node(const char *path, unsigned int major, unsigned int minor, umode_t mode);
void lkpi_chrdev_node_remove(const char *path);

/* Pseudo file systems (dma-buf's "dmabuf" fs): only the bits drivers touch. */
struct fs_context;
struct super_block { void *s_fs_info; };
struct file_system_type { const char *name; int (*init_fs_context)(struct fs_context *); void (*kill_sb)(struct super_block *); struct module *owner; };
struct dentry_operations { char *(*d_dname)(struct dentry *, char *, int); void (*d_release)(struct dentry *); };
#define kill_anon_super NULL
struct vfsmount;
struct inode *alloc_anon_inode(struct super_block *sb);
struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt, const char *name, int flags,
			       const struct file_operations *fops);
char *dynamic_dname(char *buf, int buflen, const char *fmt, ...);
char *strndup_user(const char __user *s, long n);
#define simple_dname NULL

/* Descriptor helpers. */
struct fd { struct file *file; };
static inline struct fd fdget(unsigned int fd) { struct fd f = { fget(fd) }; return f; }
static inline void fdput(struct fd f) { if (f.file) fput(f.file); }
#define fd_file(f) ((f).file)
