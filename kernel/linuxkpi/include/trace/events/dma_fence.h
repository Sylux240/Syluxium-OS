/* LinuxKPI: dma-fence tracepoints (no tracing in Syluxium yet). */
#pragma once
#include <linux/lkpi_all.h>
#define trace_dma_fence_emit(...) do { } while (0)
#define trace_dma_fence_init(...) do { } while (0)
#define trace_dma_fence_destroy(...) do { } while (0)
#define trace_dma_fence_enable_signal(...) do { } while (0)
#define trace_dma_fence_signaled(...) do { } while (0)
#define trace_dma_fence_wait_start(...) do { } while (0)
#define trace_dma_fence_wait_end(...) do { } while (0)
#ifndef EXPORT_TRACEPOINT_SYMBOL
#define EXPORT_TRACEPOINT_SYMBOL(name)
#define EXPORT_TRACEPOINT_SYMBOL_GPL(name)
#endif
