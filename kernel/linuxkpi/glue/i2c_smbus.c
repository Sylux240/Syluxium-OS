// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LinuxKPI: the SMBus API (include/linux/i2c.h) for Linux drivers.
 *
 * An adapter with a native SMBus controller (algo->smbus_xfer) gets the
 * request as it is; otherwise it is emulated with plain I2C messages over
 * i2c_transfer(), as Linux's drivers/i2c/i2c-core-smbus.c does (without
 * packet error checking). Used by NIC sensor/SFP access (igb), touchpad and
 * sensor drivers on SMBus, and anything else that reads registers through
 * i2c_smbus_*().
 */
#include <linux/lkpi_all.h>
#include <linux/i2c.h>

static s32 smbus_emulated(struct i2c_adapter *adap, u16 addr, unsigned short flags, char read_write, u8 command,
			  int size, union i2c_smbus_data *data)
{
	unsigned char buf0[I2C_SMBUS_BLOCK_MAX + 3];
	unsigned char buf1[I2C_SMBUS_BLOCK_MAX + 2];
	int nmsgs = read_write == I2C_SMBUS_READ ? 2 : 1;
	struct i2c_msg msg[2] = {
		{ .addr = addr, .flags = flags & I2C_M_TEN, .len = 1, .buf = buf0 },
		{ .addr = addr, .flags = (flags & I2C_M_TEN) | I2C_M_RD, .len = 0, .buf = buf1 },
	};
	int i, r;

	buf0[0] = command;
	switch (size) {
	case I2C_SMBUS_QUICK:
		msg[0].len = 0;
		msg[0].flags = (flags & I2C_M_TEN) | (read_write == I2C_SMBUS_READ ? I2C_M_RD : 0);
		nmsgs = 1;
		break;
	case I2C_SMBUS_BYTE:
		if (read_write == I2C_SMBUS_READ) {
			/* A plain read: one message, no command byte. */
			msg[0].flags = I2C_M_RD | (flags & I2C_M_TEN);
			msg[0].buf = buf1;
			nmsgs = 1;
		}
		break;
	case I2C_SMBUS_BYTE_DATA:
		if (read_write == I2C_SMBUS_READ)
			msg[1].len = 1;
		else {
			msg[0].len = 2;
			buf0[1] = data->byte;
		}
		break;
	case I2C_SMBUS_WORD_DATA:
		if (read_write == I2C_SMBUS_READ)
			msg[1].len = 2;
		else {
			msg[0].len = 3;
			buf0[1] = data->word & 0xff;
			buf0[2] = data->word >> 8;
		}
		break;
	case I2C_SMBUS_PROC_CALL:
		nmsgs = 2;
		read_write = I2C_SMBUS_READ;
		msg[0].len = 3;
		msg[1].len = 2;
		buf0[1] = data->word & 0xff;
		buf0[2] = data->word >> 8;
		break;
	case I2C_SMBUS_BLOCK_DATA:
		if (read_write == I2C_SMBUS_READ) {
			msg[1].flags |= I2C_M_RECV_LEN;
			msg[1].len = 1; /* the count byte; the bus driver extends it */
		} else {
			if (data->block[0] == 0 || data->block[0] > I2C_SMBUS_BLOCK_MAX)
				return -EINVAL;
			msg[0].len = data->block[0] + 2;
			for (i = 1; i < msg[0].len; i++)
				buf0[i] = data->block[i - 1];
		}
		break;
	case I2C_SMBUS_I2C_BLOCK_BROKEN:
	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (data->block[0] > I2C_SMBUS_BLOCK_MAX)
			return -EINVAL;
		if (read_write == I2C_SMBUS_READ) {
			msg[1].len = data->block[0];
		} else {
			msg[0].len = data->block[0] + 1;
			for (i = 1; i <= data->block[0]; i++)
				buf0[i] = data->block[i];
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	r = __i2c_transfer(adap, msg, nmsgs);
	if (r < 0)
		return r;
	if (r != nmsgs)
		return -EIO;

	if (read_write == I2C_SMBUS_READ) {
		switch (size) {
		case I2C_SMBUS_BYTE:
			data->byte = buf1[0];
			break;
		case I2C_SMBUS_BYTE_DATA:
			data->byte = buf1[0];
			break;
		case I2C_SMBUS_WORD_DATA:
		case I2C_SMBUS_PROC_CALL:
			data->word = buf1[0] | (buf1[1] << 8);
			break;
		case I2C_SMBUS_I2C_BLOCK_DATA:
		case I2C_SMBUS_I2C_BLOCK_BROKEN:
			for (i = 0; i < data->block[0]; i++)
				data->block[i + 1] = buf1[i];
			break;
		case I2C_SMBUS_BLOCK_DATA:
			if (buf1[0] > I2C_SMBUS_BLOCK_MAX)
				return -EPROTO;
			for (i = 0; i < buf1[0] + 1; i++)
				data->block[i] = buf1[i];
			break;
		}
	}
	return 0;
}

s32 __i2c_smbus_xfer(struct i2c_adapter *adap, u16 addr, unsigned short flags, char read_write, u8 command,
		     int protocol, union i2c_smbus_data *data)
{
	flags &= I2C_M_TEN | I2C_CLIENT_PEC;
	if (adap->algo->smbus_xfer) {
		int r = -EAGAIN, tries;
		for (tries = 0; tries <= adap->retries && r == -EAGAIN; tries++)
			r = adap->algo->smbus_xfer(adap, addr, flags, read_write, command, protocol, data);
		if (r != -EOPNOTSUPP || (!adap->algo->master_xfer && !adap->algo->xfer))
			return r;
	}
	return smbus_emulated(adap, addr, flags, read_write, command, protocol, data);
}

s32 i2c_smbus_xfer(struct i2c_adapter *adap, u16 addr, unsigned short flags, char read_write, u8 command,
		   int protocol, union i2c_smbus_data *data)
{
	s32 r;
	mutex_lock(&adap->bus_lock);
	r = __i2c_smbus_xfer(adap, addr, flags, read_write, command, protocol, data);
	mutex_unlock(&adap->bus_lock);
	return r;
}

#define XFER(c, rw, cmd, size, d) i2c_smbus_xfer((c)->adapter, (c)->addr, (c)->flags, rw, cmd, size, d)

s32 i2c_smbus_read_byte(const struct i2c_client *client)
{
	union i2c_smbus_data d;
	s32 r = XFER(client, I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &d);
	return r < 0 ? r : d.byte;
}
s32 i2c_smbus_write_byte(const struct i2c_client *client, u8 value)
{
	return XFER(client, I2C_SMBUS_WRITE, value, I2C_SMBUS_BYTE, NULL);
}
s32 i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command)
{
	union i2c_smbus_data d;
	s32 r = XFER(client, I2C_SMBUS_READ, command, I2C_SMBUS_BYTE_DATA, &d);
	return r < 0 ? r : d.byte;
}
s32 i2c_smbus_write_byte_data(const struct i2c_client *client, u8 command, u8 value)
{
	union i2c_smbus_data d = { .byte = value };
	return XFER(client, I2C_SMBUS_WRITE, command, I2C_SMBUS_BYTE_DATA, &d);
}
s32 i2c_smbus_read_word_data(const struct i2c_client *client, u8 command)
{
	union i2c_smbus_data d;
	s32 r = XFER(client, I2C_SMBUS_READ, command, I2C_SMBUS_WORD_DATA, &d);
	return r < 0 ? r : d.word;
}
s32 i2c_smbus_write_word_data(const struct i2c_client *client, u8 command, u16 value)
{
	union i2c_smbus_data d = { .word = value };
	return XFER(client, I2C_SMBUS_WRITE, command, I2C_SMBUS_WORD_DATA, &d);
}
s32 i2c_smbus_read_block_data(const struct i2c_client *client, u8 command, u8 *values)
{
	union i2c_smbus_data d;
	s32 r = XFER(client, I2C_SMBUS_READ, command, I2C_SMBUS_BLOCK_DATA, &d);
	if (r)
		return r;
	memcpy(values, &d.block[1], d.block[0]);
	return d.block[0];
}
s32 i2c_smbus_write_block_data(const struct i2c_client *client, u8 command, u8 length, const u8 *values)
{
	union i2c_smbus_data d;
	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	d.block[0] = length;
	memcpy(&d.block[1], values, length);
	return XFER(client, I2C_SMBUS_WRITE, command, I2C_SMBUS_BLOCK_DATA, &d);
}
s32 i2c_smbus_read_i2c_block_data(const struct i2c_client *client, u8 command, u8 length, u8 *values)
{
	union i2c_smbus_data d;
	s32 r;
	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	d.block[0] = length;
	r = XFER(client, I2C_SMBUS_READ, command, I2C_SMBUS_I2C_BLOCK_DATA, &d);
	if (r < 0)
		return r;
	memcpy(values, &d.block[1], d.block[0]);
	return d.block[0];
}
s32 i2c_smbus_write_i2c_block_data(const struct i2c_client *client, u8 command, u8 length, const u8 *values)
{
	union i2c_smbus_data d;
	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	d.block[0] = length;
	memcpy(&d.block[1], values, length);
	return XFER(client, I2C_SMBUS_WRITE, command, I2C_SMBUS_I2C_BLOCK_DATA, &d);
}
