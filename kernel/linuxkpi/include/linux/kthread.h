/* LinuxKPI: kernel threads (Syluxium kernel threads underneath). */
#ifndef _LKPI_LINUX_KTHREAD_H
#define _LKPI_LINUX_KTHREAD_H
#include <linux/lkpi_all.h>
LKPI_EXTERN_C_BEGIN
struct task_struct* lkpi_kthread_run(int (*fn)(void* data), void* data, const char* name);
#define kthread_run(fn, data, namefmt, ...) lkpi_kthread_run(fn, data, namefmt)
static inline bool kthread_should_stop(void) { return false; }
static inline int kthread_stop(struct task_struct* t) { return 0; }
LKPI_EXTERN_C_END
#endif
