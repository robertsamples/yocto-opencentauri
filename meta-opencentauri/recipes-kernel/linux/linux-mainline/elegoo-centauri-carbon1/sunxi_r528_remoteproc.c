// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2026 James Turton <james.turton@gmx.com>
//
// Allwinner R528/D1/T113 HiFi4 DSP remoteproc driver
//
// Based on hardware details from Allwinner's U-Boot DSP loader.
//
// The R528 SoC contains a Cadence/Tensilica HiFi4 DSP core. This driver
// manages its lifecycle (load, start, stop) and provides the kick mechanism
// for RPMsg communication via the MSGBOX mailbox.
//
// Boot sequence (derived from Allwinner U-Boot dsp.c):
//   1. Enable DSP config clock gating
//   2. Deassert DSP config and debug resets
//   3. Set alternate reset vector (entry point from ELF)
//   4. Assert RunStall (hold DSP in reset)
//   5. Enable DSP core clock
//   6. Deassert DSP core reset
//   7. Load ELF segments into memory
//   8. Configure SRAM remap for DSP access
//   9. Deassert RunStall (let DSP execute)
//

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>

#include "remoteproc_internal.h"

/* DSP CFG register offsets (relative to DSP CFG base, e.g. 0x01700000) */
#define DSP_ALT_RESET_VEC_REG	0x0000
#define DSP_CTRL_REG0		    0x0004
#define DSP_PRID_REG		    0x000c
#define DSP_STAT_REG		    0x0010
#define DSP_VER_REG		        0x0020

/* DSP_CTRL_REG0 bits */
#define DSP_CTRL_RUN_STALL      BIT(0)
#define DSP_CTRL_START_VEC_SEL	BIT(1)
#define DSP_CTRL_DSP_CLKEN      BIT(2)

/* DSP_STAT_REG bits */
#define DSP_STAT_PFAULT_INFO_VALID	BIT(0)
#define DSP_STAT_PFAULT_ERROR		BIT(1)
#define DSP_STAT_DOUBLE_EXCE_ERROR	BIT(2)
#define DSP_STAT_XOCD_MODE		    BIT(3)
#define DSP_STAT_DEBUG_MODE		    BIT(4)
#define DSP_STAT_PWAIT_MODE		    BIT(5)
#define DSP_STAT_IRAM0_LOAD_STORE	BIT(6)

/* Default reset vector if no alternate is specified */
#define DSP_DEFAULT_RST_VEC	0x00100000

struct sunxi_r528_rproc_mem {
	void __iomem *cpu_addr;
	phys_addr_t bus_addr;
	u32 da;		/* Device address as seen by the DSP */
	size_t size;
};

struct sunxi_r528_rproc {
	struct device *dev;
	struct rproc *rproc;

	void __iomem *cfg_base;		/* DSP CFG registers */
	void __iomem *sram_base;	/* SRAMC registers (for remap) */

	struct clk *clk_dsp;		/* DSP core clock */
	struct clk *clk_cfg;		/* DSP cfg bus clock */

	struct reset_control *rst_dsp;	/* DSP core reset */
	struct reset_control *rst_cfg;	/* DSP cfg reset */
	struct reset_control *rst_dbg;	/* DSP debug reset */

	struct mbox_client mbox_cl;
	struct mbox_chan *mbox_tx;	/* TX channel: ARM -> DSP kick */
	struct mbox_chan *mbox_rx;	/* RX channel: DSP -> ARM kick */

	struct sunxi_r528_rproc_mem *mem;
	int num_mem;
};

/*
 * SRAM remap control.
 *
 * The R528 has a SRAM remap bit that swaps address ranges between the ARM
 * and DSP views. When loading firmware, the ARM needs direct access to the
 * SRAM (remap=1). After loading, the DSP needs its native view (remap=0).
 */
#define SRAMC_SRAM_REMAP_REG	0x8
#define SRAM_REMAP_ENABLE	BIT(0)

