/* LinuxKPI: Linux wait queue entries on top of the Syluxium wait queue
 * (core.cpp). wake_up() first wakes the wait_event() sleepers (a counter
 * plus a Syluxium WaitQueue), then runs the entries' callbacks: dma-fence
 * and i915_sw_fence chains, prepare_to_wait()/schedule() sleepers. */
#include <linux/lkpi_all.h>
#include <linux/wait.h>
#include <linux/sched.h>

void lkpi_wq_head_ready(struct wait_queue_head *wq)
{
	if (!wq->head.next)
		INIT_LIST_HEAD(&wq->head);
}

/* Called by wake_up() (core.cpp) with the lock not held, or with it held
 * (the _locked variants). Returns the number of exclusive waiters woken. */
static void wake_entries(struct wait_queue_head *wq, unsigned int mode, int nr_exclusive, void *key)
{
	struct wait_queue_entry *e, *next;
	if (!wq->head.next)
		return;
	list_for_each_entry_safe(e, next, &wq->head, entry) {
		unsigned flags = e->flags;
		int ret = e->func(e, mode, 0, key);
		if (ret < 0)
			break;
		if (ret && (flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive)
			break;
	}
}

void lkpi_wake_entries(struct wait_queue_head *wq, unsigned int mode, int nr, void *key)
{
	unsigned long flags;
	if (!READ_ONCE(wq->head.next))
		return;
	spin_lock_irqsave(&wq->lock, flags);
	wake_entries(wq, mode, nr, key);
	spin_unlock_irqrestore(&wq->lock, flags);
}

void lkpi_wake_up_seq(struct wait_queue_head *wq); /* core.cpp: wait_event() sleepers */

void __wake_up(struct wait_queue_head *wq, unsigned int mode, int nr, void *key)
{
	lkpi_wake_up_seq(wq);
	lkpi_wake_entries(wq, mode, nr, key);
}

void __wake_up_locked(struct wait_queue_head *wq, unsigned int mode, int nr)
{
	lkpi_wake_up_seq(wq);
	wake_entries(wq, mode, nr, NULL);
}

void __wake_up_locked_key(struct wait_queue_head *wq, unsigned int mode, void *key)
{
	lkpi_wake_up_seq(wq);
	wake_entries(wq, mode, 1, key);
}

int default_wake_function(struct wait_queue_entry *e, unsigned mode, int flags, void *key)
{
	return wake_up_state(e->private, mode);
}

int autoremove_wake_function(struct wait_queue_entry *e, unsigned mode, int sync, void *key)
{
	int ret = default_wake_function(e, mode, sync, key);
	if (ret)
		list_del_init(&e->entry);
	return ret;
}

int woken_wake_function(struct wait_queue_entry *e, unsigned mode, int sync, void *key)
{
	smp_mb();
	e->flags |= WQ_FLAG_WOKEN;
	return default_wake_function(e, mode, sync, key);
}

void add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	unsigned long flags;
	e->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq->lock, flags);
	__add_wait_queue(wq, e);
	spin_unlock_irqrestore(&wq->lock, flags);
}

void add_wait_queue_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	unsigned long flags;
	e->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq->lock, flags);
	__add_wait_queue_entry_tail(wq, e);
	spin_unlock_irqrestore(&wq->lock, flags);
}

void add_wait_queue_priority(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	add_wait_queue(wq, e);
}

void remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	unsigned long flags;
	spin_lock_irqsave(&wq->lock, flags);
	list_del(&e->entry);
	spin_unlock_irqrestore(&wq->lock, flags);
}

void prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e, int state)
{
	unsigned long flags;
	e->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq->lock, flags);
	if (!e->entry.next || list_empty(&e->entry))
		__add_wait_queue(wq, e);
	set_current_state(state);
	spin_unlock_irqrestore(&wq->lock, flags);
}

bool prepare_to_wait_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e, int state)
{
	unsigned long flags;
	bool was_empty = false;
	e->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq->lock, flags);
	if (!e->entry.next || list_empty(&e->entry)) {
		lkpi_wq_head_ready(wq);
		was_empty = list_empty(&wq->head);
		__add_wait_queue_entry_tail(wq, e);
	}
	set_current_state(state);
	spin_unlock_irqrestore(&wq->lock, flags);
	return was_empty;
}

