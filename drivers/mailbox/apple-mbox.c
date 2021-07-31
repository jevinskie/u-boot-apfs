// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <mailbox-uclass.h>
#include <asm/io.h>
#include <linux/delay.h>

#define REG_CPU_CTRL	0x0044
#define  REG_CPU_CTRL_RUN	BIT(4)
#define REG_A2I_STAT	0x8110
#define  REG_A2I_STAT_EMPTY	BIT(17)
#define  REG_A2I_STAT_FULL	BIT(16)
#define REG_I2A_STAT	0x8114
#define  REG_I2A_STAT_EMPTY	BIT(17)
#define  REG_I2A_STAT_FULL	BIT(16)
#define REG_A2I_MSG0	0x8800
#define REG_A2I_MSG1	0x8808
#define REG_I2A_MSG0	0x8830
#define REG_I2A_MSG1	0x8838

struct apple_mbox_priv {
	struct clk_bulk clks;
	void *base;
};

phys_addr_t apple_mbox_phys_start;
phys_addr_t apple_mbox_phys_addr;
phys_size_t apple_mbox_size;

static void apple_mbox_wait(struct apple_mbox_priv *priv)
{
	int retry;

	retry = 500000;
	while (--retry && readl(priv->base + REG_A2I_STAT) & REG_A2I_STAT_FULL)
		udelay(1);
	if (retry == 0)
		printf("%s: A2I timeout\n", __func__);

	retry = 500000;
	while (--retry && readl(priv->base + REG_I2A_STAT) & REG_I2A_STAT_EMPTY)
		udelay(1);
	if (retry == 0)
		printf("%s: I2A timeout\n", __func__);
}

static int apple_mbox_poll(struct apple_mbox_priv *priv)
{
	int retry;

	retry = 500000;
	while (--retry && readl(priv->base + REG_I2A_STAT) & REG_I2A_STAT_EMPTY)
		udelay(1);
	if (retry == 0)
		return -ETIMEDOUT;

	return 0;
}

static int apple_mbox_send(struct mbox_chan *chan, const void *data)
{
	struct apple_mbox_priv *priv = dev_get_priv(chan->dev);
	const u64 *msg = data;

	writeq(msg[0], priv->base + REG_A2I_MSG0);
	writeq((msg[1] & ~0xffULL) | chan->id, priv->base + REG_A2I_MSG1);
	while (readl(priv->base + REG_A2I_STAT) & REG_A2I_STAT_FULL)
		udelay(1);

	return 0;
}

static int apple_mbox_recv(struct mbox_chan *chan, void *data)
{
	struct apple_mbox_priv *priv = dev_get_priv(chan->dev);
	u64 *msg = data;

retry:
	if (readl(priv->base + REG_I2A_STAT) & REG_I2A_STAT_EMPTY)
		return -ENODATA;

	msg[0] = readq(priv->base + REG_I2A_MSG0);
	msg[1] = readq(priv->base + REG_I2A_MSG1);

	if ((msg[1] & 0xffULL) != chan->id)
		goto retry;

	return 0;
}

struct mbox_ops apple_mbox_ops = {
	.send = apple_mbox_send,
	.recv = apple_mbox_recv,
};

static int apple_mbox_clk_init(struct udevice *dev,
			       struct apple_mbox_priv *priv)
{
	int ret;