static void sunxi_r528_rproc_sram_remap(struct sunxi_r528_rproc *dsp, bool arm_access)
{
	u32 val;

	if (!dsp->sram_base)
		return;

	val = readl(dsp->sram_base + SRAMC_SRAM_REMAP_REG);
	if (arm_access)
		val |= SRAM_REMAP_ENABLE;
	else
		val &= ~SRAM_REMAP_ENABLE;
	writel(val, dsp->sram_base + SRAMC_SRAM_REMAP_REG);
}

static void sunxi_r528_rproc_set_runstall(struct sunxi_r528_rproc *dsp, bool stall)
{
	u32 val;

	val = readl(dsp->cfg_base + DSP_CTRL_REG0);
	if (stall)
		val |= DSP_CTRL_RUN_STALL;
	else
		val &= ~DSP_CTRL_RUN_STALL;
	writel(val, dsp->cfg_base + DSP_CTRL_REG0);
}

static int sunxi_r528_rproc_mem_alloc(struct rproc *rproc,
				  struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;
	void *va;

	dev_dbg(dev, "map memory: %pa+%zx\n", &mem->dma, mem->len);
	va = ioremap_wc(mem->dma, mem->len);
	if (!va) {
		dev_err(dev, "Unable to map memory region: %pa+%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	mem->va = va;
	return 0;
}

static int sunxi_r528_rproc_mem_release(struct rproc *rproc,
				    struct rproc_mem_entry *mem)
{
	dev_dbg(rproc->dev.parent, "unmap memory: %pa\n", &mem->dma);
	iounmap(mem->va);
	return 0;
}

static int sunxi_r528_rproc_prepare(struct rproc *rproc)
{
	struct sunxi_r528_rproc *dsp = rproc->priv;
	struct device *dev = dsp->dev;
	struct device_node *np = dev->of_node;
	struct of_phandle_iterator it;
	struct rproc_mem_entry *mem;
	struct reserved_mem *rmem;
	int index = 0;

	/*
	 * Register each reserved memory region as a carveout so the
	 * remoteproc ELF loader can place firmware segments.
	 */
	of_phandle_iterator_init(&it, np, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			of_node_put(it.node);
			dev_err(dev, "unable to acquire memory-region\n");
			return -EINVAL;
		}

		/*
		 * Register as a dynamically mapped carveout. The da (device
		 * address) is set to FW_RSC_ADDR_ANY so the ELF loader will
		 * use the physical addresses from the ELF headers directly.
		 * If your DSP firmware uses a resource table with specific
		 * device addresses, you may need to provide proper da values.
		 */
		mem = rproc_mem_entry_init(dev, NULL,
					  (dma_addr_t)rmem->base,
					  rmem->size, rmem->base,
					  sunxi_r528_rproc_mem_alloc,
					  sunxi_r528_rproc_mem_release,
					  it.node->name);
		if (!mem) {
			of_node_put(it.node);
			return -ENOMEM;
		}

		rproc_add_carveout(rproc, mem);
		index++;
	}

	return 0;
}

static int sunxi_r528_rproc_start(struct rproc *rproc)
{
	struct sunxi_r528_rproc *dsp = rproc->priv;
	struct device *dev = dsp->dev;
	u32 val;
	int ret;

	dev_dbg(dev, "starting DSP, entry point 0x%llx\n", rproc->bootaddr);

	/* Enable clocks. */
	ret = clk_prepare_enable(dsp->clk_cfg);
	if (ret) {
		dev_err(dev, "failed to enable cfg clock: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(dsp->clk_dsp);
	if (ret) {
		dev_err(dev, "failed to enable DSP clock: %d\n", ret);
		goto err_disable_cfg_clk;
	}

	/* Deassert config and debug resets. */
	ret = reset_control_deassert(dsp->rst_cfg);
	if (ret) {
		dev_err(dev, "failed to deassert cfg reset: %d\n", ret);
		goto err_disable_dsp_clk;
	}

	ret = reset_control_deassert(dsp->rst_dbg);
	if (ret) {
		dev_err(dev, "failed to deassert dbg reset: %d\n", ret);
		goto err_assert_cfg_rst;
	}

	/* Set alternate reset vector (ELF entry point). */
	if ((u32)rproc->bootaddr != DSP_DEFAULT_RST_VEC) {
		writel(rproc->bootaddr,
		       dsp->cfg_base + DSP_ALT_RESET_VEC_REG);

		val = readl(dsp->cfg_base + DSP_CTRL_REG0);
		val |= DSP_CTRL_START_VEC_SEL;
		writel(val, dsp->cfg_base + DSP_CTRL_REG0);
	}

	/* Hold DSP in stall while we finish setup. */
	sunxi_r528_rproc_set_runstall(dsp, true);

	/* Enable DSP core clock (internal gating bit). */
	val = readl(dsp->cfg_base + DSP_CTRL_REG0);
	val |= DSP_CTRL_DSP_CLKEN;
	writel(val, dsp->cfg_base + DSP_CTRL_REG0);

	/* Deassert DSP core reset. */
	ret = reset_control_deassert(dsp->rst_dsp);
	if (ret) {
		dev_err(dev, "failed to deassert DSP reset: %d\n", ret);
		goto err_assert_dbg_rst;
	}

	/* Switch SRAM remap so DSP has native access. */
	sunxi_r528_rproc_sram_remap(dsp, false);

	/* Release the DSP from stall — it starts executing. */
	sunxi_r528_rproc_set_runstall(dsp, false);

	dev_info(dev, "DSP started, booting from 0x%llx\n", rproc->bootaddr);

	return 0;

err_assert_dbg_rst:
	reset_control_assert(dsp->rst_dbg);
err_assert_cfg_rst:
	reset_control_assert(dsp->rst_cfg);
err_disable_dsp_clk:
	clk_disable_unprepare(dsp->clk_dsp);
err_disable_cfg_clk:
	clk_disable_unprepare(dsp->clk_cfg);

	return ret;
}

static int sunxi_r528_rproc_stop(struct rproc *rproc)
{
	struct sunxi_r528_rproc *dsp = rproc->priv;

	dev_dbg(dsp->dev, "stopping DSP\n");

	/* Stall the DSP. */
	sunxi_r528_rproc_set_runstall(dsp, true);

	/* Assert resets. */
	reset_control_assert(dsp->rst_dsp);
	reset_control_assert(dsp->rst_dbg);
	reset_control_assert(dsp->rst_cfg);

	/* Disable clocks. */
	clk_disable_unprepare(dsp->clk_dsp);
	clk_disable_unprepare(dsp->clk_cfg);

	/* Restore SRAM remap for ARM access. */
	sunxi_r528_rproc_sram_remap(dsp, true);

	return 0;
}

static void sunxi_r528_rproc_kick(struct rproc *rproc, int vqid)
{
	struct sunxi_r528_rproc *dsp = rproc->priv;
	u32 msg = vqid;
	int ret;

	if (!dsp->mbox_tx) {
		dev_err(dsp->dev, "no TX mailbox channel\n");
		return;
	}

	ret = mbox_send_message(dsp->mbox_tx, &msg);
	if (ret < 0)
		dev_err(dsp->dev, "failed to send kick via mailbox: %d\n", ret);
}

/*
 * Translate DSP device addresses to kernel virtual addresses.
 *
 * The HiFi4 DSP on the R528 has an address remap that swaps two ranges:
 *   DSP view 0x10000000-0x1fffffff <-> physical 0x30000000-0x3fffffff
 *   DSP view 0x30000000-0x3fffffff <-> physical 0x10000000-0x1fffffff
 *
 * For addresses outside these ranges, the DSP address equals the physical
 * address. The remoteproc carveouts handle the actual ioremap; this
 * function just needs to match the device address to the right carveout.
 */
static void *sunxi_r528_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len,
				   bool *is_iomem)
{
	struct rproc_mem_entry *carveout;
	phys_addr_t pa;

	/* Apply the DSP address remap to get the physical address. */
	if (da >= 0x10000000 && da < 0x20000000)
		pa = da - 0x10000000 + 0x30000000;
	else if (da >= 0x30000000 && da < 0x40000000)
		pa = da - 0x30000000 + 0x10000000;
	else
		pa = da;

	/*
	 * Walk the carveout list to find which memory region contains this
	 * physical address and return the corresponding virtual address.
	 */
	list_for_each_entry(carveout, &rproc->carveouts, node) {
		if (pa >= carveout->dma &&
		    pa + len <= carveout->dma + carveout->len) {
			void *va = carveout->va + (pa - carveout->dma);

			if (is_iomem)
				*is_iomem = true;
			return va;
		}
	}

	return NULL;
}

/*
 * Mailbox RX callback — the DSP is kicking us.
 * This is called when the DSP sends a message on the MSGBOX channel,
 * typically to signal that a virtqueue has new buffers to process.
 */
static void sunxi_r528_rproc_mbox_rx_cb(struct mbox_client *cl, void *data)
{
	struct rproc *rproc = dev_get_drvdata(cl->dev);
	u32 msg = *(u32 *)data;

	/*
	 * The message value is the virtqueue index. Notify the remoteproc
	 * framework so it can process the virtqueue.
	 */
	if (rproc_vq_interrupt(rproc, msg) == IRQ_NONE)
		dev_dbg(cl->dev, "no handler for vqid %d\n", msg);
}

static void sunxi_r528_rproc_free_mbox(struct sunxi_r528_rproc *dsp)
{
	if (dsp->mbox_tx)
		mbox_free_channel(dsp->mbox_tx);
	if (dsp->mbox_rx)
		mbox_free_channel(dsp->mbox_rx);

	dsp->mbox_tx = NULL;
	dsp->mbox_rx = NULL;
}

static int sunxi_r528_rproc_request_mbox(struct sunxi_r528_rproc *dsp)
{
	struct device *dev = dsp->dev;

	/* Configure the mailbox client. */
	dsp->mbox_cl.dev = dev;
	dsp->mbox_cl.rx_callback = sunxi_r528_rproc_mbox_rx_cb;
	dsp->mbox_cl.tx_done = NULL;
	dsp->mbox_cl.tx_block = true;
	dsp->mbox_cl.tx_tout = 500; /* ms */
	dsp->mbox_cl.knows_txdone = false;

	/*
	 * Request the TX and RX mailbox channels.
	 * These must match the mbox-names in the device tree.
	 */
	dsp->mbox_tx = mbox_request_channel_byname(&dsp->mbox_cl, "tx");
	if (IS_ERR(dsp->mbox_tx)) {
		int ret = PTR_ERR(dsp->mbox_tx);

		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to request TX mbox: %d\n", ret);
		dsp->mbox_tx = NULL;
		return ret;
	}

	dsp->mbox_rx = mbox_request_channel_byname(&dsp->mbox_cl, "rx");
	if (IS_ERR(dsp->mbox_rx)) {
		int ret = PTR_ERR(dsp->mbox_rx);

		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to request RX mbox: %d\n", ret);
		dsp->mbox_rx = NULL;
		sunxi_r528_rproc_free_mbox(dsp);
		return ret;
	}

	return 0;
}

static const struct rproc_ops sunxi_r528_rproc_ops = {
	.prepare	= sunxi_r528_rproc_prepare,
	.start		= sunxi_r528_rproc_start,
	.stop		= sunxi_r528_rproc_stop,
	.kick		= sunxi_r528_rproc_kick,
	.da_to_va	= sunxi_r528_rproc_da_to_va,
	.load		= rproc_elf_load_segments,
	.parse_fw	= rproc_elf_load_rsc_table,
	.find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table,
	.sanity_check	= rproc_elf_sanity_check,
	.get_boot_addr	= rproc_elf_get_boot_addr,
};

static int sunxi_r528_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sunxi_r528_rproc *dsp;
	struct rproc *rproc;
	int ret;

	rproc = rproc_alloc(dev, dev_name(dev), &sunxi_r528_rproc_ops,
			    NULL, sizeof(*dsp));
	if (!rproc)
		return -ENOMEM;

	/* Allow auto-boot via sysfs. */
	rproc->auto_boot = false;

	dsp = rproc->priv;
	dsp->rproc = rproc;
	dsp->dev = dev;

	platform_set_drvdata(pdev, rproc);
	dev_set_drvdata(dev, rproc);

	/* Map DSP CFG registers. */
	dsp->cfg_base = devm_platform_ioremap_resource_byname(pdev, "cfg");
	if (IS_ERR(dsp->cfg_base)) {
		ret = PTR_ERR(dsp->cfg_base);
		dev_err(dev, "failed to map DSP CFG registers: %d\n", ret);
		goto err_free_rproc;
	}

	/* Map SRAMC registers (optional — needed for SRAM remap). */
	dsp->sram_base = devm_platform_ioremap_resource_byname(pdev, "sram");
	if (IS_ERR(dsp->sram_base)) {
		dev_warn(dev, "no SRAM remap registers, skipping remap\n");
		dsp->sram_base = NULL;
	}

	/* Get clocks. */
	dsp->clk_dsp = devm_clk_get(dev, "dsp");
	if (IS_ERR(dsp->clk_dsp)) {
		ret = PTR_ERR(dsp->clk_dsp);
		dev_err(dev, "failed to get DSP clock: %d\n", ret);
		goto err_free_rproc;
	}

	dsp->clk_cfg = devm_clk_get(dev, "cfg");
	if (IS_ERR(dsp->clk_cfg)) {
		ret = PTR_ERR(dsp->clk_cfg);
		dev_err(dev, "failed to get cfg clock: %d\n", ret);
		goto err_free_rproc;
	}

	/* Get resets. */
	dsp->rst_dsp = devm_reset_control_get_exclusive(dev, "dsp");
	if (IS_ERR(dsp->rst_dsp)) {
		ret = PTR_ERR(dsp->rst_dsp);
		dev_err(dev, "failed to get DSP reset: %d\n", ret);
		goto err_free_rproc;
	}

	dsp->rst_cfg = devm_reset_control_get_exclusive(dev, "cfg");
	if (IS_ERR(dsp->rst_cfg)) {
		ret = PTR_ERR(dsp->rst_cfg);
		dev_err(dev, "failed to get cfg reset: %d\n", ret);
		goto err_free_rproc;
	}

	dsp->rst_dbg = devm_reset_control_get_exclusive(dev, "dbg");
	if (IS_ERR(dsp->rst_dbg)) {
		ret = PTR_ERR(dsp->rst_dbg);
		dev_err(dev, "failed to get dbg reset: %d\n", ret);
		goto err_free_rproc;
	}

	/* Request mailbox channels. */
	ret = sunxi_r528_rproc_request_mbox(dsp);
	if (ret)
		goto err_free_rproc;

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "failed to register remoteproc: %d\n", ret);
		goto err_free_mbox;
	}

	dev_info(dev, "Allwinner HiFi4 DSP remoteproc registered\n");

	return 0;

err_free_mbox:
	sunxi_r528_rproc_free_mbox(dsp);
err_free_rproc:
	rproc_free(rproc);
	return ret;
}

static int sunxi_r528_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct sunxi_r528_rproc *dsp = rproc->priv;

	rproc_del(rproc);
	sunxi_r528_rproc_free_mbox(dsp);
	rproc_free(rproc);

	return 0;
}

static const struct of_device_id sunxi_r528_rproc_of_match[] = {
	{ .compatible = "allwinner,sunxi-r528-hifi4-rproc" },
	{},
};
MODULE_DEVICE_TABLE(of, sunxi_r528_rproc_of_match);

static struct platform_driver sunxi_r528_rproc_driver = {
	.driver = {
		.name = "sunxi-r528-hifi4-rproc",
		.of_match_table = sunxi_r528_rproc_of_match,
	},
	.probe  = sunxi_r528_rproc_probe,
	.remove = sunxi_r528_rproc_remove,
};
module_platform_driver(sunxi_r528_rproc_driver);

MODULE_AUTHOR("James Turton <james.turton@gmx.com>");
MODULE_DESCRIPTION("Allwinner R528/D1/T113 HiFi4 DSP remoteproc driver");
MODULE_LICENSE("GPL v2");
