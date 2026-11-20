/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#define mark_page_accessed_folio(f) do { } while (0)
#define lru_add_drain() do { } while (0)
#define nr_free_buffer_pages() lkpi_totalram_pages()
#define get_nr_swap_pages() 0L
#define total_swap_pages 0L
