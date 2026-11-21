/* Linux 64-bit stdint types */
/* In Linux, uint64_t is u64 (unsigned long long), so u64* and uint64_t*
 * are interchangeable in driver code. Make <stdint.h> agree (C only). */
#ifndef __cplusplus
#undef __UINT64_TYPE__
#define __UINT64_TYPE__ long long unsigned int
#undef __INT64_TYPE__
#define __INT64_TYPE__ long long int
#undef __UINT64_C_SUFFIX__
#define __UINT64_C_SUFFIX__ ULL
#undef __INT64_C_SUFFIX__
#define __INT64_C_SUFFIX__ LL
#endif
/* LinuxKPI: the kernel configuration that Linux sources compiled into
 * Syluxium see (included first, like Linux's autoconf.h + kconfig.h). */
#ifndef _LKPI_LINUX_KCONFIG_H
#define _LKPI_LINUX_KCONFIG_H

/* ---- Selected options ---- */
#define CONFIG_64BIT 1
#define CONFIG_X86 1
#define CONFIG_X86_64 1
#define CONFIG_SMP 1
#define CONFIG_NR_CPUS 64
#define CONFIG_HZ 1000
#define CONFIG_NET 1
#define CONFIG_USB 1
#define CONFIG_PCI 1
#define CONFIG_WLAN 1
#define CONFIG_CFG80211 1
#define CONFIG_MAC80211 1
#define CONFIG_MAC80211_RC_MINSTREL 1
#define CONFIG_MAC80211_RC_DEFAULT_MINSTREL 1
#define CONFIG_MAC80211_RC_DEFAULT "minstrel_ht"
#define CONFIG_MAC80211_STA_HASH_MAX_SIZE 0
#define CONFIG_CFG80211_DEFAULT_PS 1
#define CONFIG_CFG80211_CRDA_SUPPORT 1
#define CONFIG_CFG80211_REQUIRE_SIGNED_REGDB_OFF 1
#define CONFIG_MT7601U 1
#define CONFIG_MAC80211_HWSIM 1
#define CONFIG_RTL8XXXU 1
#define CONFIG_BASE_SMALL 0
#define CONFIG_PAGE_SHIFT 12
/* DRM fbdev emulation: GPU drivers light the screen up themselves and hand
 * their console framebuffer to the Syluxium console (glue/fbdev.c). */
#define CONFIG_DRM_FBDEV_EMULATION 1
#define CONFIG_DRM_FBDEV_OVERALLOC 100
#define CONFIG_FB_CORE 1
#define CONFIG_FB_DEFERRED_IO 1

/* ---- IS_ENABLED() & co. (from Linux's include/linux/kconfig.h) ---- */
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define __and(x, y) ___and(x, y)
#define ___and(x, y) ____and(__ARG_PLACEHOLDER_##x, y)
#define ____and(arg1_or_junk, y) __take_second_arg(arg1_or_junk y, 0)
#define __or(x, y) ___or(x, y)
#define ___or(x, y) ____or(__ARG_PLACEHOLDER_##x, y)
#define ____or(arg1_or_junk, y) __take_second_arg(arg1_or_junk 1, y)
#define __is_defined(x) ___is_defined(x)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define IS_BUILTIN(option) __is_defined(option)
#define IS_MODULE(option) __is_defined(option##_MODULE)
#define IS_REACHABLE(option) __or(IS_BUILTIN(option), __and(IS_MODULE(option), __is_defined(MODULE)))
#define IS_ENABLED(option) __or(IS_BUILTIN(option), IS_MODULE(option))

#endif

/* Types headers mention in prototypes before their definitions. */
struct seq_file;
struct file;
struct inode;
struct vm_area_struct;
struct poll_table_struct;

/* Small helpers Linux code expects everywhere. */
#define u64_to_user_ptr(x) ((void __user *)(unsigned long)(x))
#define might_lock(lock) do { } while (0)
#define might_lock_read(lock) do { } while (0)

/* Attributes (__must_check, __packed, ...) used by headers before anything else. */
#include <linux/compiler.h>

/* Exports are no-ops in a monolithic kernel; visible from every file. */
#ifndef EXPORT_SYMBOL
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#endif
