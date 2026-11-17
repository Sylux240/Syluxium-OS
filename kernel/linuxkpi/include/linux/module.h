#ifndef _LKPI_LINUX_MODULE_H
#define _LKPI_LINUX_MODULE_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* Drivers are built into the kernel: module_init() functions are collected
 * in a linker section and run once at boot, before devices are probed. */
typedef int (*lkpi_initcall_t)(void);
#define module_init(fn)                                                                    \
    static lkpi_initcall_t __lkpi_initcall_##fn __attribute__((used, section(".lkpi_initcalls"))) = fn
#define module_exit(fn) static void (*__lkpi_exitcall_##fn)(void) __attribute__((unused)) = fn
#define late_initcall(fn) module_init(fn)
#define subsys_initcall(fn) module_init(fn)
#define device_initcall(fn) module_init(fn)

#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define MODULE_FIRMWARE(x)
#define MODULE_ALIAS(x)
#define MODULE_SOFTDEP(x)
#define MODULE_IMPORT_NS(x)
#define MODULE_DEVICE_TABLE(type, name)
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define EXPORT_SYMBOL_NS_GPL(x, ns)
#define THIS_MODULE ((void*)0)
#define KBUILD_MODNAME "syluxium"
#define module_param(name, type, perm)
#define module_param_named(name, var, type, perm)
#define module_param_array(name, type, nump, perm)
#define MODULE_PARM_DESC(p, d)
#define try_module_get(m) 1
#define module_put(m) do { } while (0)
struct module;

LKPI_EXTERN_C_END

#endif
