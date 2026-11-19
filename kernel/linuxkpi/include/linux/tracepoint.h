/* LinuxKPI: tracepoints compiled out (as with CONFIG_TRACEPOINTS=n): every
 * trace_foo() becomes an empty inline function. */
#ifndef _LKPI_LINUX_TRACEPOINT_H
#define _LKPI_LINUX_TRACEPOINT_H
#include <linux/lkpi_all.h>
#define PARAMS(args...) args
#define TP_PROTO(args...) args
#define TP_ARGS(args...) args
#define TP_CONDITION(args...) args
#define DECLARE_TRACE(name, proto, args)                                         \
    static inline void trace_##name(proto) {}                                    \
    static inline bool trace_##name##_enabled(void) { return false; }
#define DECLARE_EVENT_CLASS(name, proto, args, tstruct, assign, print)
#define DEFINE_EVENT(template, name, proto, args) DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define DEFINE_EVENT_FN(template, name, proto, args, reg, unreg) DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define DEFINE_EVENT_PRINT(template, name, proto, args, print) DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define DEFINE_EVENT_CONDITION(template, name, proto, args, cond) DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define TRACE_EVENT(name, proto, args, struct, assign, print) DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define TRACE_EVENT_FN(name, proto, args, struct, assign, print, reg, unreg) DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define TRACE_EVENT_CONDITION(name, proto, args, cond, struct, assign, print) DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define TRACE_DEFINE_ENUM(x)
#define TRACE_DEFINE_SIZEOF(x)
#define EXPORT_TRACEPOINT_SYMBOL(x)
#define EXPORT_TRACEPOINT_SYMBOL_GPL(x)
#endif
