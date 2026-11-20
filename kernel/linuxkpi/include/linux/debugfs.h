/* LinuxKPI: debugfs is not available; everything is a no-op (as with
 * CONFIG_DEBUG_FS=n). */
#ifndef _LKPI_LINUX_DEBUGFS_H
#define _LKPI_LINUX_DEBUGFS_H
#include <linux/lkpi_all.h>
#include <linux/fs.h>
struct file;
struct inode;
struct seq_file { void* private; };
static inline void seq_printf(struct seq_file* m, const char* fmt, ...) {}
static inline void seq_puts(struct seq_file* m, const char* s) {}
static inline void seq_putc(struct seq_file* m, char c) {}
static inline int seq_write(struct seq_file* m, const void* data, size_t len) { return 0; }
static inline int single_open(struct file* f, int (*show)(struct seq_file*, void*), void* data) { return -ENODEV; }
#define single_release NULL
#define seq_read NULL
#define seq_lseek NULL
struct poll_table_struct;
typedef long long loff_t_lkpi;
struct debugfs_regset32;
struct debugfs_blob_wrapper { void* data; unsigned long size; };
#define DEFINE_DEBUGFS_ATTRIBUTE(__fops, __get, __set, __fmt) \
    static const struct file_operations __fops __attribute__((unused)) = {}; \
    static void* __fops##_lkpi_refs[] __attribute__((unused)) = { (void*)(__get), (void*)(__set) }
#define DEFINE_SIMPLE_ATTRIBUTE DEFINE_DEBUGFS_ATTRIBUTE
#define DEFINE_SHOW_ATTRIBUTE(__name) static const struct file_operations __name##_fops __attribute__((unused)) = {}
#define DEFINE_SHOW_STORE_ATTRIBUTE(__name) static const struct file_operations __name##_fops __attribute__((unused)) = {}
static inline struct dentry* debugfs_create_dir(const char* name, struct dentry* parent) { return (struct dentry*)ERR_PTR(-ENODEV); }
static inline struct dentry* debugfs_create_file(const char* name, unsigned short mode, struct dentry* parent, void* data, const struct file_operations* fops) { return (struct dentry*)ERR_PTR(-ENODEV); }
#define debugfs_create_file_unsafe debugfs_create_file
static inline struct dentry* debugfs_create_symlink(const char* name, struct dentry* parent, const char* dest) { return (struct dentry*)ERR_PTR(-ENODEV); }
static inline struct dentry* debugfs_rename(struct dentry* old_dir, struct dentry* old_dentry, struct dentry* new_dir, const char* new_name) { return (struct dentry*)ERR_PTR(-ENODEV); }
static inline void debugfs_remove(struct dentry* dentry) {}
#define debugfs_remove_recursive debugfs_remove
static inline struct dentry* debugfs_lookup(const char* name, struct dentry* parent) { return NULL; }
static inline void debugfs_lookup_and_remove(const char* name, struct dentry* parent) {}
#define debugfs_create_u8(n, m, p, v) do { } while (0)
#define debugfs_create_u16(n, m, p, v) do { } while (0)
#define debugfs_create_u32(n, m, p, v) do { } while (0)
#define debugfs_create_u64(n, m, p, v) do { } while (0)
#define debugfs_create_x8(n, m, p, v) do { } while (0)
#define debugfs_create_x32(n, m, p, v) do { } while (0)
#define debugfs_create_bool(n, m, p, v) do { } while (0)
#define debugfs_create_ulong(n, m, p, v) do { } while (0)
#define debugfs_create_atomic_t(n, m, p, v) do { } while (0)
#define debugfs_create_blob(n, m, p, b) ((struct dentry*)ERR_PTR(-ENODEV))
#define debugfs_create_devm_seqfile(d, n, p, f) do { } while (0)
static inline bool debugfs_initialized(void) { return false; }
static inline ssize_t simple_read_from_buffer(void __user* to, size_t count, loff_t* ppos, const void* from, size_t available) { return 0; }
static inline ssize_t simple_write_to_buffer(void* to, size_t available, loff_t* ppos, const void __user* from, size_t count) { return 0; }
static inline int simple_open(struct inode* inode, struct file* file) { return 0; }
#define default_llseek NULL
#define no_llseek NULL
#define noop_llseek NULL
#define generic_file_llseek NULL
#endif
