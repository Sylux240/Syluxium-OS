/* LinuxKPI: register maps. None are created (regmap_init fails): drivers
 * that offer them as an extra (btintel's debug registers) go on without. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/err.h>

struct regmap;
struct device;
enum regmap_endian { REGMAP_ENDIAN_DEFAULT = 0, REGMAP_ENDIAN_BIG, REGMAP_ENDIAN_LITTLE, REGMAP_ENDIAN_NATIVE };
struct regmap_bus {
    int (*write)(void *context, const void *data, size_t count);
    int (*gather_write)(void *context, const void *reg, size_t reg_len, const void *val, size_t val_len);
    int (*read)(void *context, const void *reg_buf, size_t reg_size, void *val_buf, size_t val_size);
    void (*free_context)(void *context);
    enum regmap_endian reg_format_endian_default;
    enum regmap_endian val_format_endian_default;
};
struct regmap_config {
    const char *name;
    int reg_bits;
    int reg_stride;
    int pad_bits;
    int val_bits;
    unsigned int max_register;
};
static inline struct regmap *regmap_init(struct device *dev, const struct regmap_bus *bus, void *bus_context,
                                         const struct regmap_config *config)
{
    return ERR_PTR(-EOPNOTSUPP);
}
static inline void regmap_exit(struct regmap *map) {}
