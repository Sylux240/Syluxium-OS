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
#define I2C_M_RECV_LEN 0x0400
#define I2C_M_NO_RD_ACK 0x0800
#define I2C_M_IGNORE_NAK 0x1000
#define I2C_M_REV_DIR_ADDR 0x2000
	__u16 len;
	__u8 *buf;
};
#define I2C_FUNC_I2C 0x00000001
#define I2C_FUNC_10BIT_ADDR 0x00000002
#define I2C_FUNC_PROTOCOL_MANGLING 0x00000004
#define I2C_FUNC_SMBUS_PEC 0x00000008
#define I2C_FUNC_NOSTART 0x00000010
#define I2C_FUNC_SLAVE 0x00000020
#define I2C_FUNC_SMBUS_BLOCK_PROC_CALL 0x00008000
#define I2C_FUNC_SMBUS_QUICK 0x00010000
#define I2C_FUNC_SMBUS_READ_BYTE 0x00020000
#define I2C_FUNC_SMBUS_WRITE_BYTE 0x00040000
#define I2C_FUNC_SMBUS_READ_BYTE_DATA 0x00080000
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA 0x00100000
#define I2C_FUNC_SMBUS_READ_WORD_DATA 0x00200000
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA 0x00400000
#define I2C_FUNC_SMBUS_PROC_CALL 0x00800000
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA 0x01000000
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000
#define I2C_FUNC_SMBUS_READ_I2C_BLOCK 0x04000000
#define I2C_FUNC_SMBUS_WRITE_I2C_BLOCK 0x08000000
#define I2C_FUNC_SMBUS_EMUL 0x0eff0008
#define I2C_FUNC_SMBUS_EMUL_ALL (I2C_FUNC_SMBUS_EMUL | I2C_FUNC_SMBUS_READ_BLOCK_DATA | I2C_FUNC_SMBUS_BLOCK_PROC_CALL)
#define I2C_SMBUS_BLOCK_MAX 32
#define I2C_AQ_NO_CLK_STRETCH (1 << 4)
#define I2C_AQ_COMB (1 << 0)
#define I2C_AQ_COMB_WRITE_FIRST (1 << 1)
#define I2C_AQ_COMB_READ_SECOND (1 << 2)
#define I2C_AQ_COMB_SAME_ADDR (1 << 3)
#define I2C_AQ_NO_ZERO_LEN_READ (1 << 5)
#define I2C_AQ_NO_ZERO_LEN_WRITE (1 << 6)
#define I2C_AQ_NO_ZERO_LEN (I2C_AQ_NO_ZERO_LEN_READ | I2C_AQ_NO_ZERO_LEN_WRITE)
#define I2C_AQ_NO_REP_START (1 << 7)
static inline u8 i2c_8bit_addr_from_msg(const struct i2c_msg *msg) { return (msg->addr << 1) | (msg->flags & I2C_M_RD); }
#define I2C_NAME_SIZE 20
#define I2C_CLASS_HWMON (1 << 0)
#define I2C_CLASS_DDC (1 << 3)
#define I2C_LOCK_ROOT_ADAPTER 0x01
#define I2C_LOCK_SEGMENT 0x02
struct i2c_adapter;
union i2c_smbus_data;
struct i2c_algorithm {
	int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	u32 (*functionality)(struct i2c_adapter *adap);
	int (*master_xfer_atomic)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	/* appended: native SMBus controllers (else SMBus is emulated over master_xfer) */
	int (*smbus_xfer)(struct i2c_adapter *adap, u16 addr, unsigned short flags, char read_write, u8 command,
			  int size, union i2c_smbus_data *data);
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
	const struct i2c_lock_operations *lock_ops; /* appended */
	const struct i2c_adapter_quirks *quirks;
};
struct i2c_lock_operations {
	void (*lock_bus)(struct i2c_adapter *adapter, unsigned int flags);
	int (*trylock_bus)(struct i2c_adapter *adapter, unsigned int flags);
	void (*unlock_bus)(struct i2c_adapter *adapter, unsigned int flags);
};
struct i2c_adapter_quirks { u64 flags; int max_num_msgs; u16 max_write_len, max_read_len, max_comb_1st_msg_len, max_comb_2nd_msg_len; };
#define to_i2c_adapter(d) container_of(d, struct i2c_adapter, dev)
static inline void *i2c_get_adapdata(const struct i2c_adapter *a) { return dev_get_drvdata(&a->dev); }
static inline void i2c_set_adapdata(struct i2c_adapter *a, void *data) { dev_set_drvdata(&a->dev, data); }
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int i2c_add_adapter(struct i2c_adapter *adap);
void i2c_del_adapter(struct i2c_adapter *adap);
static inline int i2c_add_numbered_adapter(struct i2c_adapter *a) { return i2c_add_adapter(a); }
#define i2c_get_adapter(nr) ((struct i2c_adapter *)NULL)
#define i2c_put_adapter(a) do { } while (0)
#define i2c_lock_bus(a, f) do { if ((a)->lock_ops) (a)->lock_ops->lock_bus(a, f); else mutex_lock(&(a)->bus_lock); } while (0)
#define i2c_unlock_bus(a, f) do { if ((a)->lock_ops) (a)->lock_ops->unlock_bus(a, f); else mutex_unlock(&(a)->bus_lock); } while (0)

