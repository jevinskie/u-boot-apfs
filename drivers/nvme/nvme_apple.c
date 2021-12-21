// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <common.h>
#include <dm.h>
#include <mailbox.h>
#include <mapmem.h>
#include "nvme.h"
#include <reset.h>

#include <asm/io.h>
#include <asm/arch-apple/rtkit.h>
#include <linux/iopoll.h>

#undef readl_poll_timeout
#define readl_poll_timeout readl_poll_sleep_timeout

#define REG_CPU_CTRL	0x0044
#define  REG_CPU_CTRL_RUN	BIT(4)

#define ANS_BOOT_STATUS		0x1300
#define  ANS_BOOT_STATUS_OK	0xde71ce55

struct apple_nvme_priv {
	struct nvme_dev ndev;
	void *base;
	void *asc;
	struct reset_ctl_bulk resets;
	struct mbox_chan chan;
};

static int apple_nvme_probe(struct udevice *dev)
{
	struct apple_nvme_priv *priv = dev_get_priv(dev);
	fdt_addr_t addr;
	u32 ctrl, stat;
	int ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	addr = dev_read_addr_index(dev, 1);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;
	priv->asc = map_sysmem(addr, 0);

	ret = reset_get_bulk(dev, &priv->resets);
	if (ret < 0)
		return ret;

	ret = mbox_get_by_index(dev, 0, &priv->chan);
	if (ret < 0)
		return ret;

	ctrl = readl(priv->asc + REG_CPU_CTRL);
	writel(ctrl | REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);

	ret = apple_rtkit_init(&priv->chan);
	if (ret < 0)
		return ret;

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

	strcpy(priv->ndev.vendor, "Apple");

	priv->ndev.bar = priv->base;
	return nvme_init(dev);
}

static int apple_nvme_remove(struct udevice *dev)
{
	struct apple_nvme_priv *priv = dev_get_priv(dev);
	u32 ctrl;

	apple_rtkit_shutdown(&priv->chan, APPLE_RTKIT_PWR_STATE_SLEEP);

	ctrl = readl(priv->asc + REG_CPU_CTRL);
	writel(ctrl & ~REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);

	reset_assert_bulk(&priv->resets);
	reset_deassert_bulk(&priv->resets);

	return 0;
}

static const struct udevice_id apple_nvme_ids[] = {
	{ .compatible = "apple,t8103-ans-nvme" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_nvme) = {
	.name = "apple_nvme",
	.id = UCLASS_NVME,
	.of_match = apple_nvme_ids,
	.priv_auto = sizeof(struct apple_nvme_priv),
	.probe = apple_nvme_probe,
	.remove = apple_nvme_remove,
	.flags = DM_FLAG_OS_PREPARE,
};
