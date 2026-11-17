#ifndef _LKPI_LINUX_COMPLETION_H
#define _LKPI_LINUX_COMPLETION_H

#include <linux/wait.h>

LKPI_EXTERN_C_BEGIN

struct completion {
    volatile unsigned int done;
    wait_queue_head_t wait;
};

#define COMPLETION_INITIALIZER(w) {0, __WAIT_QUEUE_HEAD_INITIALIZER((w).wait)}
#define DECLARE_COMPLETION(w) struct completion w = COMPLETION_INITIALIZER(w)
#define DECLARE_COMPLETION_ONSTACK(w) DECLARE_COMPLETION(w)

void init_completion(struct completion* c);
#define reinit_completion(c) ((c)->done = 0)
void complete(struct completion* c);
void complete_all(struct completion* c);
void wait_for_completion(struct completion* c);
unsigned long wait_for_completion_timeout(struct completion* c, unsigned long timeout);
int wait_for_completion_interruptible(struct completion* c);
long wait_for_completion_interruptible_timeout(struct completion* c, unsigned long timeout);
#define wait_for_completion_killable(c) wait_for_completion_interruptible(c)
#define wait_for_completion_killable_timeout(c, t) wait_for_completion_interruptible_timeout(c, t)
bool try_wait_for_completion(struct completion* c);
bool completion_done(struct completion* c);

LKPI_EXTERN_C_END

#endif
