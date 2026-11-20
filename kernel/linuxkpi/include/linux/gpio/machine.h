/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct gpiod_lookup { const char *key; u16 chip_hwnum; const char *con_id; unsigned int idx; unsigned long flags; };
struct gpiod_lookup_table { struct list_head list; const char *dev_id; struct gpiod_lookup table[]; };
#define GPIO_LOOKUP_IDX(_key, _chip_hwnum, _con_id, _idx, _flags) { .key = _key, .chip_hwnum = _chip_hwnum, .con_id = _con_id, .idx = _idx, .flags = _flags }
#define gpiod_add_lookup_table(t) do { } while (0)
#define gpiod_remove_lookup_table(t) do { } while (0)
#define GPIO_ACTIVE_HIGH 0
#define GPIO_ACTIVE_LOW 1
