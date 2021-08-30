// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (C) 2021 Corellium LLC
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <mapmem.h>
#include <pci.h>
#include <asm/io.h>
#include <asm-generic/gpio.h>
#include <linux/delay.h>
#include <linux/iopoll.h>

#undef readl_poll_timeout
#define readl_poll_timeout readl_poll_sleep_timeout

#define usleep_range(a, b) udelay((b))

extern dma_addr_t apple_dart_bus_start;
extern phys_addr_t apple_dart_phys_start;
extern phys_size_t apple_dart_size;

#define CORE_RC_PHYIF_CTL		0x00024
#define   CORE_RC_PHYIF_CTL_RUN		BIT(0)
#define CORE_RC_PHYIF_STAT		0x00028
#define   CORE_RC_PHYIF_STAT_REFCLK	BIT(4)
#define CORE_RC_CTL			0x00050
#define   CORE_RC_CTL_RUN		BIT(0)
#define CORE_RC_STAT			0x00058
#define   CORE_RC_STAT_READY		BIT(0)
#define CORE_PHY_CTL			0x80000
#define   CORE_PHY_CTL_CLK0REQ		BIT(0)
#define   CORE_PHY_CTL_CLK1REQ		BIT(1)
#define   CORE_PHY_CTL_CLK0ACK		BIT(2)
#define   CORE_PHY_CTL_CLK1ACK		BIT(3)
#define   CORE_PHY_CTL_RESET		BIT(7)
#define CORE_LANE_CFG(port)		(0x84000 + 0x4000 * (port))
#define   CORE_LANE_CFG_REFCLK0REQ	BIT(0)
#define   CORE_LANE_CFG_REFCLK1		BIT(1)
#define   CORE_LANE_CFG_REFCLK0ACK	BIT(2)
#define   CORE_LANE_CFG_REFCLKEN	(BIT(9) | BIT(10))
#define CORE_LANE_CTL(port)		(0x84004 + 0x4000 * (port))
#define   CORE_LANE_CTL_CFGACC		BIT(15)

#define PORT_LTSSMCTL			0x00080
#define   PORT_LTSSMCTL_START		BIT(0)
#define PORT_INTSTAT			0x00100
#define   PORT_INT_CPL_TIMEOUT		BIT(23)
#define   PORT_INT_RID2SID_MAPERR	BIT(22)
#define   PORT_INT_CPL_ABORT		BIT(21)
#define   PORT_INT_MSI_BAD_DATA		BIT(19)
#define   PORT_INT_MSI_ERR		BIT(18)
#define   PORT_INT_REQADDR_GT32		BIT(17)
#define   PORT_INT_AF_TIMEOUT		BIT(15)
#define   PORT_INT_LINK_DOWN		BIT(14)
#define   PORT_INT_LINK_UP		BIT(12)
#define   PORT_INT_LINK_BWMGMT		BIT(11)
#define   PORT_INT_AER_MASK		(15 << 4)
#define   PORT_INT_PORT_ERR		BIT(4)
#define PORT_INTMSKSET			0x00104
#define PORT_INTMSKCLR			0x00108
#define PORT_MSICFG			0x00124
#define   PORT_MSICFG_EN		BIT(0)
#define   PORT_MSICFG_L2MSINUM_SHIFT	4
#define PORT_MSIBASE			0x00128
#define   PORT_MSIBASE_1_SHIFT		16
#define PORT_MSIADDR			0x00168
#define PORT_LINKSTS			0x00208
#define   PORT_LINKSTS_UP		BIT(0)
#define   PORT_LINKSTS_BUSY		BIT(2)
#define PORT_LINKCMDSTS			0x00210
#define PORT_APPCLK			0x00800
#define   PORT_APPCLK_EN		BIT(0)
#define   PORT_APPCLK_CGDIS		BIT(8)
#define PORT_STATUS			0x00804
#define   PORT_STATUS_READY		BIT(0)
#define PORT_REFCLK			0x00810
#define   PORT_REFCLK_EN		BIT(0)
#define   PORT_REFCLK_CGDIS		BIT(8)
#define PORT_PERST			0x00814
#define   PORT_PERST_OFF		BIT(0)
#define PORT_RID2SID(i16)		(0x00828 + 4 * (i16))
#define   PORT_RID2SID_VALID		BIT(31)
#define   PORT_RID2SID_SID_SHIFT	16
#define   PORT_RID2SID_BUS_SHIFT	8
#define   PORT_RID2SID_DEV_SHIFT	3
#define   PORT_RID2SID_FUNC_SHIFT	0

#define NUM_PORTS	3

struct apple_pcie_priv {
	struct udevice *dev;
	struct clk_bulk clks;
	void *base_config;
	void *base_rc;
	void *base_port[NUM_PORTS];
	struct gpio_desc pwren[NUM_PORTS];
	struct gpio_desc perstn[NUM_PORTS];
};

