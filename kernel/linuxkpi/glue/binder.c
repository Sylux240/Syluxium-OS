/* LinuxKPI glue for the binder driver (drivers/android): credentials, task
 * references, the per-process mm, misc devices, list_lru, task_work.
 * The Syluxium side (processes, descriptors, binderfs) is binder.cpp. */
#include <linux/lkpi_all.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/lkpi_binder.h>

struct user_namespace init_user_ns;

/* binder.cpp */
void lkpi_current_ids(unsigned int ids[8]); /* ruid euid suid fsuid rgid egid sgid fsgid */
unsigned int lkpi_task_euid(struct task_struct *t);
void lkpi_task_free(struct task_struct *t);
int lkpi_chrdev_node(const char *path, unsigned int major, unsigned int minor, unsigned short perms);

/* ---- credentials ---- */
const struct cred *lkpi_current_cred(void)
{
	unsigned int ids[8];
	struct cred *c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return NULL;
	lkpi_current_ids(ids);
	refcount_set(&c->usage, 1);
	c->uid = KUIDT_INIT(ids[0]);
	c->euid = KUIDT_INIT(ids[1]);
	c->suid = KUIDT_INIT(ids[2]);
	c->fsuid = KUIDT_INIT(ids[3]);
	c->gid = KGIDT_INIT(ids[4]);
	c->egid = KGIDT_INIT(ids[5]);
	c->sgid = KGIDT_INIT(ids[6]);
	c->fsgid = KGIDT_INIT(ids[7]);
	return c;
}

const struct cred *get_cred(const struct cred *c)
{
	if (c)
		refcount_inc(&((struct cred *)c)->usage);
	return c;
}

void put_cred(const struct cred *c)
{
	if (c && refcount_dec_and_test(&((struct cred *)c)->usage))
		kfree(c);
}

kuid_t lkpi_current_euid(void)
{
	unsigned int ids[8];
	lkpi_current_ids(ids);
	return KUIDT_INIT(ids[1]);
}

/* ---- tasks: per-thread tasks live as long as their thread; the
 * per-process one (group_leader) as long as its last reference. ---- */
struct task_struct *get_task_struct(struct task_struct *t)
{
	__atomic_add_fetch(&t->lkpi_usage, 1, __ATOMIC_RELAXED);
	return t;
}

void put_task_struct(struct task_struct *t)
{
	if (__atomic_sub_fetch(&t->lkpi_usage, 1, __ATOMIC_ACQ_REL) == 0)
		lkpi_task_free(t);
}

kuid_t task_euid(struct task_struct *t) { return KUIDT_INIT(lkpi_task_euid(t)); }

struct lkpi_task_work {
	struct work_struct work;
	struct callback_head *twork;
};

static void task_work_fn(struct work_struct *w)
{
	struct lkpi_task_work *tw = container_of(w, struct lkpi_task_work, work);
	tw->twork->func(tw->twork);
	kfree(tw);
}

int task_work_add(struct task_struct *task, struct callback_head *twork, enum task_work_notify_mode mode)
{
	struct lkpi_task_work *tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
	if (!tw)
		return -ENOMEM;
	tw->twork = twork;
	INIT_WORK(&tw->work, task_work_fn);
	schedule_work(&tw->work);
	return 0;
}

/* ---- mm ---- */
void mmgrab(struct mm_struct *mm) { atomic_inc(&mm->mm_count); }
void mmdrop(struct mm_struct *mm)
{
	if (atomic_dec_and_test(&mm->mm_count))
		kfree(mm);
}
bool mmget_not_zero(struct mm_struct *mm) { return atomic_inc_not_zero(&mm->mm_users); }
void mmput(struct mm_struct *mm) { atomic_dec(&mm->mm_users); }

void memset_page(struct page *page, size_t offset, int val, size_t len)
{
	memset((char *)page_address(page) + offset, val, len);
}

/* ---- misc devices: major 10, one file_operations per minor ---- */
static LIST_HEAD(misc_list);
static DEFINE_MUTEX(misc_mtx);
static unsigned long misc_minors[4]; /* dynamic minors 64..319 */

