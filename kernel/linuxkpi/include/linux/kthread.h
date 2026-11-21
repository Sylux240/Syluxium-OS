/* LinuxKPI: kernel threads (Syluxium kernel threads underneath). */
#ifndef _LKPI_LINUX_KTHREAD_H
#define _LKPI_LINUX_KTHREAD_H
#include <linux/lkpi_all.h>
LKPI_EXTERN_C_BEGIN
struct task_struct* lkpi_kthread_run(int (*fn)(void* data), void* data, const char* name);
#define kthread_run(fn, data, namefmt, ...) lkpi_kthread_run(fn, data, namefmt)
bool kthread_should_stop(void);
bool kthread_should_park(void);
void kthread_parkme(void);
int kthread_park(struct task_struct* t);
void kthread_unpark(struct task_struct* t);
int kthread_stop(struct task_struct* t);
LKPI_EXTERN_C_END
#endif