static inline void rmwl(u32 clr, u32 set, void *addr)
{
	writel((readl(addr) & ~clr) | set, addr);
}

static inline void rmww(u16 clr, u16 set, volatile void __iomem *addr)
{
	writew((readw(addr) & ~clr) | set, addr);
}

static int apple_pcie_config_address(const struct udevice *bus,
				     pci_dev_t bdf, uint offset,
				     void **paddress)
{
	struct apple_pcie_priv *priv = dev_get_priv(bus);
	void *addr;

	if (PCI_BUS(bdf) > 15)
		return -ENODEV;

	addr = priv->base_config;
	addr += PCI_BUS(bdf) << 20;
	addr += PCI_DEV(bdf) << 15;
	addr += PCI_FUNC(bdf) << 12;
	addr += offset;
	*paddress = addr;

	return 0;
}

static int apple_pcie_read_config(const struct udevice *bus, pci_dev_t bdf,
				  uint offset, ulong *valuep,
				  enum pci_size_t size)
{
	int ret;
	ret = pci_generic_mmap_read_config(bus, apple_pcie_config_address,
					   bdf, offset, valuep, size);
	return ret;
}

static int apple_pcie_write_config(struct udevice *bus, pci_dev_t bdf,
				   uint offset, ulong value,
				   enum pci_size_t size)
{
	return pci_generic_mmap_write_config(bus, apple_pcie_config_address,
					     bdf, offset, value, size);
}

static const struct dm_pci_ops apple_pcie_ops = {
	.read_config = apple_pcie_read_config,
	.write_config = apple_pcie_write_config,
};

static int apple_pcie_link_up(struct apple_pcie_priv *pcie, unsigned idx)
{
	uint32_t linksts = readl(pcie->base_port[idx] + PORT_LINKSTS);
	return !!(linksts & PORT_LINKSTS_UP);
}

static void apple_pcie_port_pwren(struct apple_pcie_priv *pcie, unsigned idx)
{
	if (pcie->pwren[idx].dev == NULL)
		return;

	dm_gpio_set_dir_flags(&pcie->pwren[idx], GPIOD_IS_OUT);
	dm_gpio_set_value(&pcie->pwren[idx], 1);
	usleep_range(2500, 5000);
}

static int apple_pcie_setup_refclk(struct apple_pcie_priv *pcie, unsigned idx)
{
	u32 stat;
	int res;

	res = readl_poll_timeout(pcie->base_rc + CORE_RC_PHYIF_STAT, stat, (stat & CORE_RC_PHYIF_STAT_REFCLK), 100, 50000);
	if (res < 0) {
		printf("%s: core refclk wait timed out.\n", __func__);
		return res;
	}

	rmwl(0, CORE_LANE_CTL_CFGACC, pcie->base_rc + CORE_LANE_CTL(idx));
	rmwl(0, CORE_LANE_CFG_REFCLK0REQ, pcie->base_rc + CORE_LANE_CFG(idx));
	res = readl_poll_timeout(pcie->base_rc + CORE_LANE_CFG(idx), stat, (stat & CORE_LANE_CFG_REFCLK0ACK), 100, 50000);
	if (res < 0) {
		printf("%s: lane refclk%d wait timed out (port %d).\n", __func__, 0, idx);
		return res;
	}
	rmwl(0, CORE_LANE_CFG_REFCLK1, pcie->base_rc + CORE_LANE_CFG(idx));
	res = readl_poll_timeout(pcie->base_rc + CORE_LANE_CFG(idx), stat, (stat & CORE_LANE_CFG_REFCLK1), 100, 50000);
	if(res < 0) {
		printf("%s: lane refclk%d wait timed out (port %d).\n", __func__, 1, idx);
		return res;
	}
	rmwl(CORE_LANE_CTL_CFGACC, 0, pcie->base_rc + CORE_LANE_CTL(idx));
	udelay(1);
	rmwl(0, CORE_LANE_CFG_REFCLKEN, pcie->base_rc + CORE_LANE_CFG(idx));

	rmwl(0, PORT_REFCLK_EN, pcie->base_port[idx] + PORT_REFCLK);

	return 0;
}

