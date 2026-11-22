/* LinuxKPI: no kernel memory sanitizer. */
#pragma once
#define kmsan_handle_dma(page, offset, size, dir) do { } while (0)
#define kmsan_unpoison_memory(addr, size) do { } while (0)
