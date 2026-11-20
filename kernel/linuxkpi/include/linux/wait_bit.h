/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: waiting on a bit - a shared wait queue polled on wake. */
#include <linux/wait.h>
wait_queue_head_t *bit_waitqueue(void *word, int bit);
#define var_waitqueue(var) bit_waitqueue(var, 0)
#define __var_waitqueue(var) bit_waitqueue((void *)(var), 0)
#define ___wait_var_event(var, cond, state, excl, ret, cmd) ({ wait_event(*__var_waitqueue(var), cond); 0; })
void wake_up_bit(void *word, int bit);
#define wake_up_var(var) wake_up_bit(var, 0)
#define wait_var_event(var, cond) wait_event(*var_waitqueue(var), cond)
#define wait_var_event_timeout(var, cond, t) wait_event_timeout(*var_waitqueue(var), cond, t)
#define wait_var_event_killable(var, cond) wait_event_killable(*var_waitqueue(var), cond)
#define wait_on_bit(word, bit, mode) ({ wait_event(*bit_waitqueue(word, bit), !test_bit(bit, word)); 0; })
#define wait_on_bit_timeout(word, bit, mode, t) ({ wait_event_timeout(*bit_waitqueue(word, bit), !test_bit(bit, word), t) ? 0 : -EAGAIN; })
#define clear_and_wake_up_bit(bit, word) do { clear_bit(bit, word); smp_mb__after_atomic(); wake_up_bit(word, bit); } while (0)
