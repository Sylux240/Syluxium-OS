/* LinuxKPI: more of Linux's compiler_types.h / compiler_attributes.h /
 * stddef.h / build_bug.h, needed by the larger subsystems (cfg80211,
 * mac80211). Included from <linux/compiler.h>. */
#ifndef _LKPI_LINUX_COMPILER_EXT_H
#define _LKPI_LINUX_COMPILER_EXT_H

#define __bitwise
#define __nocast
#define __safe
#define __private
#define ACCESS_PRIVATE(p, member) ((p)->member)
#define __acquires(x)
#define __releases(x)
#define __acquire(x) (void)0
#define __release(x) (void)0
#define __must_hold(x)
#define __cond_lock(x, c) (c)
#define __cond_acquires(x)
#define __noreturn __attribute__((__noreturn__))
#define __pure __attribute__((__pure__))
#define __attribute_const__ __attribute__((__const__))
#define __section(s) __attribute__((__section__(s)))
#define __weak __attribute__((__weak__))
#define __alias(symbol) __attribute__((__alias__(#symbol)))
#define __visible __attribute__((__externally_visible__))
#define __noinline noinline
#define noinline_for_stack noinline
#define __no_kcsan
#define __no_sanitize_address
#define notrace
#define __randomize_layout
#define __no_randomize_layout
#define randomized_struct_fields_start struct {
#define randomized_struct_fields_end };
#define __designated_init
#define __nonstring
#define __counted_by_le(x)
#define __counted_by_be(x)
#define __net_init
#define __net_exit
#define __net_initdata
#define __exit_p(x) x
#define __ro_after_init
#define __initconst
#define __exitdata
#define __refdata
#define __ref
#define __rcu
#define __kernel
#define __user
#define __percpu
#define __must_be_array(a) 0
#define __same_type(a, b) __builtin_types_compatible_p(__typeof__(a), __typeof__(b))
#define __is_constexpr(x) (sizeof(int) == sizeof(*(8 ? ((void*)((long)(x) * 0l)) : (int*)8)))
#define statically_true(x) (__builtin_constant_p(x) && (x))
#define data_race(expr) (expr)
#define unreachable() __builtin_unreachable()
#define __diag_push()
#define __diag_pop()
#define __diag_ignore(...)
#define __diag_ignore_all(...)
#define __PASTE(a, b) ___PASTE(a, b)
#define ___PASTE(a, b) a##b
#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)
#define __scalar_type_to_expr_cases(type) unsigned type: (unsigned type)0, signed type: (signed type)0
#define __unqual_scalar_typeof(x) __typeof__(_Generic((x), char: (char)0, __scalar_type_to_expr_cases(char), \
    __scalar_type_to_expr_cases(short), __scalar_type_to_expr_cases(int), __scalar_type_to_expr_cases(long), \
    __scalar_type_to_expr_cases(long long), default: (x)))
#define __native_word(t) (sizeof(t) == sizeof(char) || sizeof(t) == sizeof(short) || sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))

#ifndef __cplusplus
#define __compiletime_error(msg) __attribute__((__error__(msg)))
#define __compiletime_assert(condition, msg, prefix, suffix) \
    do { extern void prefix##suffix(void) __compiletime_error(msg); if (!(condition)) prefix##suffix(); } while (0)
#define _compiletime_assert(condition, msg, prefix, suffix) __compiletime_assert(condition, msg, prefix, suffix)
#define compiletime_assert(condition, msg) _compiletime_assert(condition, msg, __compiletime_assert_, __COUNTER__)
#define compiletime_assert_atomic_type(t) do { } while (0)
#define BUILD_BUG_ON_ZERO(e) ((int)(sizeof(struct { int:(-!!(e)); })))
#define BUILD_BUG_ON_MSG(cond, msg) compiletime_assert(!(cond), msg)
#define BUILD_BUG_ON_INVALID(e) ((void)(sizeof((long)(e))))
#define BUILD_BUG() BUILD_BUG_ON_MSG(1, "BUILD_BUG failed")
#define __BUILD_BUG_ON_NOT_POWER_OF_2(n) BUILD_BUG_ON(((n) & ((n) - 1)) != 0)
#define BUILD_BUG_ON_NOT_POWER_OF_2(n) BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))
#ifndef static_assert
#define static_assert(expr, ...) __static_assert(expr, ##__VA_ARGS__, #expr)
#define __static_assert(expr, msg, ...) _Static_assert(expr, msg)
#endif
#endif

/* <linux/stddef.h> / <uapi/linux/stddef.h> */
#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE*)0)->MEMBER))
#define offsetofend(TYPE, MEMBER) (offsetof(TYPE, MEMBER) + sizeof_field(TYPE, MEMBER))
#define typeof_member(T, m) __typeof__(((T*)0)->m)
#define __struct_group(TAG, NAME, ATTRS, MEMBERS...) \
    union { struct { MEMBERS } ATTRS; struct TAG { MEMBERS } ATTRS NAME; } ATTRS
#define struct_group(NAME, MEMBERS...) __struct_group(/* no tag */, NAME, /* no attrs */, MEMBERS)
#define struct_group_attr(NAME, ATTRS, MEMBERS...) __struct_group(/* no tag */, NAME, ATTRS, MEMBERS)
#define struct_group_tagged(TAG, NAME, MEMBERS...) __struct_group(TAG, NAME, /* no attrs */, MEMBERS)
#define __DECLARE_FLEX_ARRAY(TYPE, NAME) struct { struct { } __empty_##NAME; TYPE NAME[]; }
#define DECLARE_FLEX_ARRAY(TYPE, NAME) __DECLARE_FLEX_ARRAY(TYPE, NAME)
#define __stringify_1(x...) #x
#define __stringify(x...) __stringify_1(x)

#endif
