// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2006-2007 PA Semi, Inc
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <asm/io.h>
#include <i2c.h>
#include <linux/delay.h>

/* Register offsets */
#define REG_MTXFIFO	0x00
#define REG_MRXFIFO	0x04
#define REG_SMSTA	0x14
#define REG_CTL		0x1c
#define REG_REV		0x28
#define REG_RDCOUNT	0x2c
#define REG_LOCK	0x44

/* Register defs */
#define MTXFIFO_READ	0x00000400
#define MTXFIFO_STOP	0x00000200
#define MTXFIFO_START	0x00000100
#define MTXFIFO_DATA_M	0x000000ff

#define MRXFIFO_EMPTY	0x00000100
#define MRXFIFO_DATA_M	0x000000ff

#define SMSTA_XEN	0x08000000
#define SMSTA_MTN	0x00200000

#define CTL_MAGIC	0x00000800
#define CTL_MRR		0x00000400
#define CTL_MTR		0x00000200
#define CTL_CLK_M	0x000000ff

#define RDCOUNT_SHIFT	8
#define RDCOUNT_MASK	0x0000ff00

#define CLK_100K_DIV	15
#define CLK_400K_DIV	21

struct apple_i2c_priv {
	struct clk_bulk clks;
	void *base;
};

static inline u8 i2c_8bit_addr_from_msg(const struct i2c_msg *msg)
{
	return (msg->addr << 1) | (msg->flags & I2C_M_RD ? 1 : 0);
}

static inline void reg_write(struct apple_i2c_priv *priv, int reg, int val)
{
	writel(val, priv->base + reg);
}

static inline int reg_read(struct apple_i2c_priv *priv, int reg)
{
	int val;
	val = readl(priv->base + reg);
	return val;
}

#define TXFIFO_WR(priv, reg)	reg_write((priv), REG_MTXFIFO, (reg))
#define RXFIFO_RD(priv)		reg_read((priv), REG_MRXFIFO)

static void apple_i2c_clear(struct apple_i2c_priv *priv)
{
	unsigned int status;

	status = reg_read(priv, REG_SMSTA);
	reg_write(priv, REG_SMSTA, status);
}

static int apple_i2c_waitready(struct apple_i2c_priv *priv)
{
	int timeout = 10;
	unsigned int status;

	status = reg_read(priv, REG_SMSTA);

	while (!(status & SMSTA_XEN) && timeout--) {
		udelay(1000);
		status = reg_read(priv, REG_SMSTA);
	}

	/* Got NACK? */
	if (status & SMSTA_MTN)
		return -ENXIO;

	if (timeout < 0) {
		reg_write(priv, REG_SMSTA, status);
		return -ETIME;
	}

	/* Clear XEN */
	reg_write(priv, REG_SMSTA, SMSTA_XEN);

	return 0;
}

static int apple_i2c_xfer_msg(struct apple_i2c_priv *priv,
			      struct i2c_msg *msg, int stop)
{
	int read, i, err;
	u32 rd;

	read = msg->flags & I2C_M_RD ? 1 : 0;

	if (read) {
		rd = reg_read(priv, REG_RDCOUNT);
		rd &= ~RDCOUNT_MASK;
		rd |= (msg->len << RDCOUNT_SHIFT);
		reg_write(priv, REG_RDCOUNT, rd);
	}

	TXFIFO_WR(priv, MTXFIFO_START | i2c_8bit_addr_from_msg(msg));

	if (read) {
		TXFIFO_WR(priv, msg->len | MTXFIFO_READ |
				(stop ? MTXFIFO_STOP : 0));

		err = apple_i2c_waitready(priv);
		if (err)
			goto reset_out;

		for (i = 0; i < msg->len; i++) {
			rd = RXFIFO_RD(priv);
			if (rd & MRXFIFO_EMPTY) {
				err = -ENODATA;
				goto reset_out;
			}
			msg->buf[i] = rd & MRXFIFO_DATA_M;
		}
	} else if (msg->len > 0) {
		for (i = 0; i < msg->len - 1; i++)
			TXFIFO_WR(priv, msg->buf[i]);

		TXFIFO_WR(priv, msg->buf[msg->len-1] |
			  (stop ? MTXFIFO_STOP : 0));
	}

	return 0;

 reset_out:
	if (err)
		printf("%s: err %d\n", __func__, err);
	return err;
}

static int apple_i2c_xfer(struct udevice *bus, struct i2c_msg *msg,
			  int nmsgs)
{
	struct apple_i2c_priv *priv = dev_get_priv(bus);
	int ret, i;

	apple_i2c_clear(priv);

	ret = 0;

	for (i = 0; i < nmsgs && !ret; i++)
		ret = apple_i2c_xfer_msg(priv, &msg[i], (i == (nmsgs - 1)));

	return ret ? ret : 0;
}

static const struct dm_i2c_ops apple_i2c_ops = {
	.xfer = apple_i2c_xfer,
};

static int apple_i2c_clk_init(struct udevice *dev,
			      struct apple_i2c_priv *priv)
{
	int ret;

	ret = clk_get_bulk(dev, &priv->clks);
	if (ret == -ENOSYS)
		return 0;
	if (ret)
		return ret;

	ret = clk_enable_bulk(&priv->clks);
	if (ret) {
		clk_release_bulk(&priv->clks);
		return ret;
	}

	return 0;
}

static int apple_i2c_probe(struct udevice *dev)
{
	struct apple_i2c_priv *priv = dev_get_priv(dev);
	int ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	ret = apple_i2c_clk_init(dev, priv);
	if (ret)
		return ret;

	reg_write(priv, REG_CTL, CLK_400K_DIV | CTL_MAGIC);

	return 0;
}

static const struct udevice_id apple_i2c_of_match[] = {
	{ .compatible = "apple,i2c-v0" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_i2c) = {
	.name = "apple-i2c",
	.id = UCLASS_I2C,
	.of_match = apple_i2c_of_match,
	.probe = apple_i2c_probe,
	.priv_auto = sizeof(struct apple_i2c_priv),
	.ops = &apple_i2c_ops,
};
