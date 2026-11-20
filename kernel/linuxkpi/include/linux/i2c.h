/* LinuxKPI: I2C buses as GPU drivers use them (DDC/EDID reads). */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/device.h>
#include <linux/mutex.h>

struct i2c_msg {
	__u16 addr;
	__u16 flags;
#define I2C_M_RD 0x0001
#define I2C_M_TEN 0x0010
#define I2C_M_STOP 0x8000
#define I2C_M_NOSTART 0x4000
	__u16 len;
	__u8 *buf;
};
#define I2C_FUNC_I2C 0x00000001
#define I2C_FUNC_SMBUS_EMUL 0x0eff0008
struct i2c_adapter;
struct i2c_algorithm {
	int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	u32 (*functionality)(struct i2c_adapter *adap);
};
struct i2c_adapter {
	struct module *owner;
	unsigned int class;
	const struct i2c_algorithm *algo;
	void *algo_data;
	struct mutex bus_lock;
	int timeout;
	int retries;
	struct device dev;
	int nr;
	char name[48];
};
#define to_i2c_adapter(d) container_of(d, struct i2c_adapter, dev)
static inline void *i2c_get_adapdata(const struct i2c_adapter *a) { return dev_get_drvdata(&a->dev); }
static inline void i2c_set_adapdata(struct i2c_adapter *a, void *data) { dev_set_drvdata(&a->dev, data); }
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int i2c_add_adapter(struct i2c_adapter *adap);
void i2c_del_adapter(struct i2c_adapter *adap);
#define i2c_get_adapter(nr) ((struct i2c_adapter *)NULL)
#define i2c_put_adapter(a) do { } while (0)
#define i2c_lock_bus(a, f) mutex_lock(&(a)->bus_lock)
#define i2c_unlock_bus(a, f) mutex_unlock(&(a)->bus_lock)

/* I2C client drivers (bridges/panels on embedded boards): never probed on PCs. */
struct i2c_device_id { char name[20]; unsigned long driver_data; };
struct i2c_board_info { char type[20]; unsigned short flags; unsigned short addr; void *platform_data; };
struct i2c_client { unsigned short flags; unsigned short addr; char name[20]; struct i2c_adapter *adapter; struct device dev; };
struct i2c_driver {
	int (*probe)(struct i2c_client *client);
	void (*remove)(struct i2c_client *client);
	struct device_driver driver;
	const struct i2c_device_id *id_table;
};
#define i2c_register_driver(owner, drv) 0
#define i2c_add_driver(drv) 0
#define i2c_del_driver(drv) do { } while (0)
#define to_i2c_client(d) container_of(d, struct i2c_client, dev)
