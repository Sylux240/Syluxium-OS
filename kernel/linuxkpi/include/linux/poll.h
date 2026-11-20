/* LinuxKPI: poll. A driver's f_op->poll calls poll_wait() on its wait queues
 * and returns the ready mask; Syluxium re-polls when a LinuxKPI wait queue
 * wakes (wake_up() notifies the poll subsystem). */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/fs.h>
#include <linux/wait.h>

typedef struct poll_table_struct { int unused; } poll_table;
static inline void poll_wait(struct file *f, wait_queue_head_t *wq, poll_table *p) { (void)f; (void)wq; (void)p; }
#define wake_up_locked_poll(wq, events) wake_up(wq)
#define wake_up_interruptible_poll(wq, events) wake_up(wq)
#define poll_requested_events(pt) (~0u)
#define EPOLLIN 0x00000001
#define EPOLLPRI 0x00000002
#define EPOLLOUT 0x00000004
#define EPOLLERR 0x00000008
#define EPOLLHUP 0x00000010
#define EPOLLRDNORM 0x00000040
#define EPOLLRDBAND 0x00000080
#define EPOLLWRNORM 0x00000100
#define EPOLLRDHUP 0x00002000
#define POLLIN EPOLLIN
#define POLLOUT EPOLLOUT
#define POLLERR EPOLLERR
#define POLLHUP EPOLLHUP
#define POLLRDNORM EPOLLRDNORM
#define POLLPRI EPOLLPRI
