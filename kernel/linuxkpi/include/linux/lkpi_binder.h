/* LinuxKPI: what the binder driver (drivers/android) needs beyond the common
 * headers: user ids and credentials, a per-process mm, task references,
 * misc devices, list_lru, task_work, security hooks (no LSM: every check
 * passes and the security context is fixed). glue/binder.c, binder.cpp. */
#pragma once
struct page;
#define CONFIG_ANDROID_BINDER_IPC 1
#define CONFIG_ANDROID_BINDER_DEVICES "binder,hwbinder,vndbinder"
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/refcount.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/rcupdate.h>

/* ---- user ids ---- */
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef struct { uid_t val; } kuid_t;
typedef struct { gid_t val; } kgid_t;
struct user_namespace { int unused; };
extern struct user_namespace init_user_ns;
#define current_user_ns() (&init_user_ns)
#define KUIDT_INIT(v) ((kuid_t){ (uid_t)(v) })
#define KGIDT_INIT(v) ((kgid_t){ (gid_t)(v) })
#define INVALID_UID KUIDT_INIT(-1)
#define INVALID_GID KGIDT_INIT(-1)
#define GLOBAL_ROOT_UID KUIDT_INIT(0)
#define GLOBAL_ROOT_GID KGIDT_INIT(0)
static inline uid_t from_kuid(struct user_namespace *ns, kuid_t k) { return k.val; }
static inline gid_t from_kgid(struct user_namespace *ns, kgid_t k) { return k.val; }
static inline kuid_t make_kuid(struct user_namespace *ns, uid_t u) { return KUIDT_INIT(u); }
static inline kgid_t make_kgid(struct user_namespace *ns, gid_t g) { return KGIDT_INIT(g); }
static inline bool uid_eq(kuid_t a, kuid_t b) { return a.val == b.val; }
static inline bool gid_eq(kgid_t a, kgid_t b) { return a.val == b.val; }
static inline bool uid_valid(kuid_t k) { return k.val != (uid_t)-1; }
static inline bool gid_valid(kgid_t k) { return k.val != (gid_t)-1; }

/* ---- credentials: a snapshot of a process's ids, reference counted ---- */
struct cred {
	refcount_t usage;
	kuid_t uid, euid, suid, fsuid;
	kgid_t gid, egid, sgid, fsgid;
};
const struct cred *lkpi_current_cred(void); /* a new reference */
const struct cred *get_cred(const struct cred *c);
void put_cred(const struct cred *c);
kuid_t lkpi_current_euid(void);
#define current_euid() lkpi_current_euid()
#define current_uid() lkpi_current_euid()

/* ---- tasks ---- */
struct pid_namespace { int unused; };
#define task_active_pid_ns(t) ((struct pid_namespace *)NULL)
#define task_tgid_nr_ns(t, ns) ((t)->tgid)
#ifndef task_pid_nr
#define task_pid_nr(t) ((t)->pid)
#endif
#ifndef task_tgid_nr
#define task_tgid_nr(t) ((t)->tgid)
#endif
struct task_struct *get_task_struct(struct task_struct *t);
void put_task_struct(struct task_struct *t);
kuid_t task_euid(struct task_struct *t);
#define MAX_NICE 19
#define MIN_NICE -20
#define RLIMIT_NICE 13
#define task_nice(t) 0
#define set_user_nice(t, n) do { (void)(t); (void)(n); } while (0)
#define can_nice(t, n) true
#define rlimit(r) 0UL
#define rlimit_to_nice(r) (MAX_NICE - (long)(r) + 1)
#ifndef TASK_FREEZABLE
#define TASK_FREEZABLE 0
#endif
#define freezing(t) false
#define try_to_freeze() false

/* task_work: binder closes descriptors after its ioctl returns; here a work
 * item runs the callback (outside the ioctl, as Linux's return-to-user does). */
enum task_work_notify_mode { TWA_NONE, TWA_RESUME, TWA_SIGNAL, TWA_SIGNAL_NO_IPI, TWA_NMI_CURRENT };
static inline void init_task_work(struct callback_head *twork, void (*func)(struct callback_head *)) { twork->func = func; }
int task_work_add(struct task_struct *task, struct callback_head *twork, enum task_work_notify_mode mode);

