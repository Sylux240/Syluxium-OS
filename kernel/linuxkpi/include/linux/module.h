#ifndef _LKPI_LINUX_MODULE_H
#define _LKPI_LINUX_MODULE_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* Drivers are built into the kernel: module_init() functions are collected
 * in a linker section and run once at boot, before devices are probed. */
typedef int (*lkpi_initcall_t)(void);
/* Levels as in Linux (section names sort in that order): subsystems
 * (cfg80211, mac80211) initialize before the drivers that use them. */
#define __lkpi_define_initcall(fn, level)                                                  \
    static lkpi_initcall_t __lkpi_initcall_##fn __attribute__((used, section(".lkpi_initcalls." #level))) = fn
#define core_initcall(fn) __lkpi_define_initcall(fn, 1)
#define postcore_initcall(fn) __lkpi_define_initcall(fn, 2)
#define arch_initcall(fn) __lkpi_define_initcall(fn, 3)
#define subsys_initcall(fn) __lkpi_define_initcall(fn, 4)
#define fs_initcall(fn) __lkpi_define_initcall(fn, 5)
#define device_initcall(fn) __lkpi_define_initcall(fn, 6)
#define module_init(fn) __lkpi_define_initcall(fn, 6)
#define late_initcall(fn) __lkpi_define_initcall(fn, 7)
#define late_initcall_sync(fn) __lkpi_define_initcall(fn, 7)
#define module_exit(fn) static void (*__lkpi_exitcall_##fn)(void) __attribute__((unused)) = fn

#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define MODULE_FIRMWARE(x)
#define MODULE_ALIAS(x)
#define MODULE_SOFTDEP(x)
#define MODULE_IMPORT_NS(x)
#define MODULE_ALIAS_NET_PF_PROTO_NAME(pf, proto, name)
#define EXPORT_SYMBOL_IF_KUNIT(sym)
#define VISIBLE_IF_KUNIT static
#define MODULE_ALIAS_RTNL_LINK(kind)
#define MODULE_INFO(tag, info)
#define kernel_param_lock(mod) do { } while (0)
#define kernel_param_unlock(mod) do { } while (0)
#define module_param_cb(name, ops, arg, perm)
#define module_param_string(name, string, len, perm)
#define module_param_hw(name, type, hwtype, perm)
#define __MODULE_PARM_TYPE(name, _type)
struct kernel_param;
struct kernel_param_ops {
    unsigned int flags;
    int (*set)(const char* val, const struct kernel_param* kp);
    int (*get)(char* buffer, const struct kernel_param* kp);
    void (*free)(void* arg);
};
struct kernel_param {
    const char* name;
    struct module* mod;
    const struct kernel_param_ops* ops;
    const unsigned short perm;
    signed char level;
    unsigned char flags;
    union { void* arg; };
};
int param_set_uint(const char* val, const struct kernel_param* kp);
int param_get_uint(char* buffer, const struct kernel_param* kp);
int param_set_int(const char* val, const struct kernel_param* kp);
int param_get_int(char* buffer, const struct kernel_param* kp);
int param_set_bool(const char* val, const struct kernel_param* kp);
int param_get_bool(char* buffer, const struct kernel_param* kp);
#define MODULE_DEVICE_TABLE(type, name)
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define EXPORT_SYMBOL_NS_GPL(x, ns)
#define THIS_MODULE ((void*)0)
#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "syluxium"
#endif
/* Module parameters can be set on the kernel command line, as for built-in
 * Linux drivers: <module>.<param>=<value> (e.g. mac80211_hwsim.radios=2). */
struct lkpi_param {
    const char* name;
    void* var;
    char type;
};
#define __lkpi_param_type_int 'i'
#define __lkpi_param_type_uint 'u'
#define __lkpi_param_type_long 'l'
#define __lkpi_param_type_ulong 'L'
#define __lkpi_param_type_short 's'
#define __lkpi_param_type_ushort 'S'
#define __lkpi_param_type_byte 'b'
#define __lkpi_param_type_bool 'B'
#define __lkpi_param_type__Bool 'B' /* bool is a macro (stdbool.h) */
#define __lkpi_param_type_invbool 'I'
#define __lkpi_param_type_charp 'c'
#define module_param_named(name, var, type, perm)                                               \
    static const struct lkpi_param __lkpi_param_##name __attribute__((used, section(".lkpi_params"))) = { \
        KBUILD_MODNAME "." #name, (void*)&(var), __lkpi_param_type_##type}
#define module_param(name, type, perm) module_param_named(name, name, type, perm)
#define module_param_array(name, type, nump, perm)
#define MODULE_PARM_DESC(p, d)
#define try_module_get(m) 1
#define module_put(m) do { } while (0)
struct module;

LKPI_EXTERN_C_END

#endif