static int misc_open(struct inode *inode, struct file *file)
{
	int minor = inode->i_rdev & 0xFFFFF;
	struct miscdevice *c, *found = NULL;
	mutex_lock(&misc_mtx);
	list_for_each_entry(c, &misc_list, list)
		if (c->minor == minor) {
			found = c;
			break;
		}
	mutex_unlock(&misc_mtx);
	if (!found || !found->fops)
		return -ENODEV;
	file->private_data = found;
	file->f_op = found->fops;
	return file->f_op->open ? file->f_op->open(inode, file) : 0;
}

static const struct file_operations misc_fops = { .open = misc_open };

int misc_register(struct miscdevice *misc)
{
	static bool registered;
	char path[96];
	mutex_lock(&misc_mtx);
	if (!registered) {
		register_chrdev(MISC_MAJOR, "misc", &misc_fops);
		registered = true;
	}
	if (misc->minor == MISC_DYNAMIC_MINOR) {
		int i;
		for (i = 0; i < 256 && test_bit(i, misc_minors); i++)
			;
		if (i == 256) {
			mutex_unlock(&misc_mtx);
			return -EBUSY;
		}
		set_bit(i, misc_minors);
		misc->minor = 64 + i;
	}
	INIT_LIST_HEAD(&misc->list);
	list_add(&misc->list, &misc_list);
	mutex_unlock(&misc_mtx);
	/* binderfs devices (binder.cpp) have no /dev node of their own */
	if (misc->name && !(misc->nodename && !misc->nodename[0])) {
		snprintf(path, sizeof(path), "/dev/%s", misc->nodename ? misc->nodename : misc->name);
		lkpi_chrdev_node(path, MISC_MAJOR, misc->minor, misc->mode ? misc->mode : 0600);
	}
	return 0;
}

void misc_deregister(struct miscdevice *misc)
{
	mutex_lock(&misc_mtx);
	list_del(&misc->list);
	if (misc->minor >= 64 && misc->minor < 320)
		clear_bit(misc->minor - 64, misc_minors);
	mutex_unlock(&misc_mtx);
}

/* ---- list_lru ---- */
int list_lru_init(struct list_lru *lru)
{
	spin_lock_init(&lru->lock);
	INIT_LIST_HEAD(&lru->one.list);
	lru->one.nr_items = 0;
	return 0;
}
void list_lru_destroy(struct list_lru *lru) { }

bool list_lru_add_obj(struct list_lru *lru, struct list_head *item)
{
	bool added = false;
	spin_lock(&lru->lock);
	if (list_empty(item)) {
		list_add_tail(item, &lru->one.list);
		lru->one.nr_items++;
		added = true;
	}
	spin_unlock(&lru->lock);
	return added;
}

bool list_lru_del_obj(struct list_lru *lru, struct list_head *item)
{
	bool removed = false;
	spin_lock(&lru->lock);
	if (!list_empty(item)) {
		list_del_init(item);
		lru->one.nr_items--;
		removed = true;
	}
	spin_unlock(&lru->lock);
	return removed;
}

unsigned long list_lru_count(struct list_lru *lru) { return lru->one.nr_items; }

void list_lru_isolate(struct list_lru_one *list, struct list_head *item)
{
	list_del_init(item);
	list->nr_items--;
}

unsigned long list_lru_walk(struct list_lru *lru, list_lru_walk_cb isolate, void *cb_arg, unsigned long nr_to_walk)
{
	struct list_head *item, *n;
	unsigned long isolated = 0;
	spin_lock(&lru->lock);
restart:
	list_for_each_safe(item, n, &lru->one.list) {
		enum lru_status ret;
		if (!nr_to_walk--)
			break;
		ret = isolate(item, &lru->one, &lru->lock, cb_arg);
		if (ret == LRU_REMOVED_RETRY) {
			isolated++;
			spin_lock(&lru->lock); /* the callback dropped it */
			goto restart;
		}
		if (ret == LRU_REMOVED)
			isolated++;
		else if (ret == LRU_RETRY)
			goto restart;
		else if (ret == LRU_STOP)
			break;
	}
	spin_unlock(&lru->lock);
	return isolated;
}

void wake_up_pollfree(struct wait_queue_head *wq) { wake_up_poll(wq, 0x10 /* EPOLLHUP */ | 0x4000 /* POLLFREE */); }

void *lkpi_file_native(struct file *f) { return f->lkpi_native; }

bool lkpi_file_wants_address(struct file *f) { return f->f_op && f->f_op->lkpi_user_address; }