/* ---- a process's address space as binder sees it (binder.cpp) ---- */
struct mm_struct {
	atomic_t mm_count; /* the structure */
	atomic_t mm_users; /* the address space is alive (the process runs) */
	void *lkpi_process;
};
struct mm_struct *lkpi_current_mm(void);
void mmgrab(struct mm_struct *mm);
void mmdrop(struct mm_struct *mm);
bool mmget_not_zero(struct mm_struct *mm);
void mmput(struct mm_struct *mm);
#define mmput_async(mm) mmput(mm)
struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr);
int vm_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page);
void zap_page_range_single(struct vm_area_struct *vma, unsigned long address, unsigned long size, void *details);
#ifndef PAGE_ALIGNED
#define PAGE_ALIGNED(addr) (((unsigned long)(addr) & (PAGE_SIZE - 1)) == 0)
#endif
void memset_page(struct page *page, size_t offset, int val, size_t len);

/* ---- files ---- */
struct files_struct;
struct file *file_close_fd(unsigned int fd);
struct file *lkpi_fget_native(unsigned int fd); /* any descriptor, wrapped if it is no Linux file */
#define filp_close(f, files) ({ (void)(f); 0; })

/* ---- misc devices (character major 10) ---- */
#define MISC_MAJOR 10
#define MISC_DYNAMIC_MINOR 255
struct miscdevice {
	int minor;
	const char *name;
	const struct file_operations *fops;
	struct list_head list;
	struct device *parent;
	struct device *this_device;
	const void *groups;
	const char *nodename;
	unsigned short mode;
};
int misc_register(struct miscdevice *misc);
void misc_deregister(struct miscdevice *misc);

/* ---- list_lru (binder's reclaimable pages; nothing reclaims them here) ---- */
struct list_lru_one { struct list_head list; long nr_items; };
struct list_lru { spinlock_t lock; struct list_lru_one one; };
enum lru_status { LRU_REMOVED, LRU_REMOVED_RETRY, LRU_ROTATE, LRU_SKIP, LRU_RETRY, LRU_STOP };
typedef enum lru_status (*list_lru_walk_cb)(struct list_head *item, struct list_lru_one *list, spinlock_t *lock, void *cb_arg);
int list_lru_init(struct list_lru *lru);
void list_lru_destroy(struct list_lru *lru);
bool list_lru_add_obj(struct list_lru *lru, struct list_head *item);
bool list_lru_del_obj(struct list_lru *lru, struct list_head *item);
unsigned long list_lru_count(struct list_lru *lru);
unsigned long list_lru_walk(struct list_lru *lru, list_lru_walk_cb isolate, void *cb_arg, unsigned long nr_to_walk);
void list_lru_isolate(struct list_lru_one *list, struct list_head *item);

/* ---- security (no LSM) ---- */
static inline int security_binder_set_context_mgr(const struct cred *mgr) { return 0; }
static inline int security_binder_transaction(const struct cred *from, const struct cred *to) { return 0; }
static inline int security_binder_transfer_binder(const struct cred *from, const struct cred *to) { return 0; }
static inline int security_binder_transfer_file(const struct cred *from, const struct cred *to, const struct file *file) { return 0; }
static inline void security_cred_getsecid(const struct cred *c, u32 *secid) { *secid = 1; }
/* Android's servicemanager asks for the caller's context with every call
 * (FLAT_BINDER_FLAG_TXN_SECURITY_CTX); binder fails the call without one. */
#define LKPI_BINDER_SECCTX "u:r:su:s0"
static inline int security_secid_to_secctx(u32 secid, char **secdata, u32 *seclen)
{
	*secdata = (char *)LKPI_BINDER_SECCTX;
	*seclen = sizeof(LKPI_BINDER_SECCTX);
	return 0;
}
static inline void security_release_secctx(char *secdata, u32 seclen) { }

/* ---- small things ---- */
#define wake_up_interruptible_sync(wq) wake_up_interruptible(wq)
void wake_up_pollfree(struct wait_queue_head *wq);
static inline size_t hlist_count_nodes(struct hlist_head *head)
{
	struct hlist_node *pos;
	size_t count = 0;
	for (pos = head->first; pos; pos = pos->next)
		count++;
	return count;
}
#ifndef module_param_call
#define module_param_call(name, set, get, arg, perm)
#endif