/* I2C client drivers (bridges/panels on embedded boards): never probed on PCs. */
struct i2c_device_id { char name[20]; unsigned long driver_data; };
struct i2c_board_info { char type[20]; unsigned short flags; unsigned short addr; void *platform_data; };
struct i2c_client { unsigned short flags; unsigned short addr; char name[20]; struct i2c_adapter *adapter; struct device dev; };
struct i2c_board_info;
struct i2c_driver {
	int (*probe)(struct i2c_client *client);
	int (*detect)(struct i2c_client *client, struct i2c_board_info *info);
	void (*remove)(struct i2c_client *client);
	struct device_driver driver;
	const struct i2c_device_id *id_table;
};
#define i2c_register_driver(owner, drv) 0
#define i2c_add_driver(drv) 0
#define i2c_del_driver(drv) do { } while (0)
#define to_i2c_client(d) container_of(d, struct i2c_client, dev)

/* ---- SMBus (appended): glue/i2c_smbus.c, emulated over i2c_transfer unless
 * the adapter has a native smbus_xfer (as Linux's drivers/i2c/i2c-core-smbus.c) ---- */
#define I2C_SMBUS_READ 1
#define I2C_SMBUS_WRITE 0
#define I2C_SMBUS_QUICK 0
#define I2C_SMBUS_BYTE 1
#define I2C_SMBUS_BYTE_DATA 2
#define I2C_SMBUS_WORD_DATA 3
#define I2C_SMBUS_PROC_CALL 4
#define I2C_SMBUS_BLOCK_DATA 5
#define I2C_SMBUS_I2C_BLOCK_BROKEN 6
#define I2C_SMBUS_BLOCK_PROC_CALL 7
#define I2C_SMBUS_I2C_BLOCK_DATA 8
#define I2C_CLIENT_PEC 0x04
#define I2C_CLIENT_TEN 0x10
union i2c_smbus_data {
	__u8 byte;
	__u16 word;
	__u8 block[I2C_SMBUS_BLOCK_MAX + 2]; /* block[0] is the length */
};
s32 i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr, unsigned short flags, char read_write, u8 command,
		   int protocol, union i2c_smbus_data *data);
s32 __i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr, unsigned short flags, char read_write, u8 command,
		     int protocol, union i2c_smbus_data *data);
s32 i2c_smbus_read_byte(const struct i2c_client *client);
s32 i2c_smbus_write_byte(const struct i2c_client *client, u8 value);
s32 i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command);
s32 i2c_smbus_write_byte_data(const struct i2c_client *client, u8 command, u8 value);
s32 i2c_smbus_read_word_data(const struct i2c_client *client, u8 command);
s32 i2c_smbus_write_word_data(const struct i2c_client *client, u8 command, u16 value);
s32 i2c_smbus_read_block_data(const struct i2c_client *client, u8 command, u8 *values);
s32 i2c_smbus_write_block_data(const struct i2c_client *client, u8 command, u8 length, const u8 *values);
s32 i2c_smbus_read_i2c_block_data(const struct i2c_client *client, u8 command, u8 length, u8 *values);
s32 i2c_smbus_write_i2c_block_data(const struct i2c_client *client, u8 command, u8 length, const u8 *values);
static inline s32 i2c_smbus_read_word_swapped(const struct i2c_client *client, u8 command)
{
	s32 v = i2c_smbus_read_word_data(client, command);
	return v < 0 ? v : (s32)swab16((u16)v);
}
static inline s32 i2c_smbus_write_word_swapped(const struct i2c_client *client, u8 command, u16 value)
{
	return i2c_smbus_write_word_data(client, command, swab16(value));
}
static inline u32 i2c_get_functionality(struct i2c_adapter *adap)
{
	if (adap->algo->functionality)
		return adap->algo->functionality(adap);
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}
static inline int i2c_check_functionality(struct i2c_adapter *adap, u32 func)
{
	return (func & i2c_get_functionality(adap)) == func;
}
