// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <common.h>
#include <clk.h>
#include <cpu_func.h>
#include <dm.h>
#include <mapmem.h>
#include <asm/io.h>

#define DART_PARAMS2		0x0004
#define  DART_PARAMS2_BYPASS_SUPPORT	BIT(0)
#define DART_TLB_OP		0x0020
#define  DART_TLB_OP_OPMASK	(0xfff << 20)
#define  DART_TLB_OP_FLUSH	(0x001 << 20)
#define  DART_TLB_OP_BUSY	BIT(2)
#define DART_TLB_OP_SIDMASK	0x0034
#define DART_ERROR_STATUS	0x0040
#define DART_TCR(sid)		(0x0100 + 4 * (sid))
#define  DART_TCR_TRANSLATE_ENABLE	BIT(7)
#define  DART_TCR_BYPASS_DART		BIT(8)
#define  DART_TCR_BYPASS_DAPF		BIT(12)
#define DART_TTBR(sid, idx)	(0x0200 + 16 * (sid) + 4 * (idx))
#define  DART_TTBR_VALID	BIT(31)
#define  DART_TTBR_SHIFT	12

struct apple_dart_priv {
	struct clk_bulk clks;
	void *base;
};

dma_addr_t apple_dart_bus_start;
phys_addr_t apple_dart_phys_start;
phys_size_t apple_dart_size = SZ_512M;

static void apple_dart_flush_tlb(struct apple_dart_priv *priv)
{
	u32 status;

	writel(0xffffffff, priv->base + DART_TLB_OP_SIDMASK);
	writel(DART_TLB_OP_FLUSH, priv->base + DART_TLB_OP);

	for (;;) {
		status = readl(priv->base + DART_TLB_OP);
		if ((status & DART_TLB_OP_OPMASK) == 0)
			break;
		if ((status & DART_TLB_OP_BUSY) == 0)
			break;
	}
}

static int apple_dart_clk_init(struct udevice *dev,
			       struct apple_dart_priv *priv)
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

static int apple_dart_bind(struct udevice *dev)
{
	void *base;
	int sid, i;

	base = dev_read_addr_ptr(dev);
	if (!base)
		return -EINVAL;

	u32 params2 = readl(base + DART_PARAMS2);
	if (params2 & DART_PARAMS2_BYPASS_SUPPORT) {
		for (sid = 0; sid < 16; sid++) {
			writel(DART_TCR_BYPASS_DART | DART_TCR_BYPASS_DAPF,
			       base + DART_TCR(sid));
			for (i = 0; i < 4; i++)
				writel(0, base + DART_TTBR(sid, i));
		}
	}

	return 0;
}

static int apple_dart_probe(struct udevice *dev)
{
	struct apple_dart_priv *priv = dev_get_priv(dev);
	phys_addr_t phys;
	u64 *l1, *l2;
	int sid, i, j;
	int ret;

	apple_dart_phys_start = gd->ram_top - apple_dart_size;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	ret = apple_dart_clk_init(dev, priv);
	if (ret)
		return ret;

	l1 = memalign(SZ_64K, SZ_64K);
	memset(l1, 0, SZ_64K);

	i = 0;
	phys = apple_dart_phys_start;
	while (phys < apple_dart_phys_start + apple_dart_size) {
		l2 = memalign(SZ_16K, SZ_16K);
		memset(l2, 0, SZ_16K);

		for (j = 0; j < 2048; j++) {
			l2[j] = phys | 0x3;
			phys += SZ_16K;
		}
		flush_dcache_range((unsigned long)l2,
				   (unsigned long)l2 + SZ_16K);

		l1[i++] = (phys_addr_t)l2 | 0x8 | 0x3;
	}

	flush_dcache_range((unsigned long)l1, (unsigned long)l1 + SZ_64K);

	for (sid = 0; sid < 16; sid++) {
		for (i = 0; i < 4; i++)
			writel(0, priv->base + DART_TTBR(sid, i));
	}

	apple_dart_flush_tlb(priv);

	for (sid = 0; sid < 16; sid++) {
		phys = (phys_addr_t)l1;
		for (i = 0; i < 4; i++) {
			writel((phys >> DART_TTBR_SHIFT) | DART_TTBR_VALID,
			       priv->base + DART_TTBR(sid, i));
			phys += SZ_16K;
		}
	}

	apple_dart_flush_tlb(priv);

	for (sid = 0; sid < 16; sid++)
		writel(DART_TCR_TRANSLATE_ENABLE, priv->base + DART_TCR(sid));
	
	return 0;
}

static const struct udevice_id apple_dart_ids[] = {
	{ .compatible = "apple,t8103-dart" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_dart) = {
	.name = "apple_dart",
	.id = UCLASS_MISC,
	.of_match = apple_dart_ids,
	.priv_auto = sizeof(struct apple_dart_priv),
	.bind = apple_dart_bind,
	.probe = apple_dart_probe
};

#ifdef DEBUG

phys_addr_t dart_base[] = {
	0x681008000,
	0x682008000,
	0x683008000,
};

void
apple_dart_error(void)
{
	void *base;
	int i;

	for (i = 0; i < 3; i++) {
		base = (void *)dart_base[i];
		printf("ERROR_STATUS 0x%x\n", readl(base + 0x0040));
		printf("ERROR_ADDRESS_LO 0x%x\n", readl(base + 0x0050));
		printf("ERROR_ADDRESS_HI 0x%x\n", readl(base + 0x0054));
	}
}

#endif