static int apple_pcie_setup_port(struct apple_pcie_priv *pcie, unsigned idx)
{
	u32 stat;
	int res;

	if (apple_pcie_link_up(pcie, idx))
		return 0;

	dm_gpio_set_dir_flags(&pcie->perstn[idx], GPIOD_IS_OUT);
	dm_gpio_set_value(&pcie->perstn[idx], 0);

	rmwl(0, PORT_APPCLK_EN, pcie->base_port[idx] + PORT_APPCLK);

	res = apple_pcie_setup_refclk(pcie, idx);
	if(res < 0)
		return res;

	apple_pcie_port_pwren(pcie, idx);

	rmwl(0, PORT_PERST_OFF, pcie->base_port[idx] + PORT_PERST);
	dm_gpio_set_value(&pcie->perstn[idx], 1);

	res = readl_poll_timeout(pcie->base_port[idx] + PORT_STATUS, stat, (stat & PORT_STATUS_READY), 100, 250000);
	if (res < 0) {
		printf("%s: status ready wait timed out (port %d).\n", __func__, idx);
		return res;
	}

	rmwl(PORT_REFCLK_CGDIS, 0, pcie->base_port[idx] + PORT_REFCLK);
	rmwl(PORT_APPCLK_CGDIS, 0, pcie->base_port[idx] + PORT_APPCLK);

	res = readl_poll_timeout(pcie->base_port[idx] + PORT_LINKSTS, stat, !(stat & PORT_LINKSTS_BUSY), 100, 250000);
	if(res < 0) {
		printf("%s: link not busy wait timed out (port %d).\n", __func__, idx);
		return res;
	}

	writel(0xfb512fff, pcie->base_port[idx] + PORT_INTMSKSET);
	writel(0x04aed000, pcie->base_port[idx] + PORT_INTSTAT);

	usleep_range(5000, 10000);

	rmwl(0, PORT_LTSSMCTL_START, pcie->base_port[idx] + PORT_LTSSMCTL);
	res = readl_poll_timeout(pcie->base_port[idx] + PORT_LINKSTS, stat, (stat & PORT_LINKSTS_UP), 100, 500000);
	if (res < 0)
		printf("%s: link up wait timed out (port %d).\n", __func__, idx);

	return 0;
}

static int apple_pcie_setup_ports(struct apple_pcie_priv *pcie)
{
	unsigned port;
	ofnode node;

	node = ofnode_first_subnode(dev_ofnode(pcie->dev));
	for (port = 0; port < NUM_PORTS; port++) {
		if (!ofnode_valid(node))
			break;
		apple_pcie_setup_port(pcie, port);
		node = ofnode_next_subnode(node);
	}

	return 0;
}

static int apple_pcie_clk_init(struct apple_pcie_priv *priv)
{
	int ret;

	ret = clk_get_bulk(priv->dev, &priv->clks);
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

static int apple_pcie_probe(struct udevice *dev)
{
	struct apple_pcie_priv *priv = dev_get_priv(dev);
	struct pci_controller *hose = dev_get_uclass_priv(dev);
	struct udevice *dart;
	fdt_addr_t addr;
	u32 phandle;
	ofnode node;
	int i, len, ret;

	priv->dev = dev;
	priv->base_config = dev_read_addr_ptr(dev);
	if (!priv->base_config)
		return -EINVAL;

	addr = dev_read_addr_index(dev, 1);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;
	priv->base_rc = map_sysmem(addr, 0);

	for (i = 0; i < NUM_PORTS; i++) {
		addr = dev_read_addr_index(dev, 2 + i);
		if (addr == FDT_ADDR_T_NONE)
			return -EINVAL;
		priv->base_port[i] = map_sysmem(addr, 0);
	}

	node = ofnode_first_subnode(dev_ofnode(dev));
	for (i = 0; i < NUM_PORTS; i++) {
		if (!ofnode_valid(node))
			break;
		ret = gpio_request_by_name_nodev(node, "pwren-gpios", 0,
						 &priv->pwren[i], 0);
		if (ret < 0 && ret != -ENOENT) {
			printf("failed to get PWREN%d gpio: %d\n", i, ret);
			return ret;
		}
		ret = gpio_request_by_name_nodev(node, "reset-gpios", 0,
						 &priv->perstn[i], 0);
		if (ret < 0) {
			printf("failed to get PERST#%d gpio: %d\n", i, ret);
			return ret;
		}
		node = ofnode_next_subnode(node);
	}

	ret = apple_pcie_clk_init(priv);
	if (ret)
		return ret;

	apple_pcie_setup_ports(priv);

	len = dev_read_size(dev, "iommu-map");
	for (i = 0; i < len / 16; i++) {
		ret = dev_read_u32_index(dev, "iommu-map", i * 4 + 1,
					 &phandle);
		if (ret < 0)
			return ret;

		ret = uclass_get_device_by_phandle_id(UCLASS_MISC, phandle,
						      &dart);
		if (ret < 0)
			return ret;
	}

	for (i = 0; i < hose->region_count; i++) {
		if (hose->regions[i].flags != PCI_REGION_SYS_MEMORY)
			continue;

		hose->regions[i].bus_start = apple_dart_bus_start;
		hose->regions[i].phys_start = apple_dart_phys_start;
		hose->regions[i].size = apple_dart_size;
	}

	return 0;
}

static const struct udevice_id apple_pcie_of_match[] = {
	{ .compatible = "apple,pcie" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_pcie) = {
	.name = "apple_pcie",
	.id = UCLASS_PCI,
	.of_match = apple_pcie_of_match,
	.probe = apple_pcie_probe,
	.priv_auto = sizeof(struct apple_pcie_priv),
	.ops = &apple_pcie_ops,
};