long prepare_to_wait_event(struct wait_queue_head *wq, struct wait_queue_entry *e, int state)
{
	unsigned long flags;
	long ret = 0;
	spin_lock_irqsave(&wq->lock, flags);
	if (signal_pending_state(state, current)) {
		if (e->entry.next)
			list_del_init(&e->entry);
		ret = -ERESTARTSYS;
	} else {
		if (!e->entry.next || list_empty(&e->entry)) {
			if (e->flags & WQ_FLAG_EXCLUSIVE)
				__add_wait_queue_entry_tail(wq, e);
			else
				__add_wait_queue(wq, e);
		}
		set_current_state(state);
	}
	spin_unlock_irqrestore(&wq->lock, flags);
	return ret;
}

void finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	unsigned long flags;
	__set_current_state(TASK_RUNNING);
	if (e->entry.next && !list_empty(&e->entry)) {
		spin_lock_irqsave(&wq->lock, flags);
		list_del_init(&e->entry);
		spin_unlock_irqrestore(&wq->lock, flags);
	}
}

long wait_woken(struct wait_queue_entry *e, unsigned mode, long timeout)
{
	set_current_state(mode);
	if (!(e->flags & WQ_FLAG_WOKEN) && !signal_pending_state(mode, current))
		timeout = schedule_timeout(timeout);
	__set_current_state(TASK_RUNNING);
	e->flags &= ~WQ_FLAG_WOKEN;
	return timeout;
}

/* ---- tasklets ---- */
static void tasklet_work(struct work_struct *w)
{
	struct tasklet_struct *t = container_of(w, struct tasklet_struct, work);
	if (!tasklet_trylock(t)) { /* still running (called directly elsewhere): come back */
		queue_work(system_highpri_wq, &t->work);
		return;
	}
	if (atomic_read(&t->count)) { /* disabled: tasklet_enable() queues it again */
		tasklet_unlock(t);
		return;
	}
	if (test_and_clear_bit(TASKLET_STATE_SCHED, &t->state)) {
		if (t->callback)
			t->callback(t);
		else if (t->func)
			t->func(t->data);
	}
	tasklet_unlock(t);
}

void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data)
{
	INIT_WORK(&t->work, tasklet_work);
	t->func = func;
	t->callback = NULL;
	t->data = data;
	t->state = 0;
	atomic_set(&t->count, 0);
}

void tasklet_setup(struct tasklet_struct *t, void (*cb)(struct tasklet_struct *))
{
	tasklet_init(t, NULL, 0);
	t->callback = cb;
	t->use_callback = true;
}

void tasklet_schedule(struct tasklet_struct *t)
{
	if (!t->work.func) /* DECLARE_TASKLET: set up on first use */
		INIT_WORK(&t->work, tasklet_work);
	if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
		queue_work(system_highpri_wq, &t->work);
}

bool tasklet_trylock(struct tasklet_struct *t)
{
	return !test_and_set_bit(TASKLET_STATE_RUN, &t->state);
}

void tasklet_unlock(struct tasklet_struct *t)
{
	smp_mb__before_atomic();
	clear_bit(TASKLET_STATE_RUN, &t->state);
}

void tasklet_unlock_wait(struct tasklet_struct *t)
{
	while (test_bit(TASKLET_STATE_RUN, &t->state))
		cpu_relax();
}

void tasklet_disable_nosync(struct tasklet_struct *t)
{
	atomic_inc(&t->count);
	smp_mb__after_atomic();
}

void tasklet_disable(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_wait(t);
	smp_mb();
}

void tasklet_enable(struct tasklet_struct *t)
{
	smp_mb__before_atomic();
	if (atomic_dec_and_test(&t->count) && test_bit(TASKLET_STATE_SCHED, &t->state))
		queue_work(system_highpri_wq, &t->work);
}

void tasklet_kill(struct tasklet_struct *t)
{
	cancel_work_sync(&t->work);
	tasklet_unlock_wait(t);
	clear_bit(TASKLET_STATE_SCHED, &t->state);
}