	ret = clk_get_bulk(dev, &priv->clks);
	if (ret == -ENOSYS || ret == -ENOENT)
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

static int apple_mbox_probe(struct udevice *dev)
{
	struct apple_mbox_priv *priv = dev_get_priv(dev);
	u64 msg[2];
	int endpoint;
	int endpoints[256];
	int nendpoints = 0;
	int msgtype;
	u64 subtype;
	u32 cpu_ctrl;
	int i, ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	ret = apple_mbox_clk_init(dev, priv);
	if (ret)
		return ret;

	if (apple_mbox_phys_start == 0) {
		apple_mbox_phys_start = (phys_addr_t)memalign(SZ_512K, SZ_64K);
		apple_mbox_phys_addr = apple_mbox_phys_start;
		apple_mbox_size = SZ_512K;
	}

	/* EP0_IDLE */
	cpu_ctrl = readl(priv->base + REG_CPU_CTRL);
	if (cpu_ctrl & REG_CPU_CTRL_RUN) {
		writeq(0x0060000000000220, priv->base + REG_A2I_MSG0);
		writeq(0x0000000000000000, priv->base + REG_A2I_MSG1);
	} else {
		writel(cpu_ctrl | REG_CPU_CTRL_RUN, priv->base + REG_CPU_CTRL);
	}

	/* EP0_WAIT_HELLO */
	apple_mbox_wait(priv);
	msg[0] = readq(priv->base + REG_I2A_MSG0);
	msg[1] = readq(priv->base + REG_I2A_MSG1);

	endpoint = msg[1] & 0xff;
	msgtype = (msg[0] >> 52) & 0xff;
	if (endpoint != 0) {
		printf("%s: unexpected endpoint %d\n", __func__, endpoint);
		return -EINVAL;
	}
	if (msgtype != 1) {
		printf("%s: unexpected message type %d\n", __func__, msgtype);
		return -EINVAL;
	}

	/* EP0_SEND_HELLO */
	subtype = msg[0] & 0xffffffff;
	writeq(0x0020000100000000 | subtype , priv->base + REG_A2I_MSG0);
	writeq(0x0000000000000000, priv->base + REG_A2I_MSG1);

wait_epmap:
	/* EP0_WAIT_EPMAP */
	apple_mbox_wait(priv);
	msg[0] = readq(priv->base + REG_I2A_MSG0);
	msg[1] = readq(priv->base + REG_I2A_MSG1);

	endpoint = msg[1] & 0xff;
	msgtype = (msg[0] >> 52) & 0xff;
	if (endpoint != 0) {
		printf("%s: unexpected endpoint %d\n", __func__, endpoint);
		return -EINVAL;
	}
	if (msgtype != 8) {
		printf("%s: unexpected message type %d\n", __func__, msgtype);
		return -EINVAL;
	}

	for (i = 0; i < 32; i++) {
		if (msg[0] & (1U << i)) {
			endpoint = i + 32 * ((msg[0] >> 32) & 7);
			endpoints[nendpoints++] = endpoint;
		}
	}

	/* EP0_SEND_EPACK */
	subtype = (msg[0] >> 32) & 0x80007;
	writeq(0x0080000000000000 | (subtype << 32) | !(subtype & 7), priv->base + REG_A2I_MSG0);
	writeq(0x0000000000000000, priv->base + REG_A2I_MSG1);

	if ((subtype & 0x80000) == 0)
		goto wait_epmap;

	for (i = 0; i < nendpoints; i++) {
		while (readl(priv->base + REG_A2I_STAT) & REG_A2I_STAT_FULL)
			udelay(1);

		/* EP0_SEND_EPSTART */
		subtype = endpoints[i];
		writeq(0x0050000000000002 | (subtype << 32), priv->base + REG_A2I_MSG0);
		writeq(0x0000000000000000, priv->base + REG_A2I_MSG1);
	}

	while (apple_mbox_poll(priv) == 0) {
		/* EP0_WAIT_PWROK */
		apple_mbox_wait(priv);
		msg[0] = readq(priv->base + REG_I2A_MSG0);
		msg[1] = readq(priv->base + REG_I2A_MSG1);

		endpoint = msg[1] & 0xff;
		msgtype = (msg[0] >> 52) & 0xff;

		if (endpoint == 0 && msgtype == 11)
			continue;

		if (endpoint == 1 || endpoint == 2 || endpoint == 4) {
			u64 size = (msg[0] >> 44) & 0xff;

			if (apple_mbox_phys_addr + (size << 12) >
			    apple_mbox_phys_start + apple_mbox_size) {
				printf("%s: out of memory\n", __func__);
				return -ENOMEM;
			}

			msg[0] &= ~0xfffffffffff;
			msg[0] |= apple_mbox_phys_addr;
			writeq(msg[0], priv->base + REG_A2I_MSG0);
			writeq(msg[1], priv->base + REG_A2I_MSG1);
			apple_mbox_phys_addr += (size << 12);
			continue;
		}
		if (endpoint != 0) {
			printf("%s: unexpected endpoint %d\n", __func__, endpoint);
			return -EINVAL;
		}
		if (msgtype != 7) {
			printf("%s: unexpected message type %d\n", __func__, msgtype);
			return -EINVAL;
		}

		/* EP0_SEND_PWRACK */
		subtype = msg[0] & 0xffffffff;
		writeq(0x00b0000000000000 | subtype , priv->base + REG_A2I_MSG0);
		writeq(0x0000000000000000, priv->base + REG_A2I_MSG1);
	}

	return 0;
}

static const struct udevice_id apple_mbox_of_match[] = {
	{ .compatible = "apple,iop-mailbox-m1" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_mbox) = {
	.name = "apple-mbox",
	.id = UCLASS_MAILBOX,
	.of_match = apple_mbox_of_match,
	.probe = apple_mbox_probe,
	.priv_auto = sizeof(struct apple_mbox_priv),
	.ops = &apple_mbox_ops,
};
