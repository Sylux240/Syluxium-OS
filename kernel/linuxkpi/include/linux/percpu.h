/* LinuxKPI: per-CPU data. One shared instance (updates are counters and
 * statistics; an occasional lost increment between CPUs is harmless). */
#ifndef _LKPI_LINUX_PERCPU_H
#define _LKPI_LINUX_PERCPU_H

#include <linux/kernel.h>
#include <linux/slab.h>

LKPI_EXTERN_C_BEGIN

int lkpi_smp_processor_id(void);
#define smp_processor_id() lkpi_smp_processor_id()
#define raw_smp_processor_id() lkpi_smp_processor_id()
#define get_cpu() ({ preempt_disable(); smp_processor_id(); })
#define put_cpu() preempt_enable()
void lkpi_preempt_disable(void);
void lkpi_preempt_enable(void);
#define preempt_disable() lkpi_preempt_disable()
#define preempt_enable() lkpi_preempt_enable()
#define preempt_enable_no_resched() lkpi_preempt_enable()
#define migrate_disable() lkpi_preempt_disable()
#define migrate_enable() lkpi_preempt_enable()
#define num_possible_cpus() 1
#define num_online_cpus() 1
#define nr_cpu_ids 1
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < 1; (cpu)++)
#define for_each_online_cpu(cpu) for_each_possible_cpu(cpu)
#define for_each_present_cpu(cpu) for_each_possible_cpu(cpu)
#define cpu_online(cpu) ((cpu) == 0)

#define alloc_percpu(type) ((type*)kzalloc(sizeof(type), GFP_KERNEL))
#define alloc_percpu_gfp(type, gfp) ((type*)kzalloc(sizeof(type), gfp))
#define __alloc_percpu(size, align) kzalloc(size, GFP_KERNEL)
#define __alloc_percpu_gfp(size, align, gfp) kzalloc(size, gfp)
#define free_percpu(p) kfree(p)
#define per_cpu_ptr(p, cpu) ((void)(cpu), (p))
#define this_cpu_ptr(p) (p)
#define raw_cpu_ptr(p) (p)
#define get_cpu_ptr(p) (p)
#define put_cpu_ptr(p) do { (void)(p); } while (0)
#define per_cpu(var, cpu) (*((void)(cpu), &(var)))
#define this_cpu_read(var) (var)
#define this_cpu_write(var, v) ((var) = (v))
#define this_cpu_inc(var) ((var)++)
#define this_cpu_dec(var) ((var)--)
#define this_cpu_add(var, v) ((var) += (v))
#define this_cpu_sub(var, v) ((var) -= (v))
#define __this_cpu_inc this_cpu_inc
#define __this_cpu_add this_cpu_add
#define __this_cpu_read this_cpu_read
#define __this_cpu_write this_cpu_write
#define DEFINE_PER_CPU(type, name) type name
#define DECLARE_PER_CPU(type, name) extern type name
#define DEFINE_PER_CPU_ALIGNED(type, name) type name

LKPI_EXTERN_C_END

#endif
