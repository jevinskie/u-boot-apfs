// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <mailbox.h>
#include <mapmem.h>
#include "nvme.h"

#include <asm/io.h>
#include <linux/iopoll.h>

#undef readl_poll_timeout
#define readl_poll_timeout readl_poll_sleep_timeout

extern phys_addr_t apple_mbox_phys_start;
extern phys_addr_t apple_mbox_phys_addr;

#define ANS_BOOT_STATUS		0x1300
#define  ANS_BOOT_STATUS_OK	0xde71ce55

#define ANS_SART_SIZE(id)	(0x0000 + 4 * (id))
#define ANS_SART_ADDR(id)	(0x0040 + 4 * (id))

struct apple_nvme_priv {
	struct nvme_dev ndev;
	struct clk_bulk clks;
	void *base;
	void *sart;
	struct mbox_chan chan;
};

static int apple_nvme_clk_init(struct udevice *dev,
			       struct apple_nvme_priv *priv)
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

static int apple_nvme_probe(struct udevice *dev)
{
	struct apple_nvme_priv *priv = dev_get_priv(dev);
	fdt_addr_t addr;
	phys_addr_t mbox_addr;
	phys_size_t mbox_size;
	u32 stat;
	int id, ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	addr = dev_read_addr_index(dev, 1);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;
	priv->sart = map_sysmem(addr, 0);

	ret = apple_nvme_clk_init(dev, priv);
	if (ret)
		return ret;

	mbox_addr = apple_mbox_phys_addr;
	ret = mbox_get_by_index(dev, 0, &priv->chan);
	if (ret < 0)
		return ret;
	if (mbox_addr == 0)
		mbox_addr = apple_mbox_phys_start;
	mbox_size = apple_mbox_phys_addr - mbox_addr;

	for (id = 0; id < 16; id++) {
		if ((readl(priv->sart + ANS_SART_SIZE(id)) & 0xff000000) == 0)
			break;
	}
	if (id == 16) {
		printf("%s: no free SART registers\n", __func__);
		return -ENOMEM;
	}

	writel(mbox_addr >> 12, priv->sart + ANS_SART_ADDR(id));
	writel(0xff000000 | (mbox_size >> 12), priv->sart + ANS_SART_SIZE(id));

	ret = readl_poll_timeout(priv->base + ANS_BOOT_STATUS, stat,
				 (stat == ANS_BOOT_STATUS_OK), 100, 500000);
	if (ret < 0) {
		printf("%s: NVMe firmware didn't boot\n", __func__);
		return -ETIMEDOUT;
	}

	writel(((64 << 16) | 64), priv->base + 0x1210);

	writel(readl(priv->base + 0x24004) | 0x1000, priv->base + 0x24004);
	writel(1, priv->base + 0x24908);
	writel(readl(priv->base + 0x24008) & ~0x800, priv->base + 0x24008);

	writel(0x102, priv->base + 0x24118);
	writel(0x102, priv->base + 0x24108);
	writel(0x102, priv->base + 0x24420);
	writel(0x102, priv->base + 0x24414);
	writel(0x10002, priv->base + 0x2441c);
	writel(0x10002, priv->base + 0x24418);
	writel(0x10002, priv->base + 0x24144);
	writel(0x10002, priv->base + 0x24524);
	writel(0x102, priv->base + 0x24508);
	writel(0x10002, priv->base + 0x24504);

	priv->ndev.bar = priv->base;
	return nvme_init(dev);
}

static const struct udevice_id apple_nvme_ids[] = {
	{ .compatible = "apple,nvme-m1" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_nvme) = {
	.name = "apple_nvme",
	.id = UCLASS_NVME,
	.of_match = apple_nvme_ids,
	.priv_auto = sizeof(struct apple_nvme_priv),
	.probe = apple_nvme_probe,
};
