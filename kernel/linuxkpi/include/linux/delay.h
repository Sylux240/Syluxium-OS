#ifndef _LKPI_LINUX_DELAY_H
#define _LKPI_LINUX_DELAY_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

void udelay(unsigned long us);   /* busy-wait */
void ndelay(unsigned long ns);
void msleep(unsigned int ms);    /* sleeps */
unsigned long msleep_interruptible(unsigned int ms);
void usleep_range(unsigned long min_us, unsigned long max_us);
#define mdelay(ms) udelay((unsigned long)(ms) * 1000)
#define fsleep(us) usleep_range(us, us)
#define ssleep(s) msleep((s) * 1000)
#define cpu_relax() __builtin_ia32_pause()
void schedule(void);
long schedule_timeout(long timeout);
#define schedule_timeout_interruptible(t) schedule_timeout(t)
#define schedule_timeout_uninterruptible(t) schedule_timeout(t)
#define cond_resched() do { } while (0)

LKPI_EXTERN_C_END

#endif
