// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <common.h>
#include <dm.h>
#include <mailbox.h>

#include <asm/io.h>
#include <asm/arch-apple/rtkit.h>
#include <asm-generic/gpio.h>

#define SMC_WRITE_KEY		0x11
#define SMC_GET_SRAM_ADDR	0x17

#define SMC_KEY_SHIFT		32
#define SMC_SIZE_SHIFT		16
#define SMC_MSGID_SHIFT		12

#define NUM_MSGIDS	16

struct apple_smc_priv {
	void *base;
	void *buf;
	struct mbox_chan chan;
	int msgid;
};

static void apple_smc_memcpy_toio(void *dst, const void *src, size_t size)
{
	u32 *io = dst;
	const u32 *mem = src;

	while (size > 0) {
		writel(*mem++, io++);
		size -= 4;
	}
}

static void apple_smc_memcpy_fromio(void *dst, void *src, size_t size)
{
	u32 *io = src;
	u32 *mem = dst;

	while (size > 0) {
		*mem++ = readl(io++);
		size -= 4;
	}
}

static int apple_smc_cmd(struct apple_smc_priv *priv, u8 cmd,
			 u32 key, const void *ibuf, size_t ibuflen,
			 void *obuf, size_t obuflen, u64 *omsg)
{
	struct apple_mbox_msg msg;
	u16 size;
	int ret;

	if (ibuf && ibuflen > 0)
		apple_smc_memcpy_toio(priv->buf, ibuf, ibuflen);

	size = max_t(size_t, ibuflen, obuflen);
	msg.msg0 = cmd | ((u64)key << 32) | ((u64)size << 16);
	msg.msg0 |= (priv->msgid << 12);
	msg.msg1 = 32;
	priv->msgid = (priv->msgid + 1) % NUM_MSGIDS;

	ret = mbox_send(&priv->chan, &msg);
	if (ret >= 0) {
		mbox_recv(&priv->chan, &msg, 250000);
		if (obuf && obuflen > 0)
			apple_smc_memcpy_fromio(obuf, priv->buf, obuflen);
		if (omsg) {
			omsg[0] = msg.msg0;
			omsg[1] = msg.msg1;
		}
	}

	return ret;
}

static int apple_smc_write_key(struct apple_smc_priv *priv, u32 key,
			       const void *data, size_t size)
{
	return apple_smc_cmd(priv, SMC_WRITE_KEY, key, data, size,
			     NULL, 0, NULL);
}

static int apple_smc_gpio_get_value(struct udevice *dev, unsigned offset)
{
	printf("%s\n", __func__);
	return -ENOSYS;
}

static int apple_smc_gpio_set_value(struct udevice *dev, unsigned offset,
				    int value)
{
	struct apple_smc_priv *priv = dev_get_priv(dev);
	char *hex = "0123456789abcdef";
	char name[32];
	u32 key, data;
	u32 mask;
	int ret;

	snprintf(name, sizeof(name), "gpio-%d", offset);
	ret = dev_read_u32(dev, name, &mask);
	if (ret < 0)
		return ret;

	key = 0x67500000 | (u32)hex[offset >> 8] << 8 | (u32)hex[offset % 16];
	data = (!!value) | mask;
	return apple_smc_write_key(priv, key, &data, sizeof(data));
}

static int apple_smc_gpio_get_direction(struct udevice *dev, unsigned offset)
{
	printf("%s\n", __func__);
	return -ENOSYS;
}

static int apple_smc_gpio_direction_input(struct udevice *dev, unsigned offset)
{
	printf("%s\n", __func__);
	return -ENOSYS;
}

static int apple_smc_gpio_direction_output(struct udevice *dev,
					   unsigned offset, int value)
{
	return apple_smc_gpio_set_value(dev, offset, value);
}

static struct dm_gpio_ops apple_smc_gpio_ops = {
	.get_value = apple_smc_gpio_get_value,
	.set_value = apple_smc_gpio_set_value,
	.get_function = apple_smc_gpio_get_direction,
	.direction_input = apple_smc_gpio_direction_input,
	.direction_output = apple_smc_gpio_direction_output,
};

static int apple_smc_gpio_probe(struct udevice *dev)
{
	struct apple_smc_priv *priv = dev_get_priv(dev);
	struct gpio_dev_priv *uc_priv;
	u64 msg[2];
	int ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	ret = mbox_get_by_index(dev, 0, &priv->chan);
	if (ret < 0)
		return ret;

	ret = apple_rtkit_init(&priv->chan, 1);
	if (ret < 0)
		return ret;

	ret = apple_smc_cmd(priv, SMC_GET_SRAM_ADDR, 0, NULL, 0,
			    NULL, 0, msg);
	if (ret < 0)
		return ret;

	priv->buf = (void *)msg[0];
	priv->msgid = 0;

	uc_priv = dev_get_uclass_priv(dev);
	uc_priv->bank_name = "gpio";
	uc_priv->gpio_count = 32;

	return 0;
}

static const struct udevice_id apple_smc_ids[] = {
	{ .compatible = "apple,smc-m1" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_smc_gpio) = {
	.name = "apple_smc_gpio",
	.id = UCLASS_GPIO,
	.of_match = apple_smc_ids,
	.priv_auto = sizeof(struct apple_smc_priv),
	.ops = &apple_smc_gpio_ops,
	.probe = apple_smc_gpio_probe,
};
