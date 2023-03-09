// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/amba/bus.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/coresight.h>
#include <linux/coresight-pmu.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include "coresight-priv.h"
#include "coresight-tpdm.h"

DEFINE_CORESIGHT_DEVLIST(tpdm_devs, "tpdm");

static umode_t tpdm_dsb_is_visible(struct kobject *kobj,
							struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	if (drvdata) {
		if (drvdata->datasets & TPDM_PIDR0_DS_DSB)
			return attr->mode;
		else
			return 0;
	}

	return 0;
}

static umode_t tpdm_cmb_is_visible(struct kobject *kobj,
							struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	if (drvdata) {
		if (drvdata->datasets & TPDM_PIDR0_DS_CMB)
			return attr->mode;
		else
			return 0;
	}

	return 0;
}

static umode_t tpdm_tc_is_visible(struct kobject *kobj,
							struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	if (drvdata) {
		if (drvdata->datasets & TPDM_PIDR0_DS_TC)
			return attr->mode;
		else
			return 0;
	}

	return 0;
}

static void tpdm_enable_dsb(struct tpdm_drvdata *drvdata)
{
	u32 val, mode, i;

	for (i = 0; i < TPDM_DSB_MAX_EDCR; i++)
		writel_relaxed(drvdata->dsb->edge_ctrl[i],
			   drvdata->base + TPDM_DSB_EDCR(i));
	for (i = 0; i < TPDM_DSB_MAX_EDCMR; i++)
		writel_relaxed(drvdata->dsb->edge_ctrl_mask[i],
			   drvdata->base + TPDM_DSB_EDCMR(i));

	for (i = 0; i < TPDM_DSB_MAX_PATT; i++) {
		writel_relaxed(drvdata->dsb->patt_val[i],
			    drvdata->base + TPDM_DSB_TPR(i));
		writel_relaxed(drvdata->dsb->patt_mask[i],
			    drvdata->base + TPDM_DSB_TPMR(i));
	}

	for (i = 0; i < TPDM_DSB_MAX_PATT; i++) {
		writel_relaxed(drvdata->dsb->trig_patt_val[i],
			    drvdata->base + TPDM_DSB_XPR(i));
		writel_relaxed(drvdata->dsb->trig_patt_mask[i],
			    drvdata->base + TPDM_DSB_XPMR(i));
	}

	val = readl_relaxed(drvdata->base + TPDM_DSB_TIER);
	/* Set pattern timestamp type and enablement */
	if (drvdata->dsb->patt_ts) {
		val |= TPDM_DSB_PATT_TSENAB;
		if (drvdata->dsb->patt_type)
			val |= TPDM_DSB_PATT_TYPE;
		else
			val &= ~TPDM_DSB_PATT_TYPE;
	} else {
		val &= ~TPDM_DSB_PATT_TSENAB;
	}
	/* Set trigger timestamp */
	if (drvdata->dsb->trig_ts)
		val |= TPDM_DSB_XTRIG_TSENAB;
	else
		val &= ~TPDM_DSB_XTRIG_TSENAB;
	writel_relaxed(val, drvdata->base + TPDM_DSB_TIER);

	val = readl_relaxed(drvdata->base + TPDM_DSB_CR);
	/* Set trigger type */
	if (drvdata->dsb->trig_type)
		val |= TPDM_DSB_TRIG_TYPE;
	else
		val &= ~TPDM_DSB_TRIG_TYPE;
	writel_relaxed(val, drvdata->base + TPDM_DSB_CR);

	/* Set the enable bit of DSB control register to 1 */
	val = readl_relaxed(drvdata->base + TPDM_DSB_CR);
	/* Set the cycle accurate mode */
	mode = TPDM_DSB_MODE_CYCACC(drvdata->dsb->mode);
	val &= ~TPDM_DSB_TEST_MODE;
	val |= FIELD_PREP(TPDM_DSB_TEST_MODE, mode);
	/* Set the byte lane for high-performance mode */
	mode = TPDM_DSB_MODE_HPBYTESEL(drvdata->dsb->mode);
	val &= ~TPDM_DSB_HPSEL;
	val |= FIELD_PREP(TPDM_DSB_HPSEL, mode);
	/* Set the performance mode */
	if (drvdata->dsb->mode & TPDM_DSB_MODE_PERF)
		val |= TPDM_DSB_MODE;
	else
		val &= ~TPDM_DSB_MODE;
	val |= TPDM_DSB_CR_ENA;
	writel_relaxed(val, drvdata->base + TPDM_DSB_CR);
}

static void tpdm_enable_cmb(struct tpdm_drvdata *drvdata)
{
	u32 val;
	int i;

	/* Configure pattern registers*/
	for (i = 0; i < TPDM_CMB_MAX_PATT; i++) {
		writel_relaxed(drvdata->cmb->patt_val[i],
			    drvdata->base + TPDM_CMB_TPR(i));
		writel_relaxed(drvdata->cmb->patt_mask[i],
			    drvdata->base + TPDM_CMB_TPMR(i));
		writel_relaxed(drvdata->cmb->trig_patt_val[i],
			    drvdata->base + TPDM_CMB_XPR(i));
		writel_relaxed(drvdata->cmb->trig_patt_mask[i],
			    drvdata->base + TPDM_CMB_XPMR(i));
	}

	val = readl_relaxed(drvdata->base + TPDM_CMB_TIER);
	if (drvdata->cmb->patt_ts)
		val = val | TPDM_CMB_TIER_PATT_TSENAB;
	else
		val = val & ~TPDM_CMB_TIER_PATT_TSENAB;
	if (drvdata->cmb->trig_ts)
		val = val | TPDM_CMB_TIER_XTRIG_TSENAB;
	else
		val = val & ~TPDM_CMB_TIER_XTRIG_TSENAB;
	if (drvdata->cmb->ts_all)
		val = val | TPDM_CMB_TIER_TS_ALL;
	else
		val = val & ~TPDM_CMB_TIER_TS_ALL;
	writel_relaxed(val, drvdata->base + TPDM_CMB_TIER);


	val = readl_relaxed(drvdata->base + TPDM_CMB_CR);
	/*
	 * Set to 0 for continuous CMB collection mode,
	 * 1 for trace-on-change CMB collection mode.
	 */
	if (drvdata->cmb->trace_mode)
		val |= TPDM_CMB_CR_MODE;
	else
		val &= ~TPDM_CMB_CR_MODE;

	/* Set the enable bit of CMB control register to 1 */
	val |= TPDM_CMB_CR_ENA;

	writel_relaxed(val, drvdata->base + TPDM_CMB_CR);
}

static void tpdm_enable_tc(struct tpdm_drvdata *drvdata)
{
	u32 val;
	int i;

	/*
	 * Each bit of TPDM_TC_CNTENCLR and TPDM_TC_CNTENSET
	 * corresponds to a supported tenure counter.
	 * Unsupported counters ignore writes and read as zeros.
	 */
	if (drvdata->tc->enable_counters) {
		writel_relaxed(0xF, drvdata->base + TPDM_TC_CNTENCLR);
		writel_relaxed(drvdata->tc->enable_counters,
						drvdata->base + TPDM_TC_CNTENSET);
	}

	if (drvdata->tc->clear_counters)
		writel_relaxed(drvdata->tc->clear_counters,
						drvdata->base + TPDM_TC_CNTENCLR);

	/*
	 * Configure count interrupt enable set/clear register.
	 * bit 0 - 3 : A write of 1 to bit N enables/clears all general purpose
	 * and TAT computation counters within tenure counter N to generate
	 * an IRQ upon saturation or rollover.
	 * bit 4: A write of 1 to this bit enables/clears all supported tenure
	 * scratchpad counters to generate an IRQ upon saturation.
	 *
	 * A write of 0 to any bit is ignored.
	 */
	if (drvdata->tc->enable_irq) {
		writel_relaxed(0xF,  drvdata->base + TPDM_TC_INTENCLR);
		writel_relaxed(drvdata->tc->enable_irq,
			    drvdata->base + TPDM_TC_INTENSET);
	}

	if (drvdata->tc->clear_irq)
		writel_relaxed(drvdata->tc->clear_irq,
			    drvdata->base + TPDM_TC_INTENCLR);

	if (drvdata->tc->tc_trig_type == TPDM_SUPPORT_TYPE_FULL) {
		for (i = 0; i < TPDM_TC_MAX_TRIG; i++) {
			writel_relaxed(drvdata->tc->trig_sel[i],
				    drvdata->base + TPDM_TC_TRIG_SEL(i));
			writel_relaxed(drvdata->tc->trig_val_lo[i],
				    drvdata->base + TPDM_TC_TRIG_LO(i));
			writel_relaxed(drvdata->tc->trig_val_hi[i],
				    drvdata->base + TPDM_TC_TRIG_HI(i));
		}
	} else if (drvdata->tc->tc_trig_type == TPDM_SUPPORT_TYPE_PARTIAL) {
		writel_relaxed(drvdata->tc->trig_sel[0],
			    drvdata->base + TPDM_TC_TRIG_SEL(0));
		writel_relaxed(drvdata->tc->trig_val_lo[0],
			    drvdata->base + TPDM_TC_TRIG_LO(0));
		writel_relaxed(drvdata->tc->trig_val_hi[0],
			    drvdata->base + TPDM_TC_TRIG_HI(0));
	}

	val = readl_relaxed(drvdata->base + TPDM_TC_CR);
	/* 
	 * APB retrieval is enabled via a setting of 1,
	 * else data sets are transmitted over ATB
	 */
	if (drvdata->tc->retrieval_mode == TPDM_MODE_APB)
		val = val | TPDM_TC_CR_RETRIEVAL_MODE;
	else
		val = val & ~TPDM_TC_CR_RETRIEVAL_MODE;

	/*
	 * A setting of 0 indicates all supported tenure GP(general pupose)
	 * counters and TAT(Total Accumulated Tenure) metric logic are
	 * configured to saturate. A setting of 1 indicates all supported
	 * tenure GP counters and TAT metric logic are configured to rollover.
	 */
	if (drvdata->tc->sat_mode)
		val = val | TPDM_TC_CR_SO;
	else
		val = val & ~TPDM_TC_CR_SO;

	/* Set the enable bit of TC control register to 1 */
	val |= TPDM_TC_CR_ENA;

	writel_relaxed(val, drvdata->base + TPDM_TC_CR);
}

/* TPDM enable operations
 * The TPDM or Monitor serves as data collection component for various
 * dataset types. It covers Basic Counts(BC), Tenure Counts(TC),
 * Continuous Multi-Bit(CMB), Multi-lane CMB(MCMB) and Discrete Single
 * Bit(DSB). This function will initialize the configuration according
 * to the dataset type supported by the TPDM.
 */
static void __tpdm_enable(struct tpdm_drvdata *drvdata)
{
	CS_UNLOCK(drvdata->base);

	/* Enable dataset packet */
	if (drvdata->datasets & TPDM_PIDR0_DS_DSB)
		tpdm_enable_dsb(drvdata);
	if (drvdata->datasets & TPDM_PIDR0_DS_CMB)
		tpdm_enable_cmb(drvdata);
	if (drvdata->datasets & TPDM_PIDR0_DS_TC)
		tpdm_enable_tc(drvdata);

	CS_LOCK(drvdata->base);
}

static int tpdm_enable(struct coresight_device *csdev,
		       struct perf_event *event, u32 mode)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	spin_lock(&drvdata->spinlock);
	if (drvdata->enable) {
		spin_unlock(&drvdata->spinlock);
		return -EBUSY;
	}

	__tpdm_enable(drvdata);
	drvdata->enable = true;
	spin_unlock(&drvdata->spinlock);

	dev_dbg(drvdata->dev, "TPDM tracing enabled\n");
	return 0;
}

static void tpdm_disable_dsb(struct tpdm_drvdata *drvdata)
{
	u32 val;

	/* Set the enable bit of DSB control register to 0 */
	val = readl_relaxed(drvdata->base + TPDM_DSB_CR);
	val &= ~TPDM_DSB_CR_ENA;
	writel_relaxed(val, drvdata->base + TPDM_DSB_CR);
}

static void tpdm_disable_cmb(struct tpdm_drvdata *drvdata)
{
	u32 val;

	val = readl_relaxed(drvdata->base + TPDM_CMB_CR);
	val &= ~TPDM_CMB_CR_ENA;

	/* Set the enable bit of CMB control register to 0 */
	writel_relaxed(val, drvdata->base + TPDM_CMB_CR);
}

static void tpdm_disable_tc(struct tpdm_drvdata *drvdata)
{
	u32 val;

	val = readl_relaxed(drvdata->base + TPDM_TC_CR);
	val &= ~TPDM_TC_CR_ENA;

	/* Set the enable bit of TC control register to 0 */
	writel_relaxed(val, drvdata->base + TPDM_TC_CR);
}

/* TPDM disable operations */
static void __tpdm_disable(struct tpdm_drvdata *drvdata)
{
	CS_UNLOCK(drvdata->base);

	/* Disable dataset packet */
	if (drvdata->datasets & TPDM_PIDR0_DS_DSB)
		tpdm_disable_dsb(drvdata);
	if (drvdata->datasets & TPDM_PIDR0_DS_CMB)
		tpdm_disable_cmb(drvdata);
	if (drvdata->datasets & TPDM_PIDR0_DS_TC)
		tpdm_disable_tc(drvdata);

	CS_LOCK(drvdata->base);
}

static void tpdm_disable(struct coresight_device *csdev,
			 struct perf_event *event)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	spin_lock(&drvdata->spinlock);
	if (!drvdata->enable) {
		spin_unlock(&drvdata->spinlock);
		return;
	}

	__tpdm_disable(drvdata);
	drvdata->enable = false;
	spin_unlock(&drvdata->spinlock);

	dev_dbg(drvdata->dev, "TPDM tracing disabled\n");
}

static const struct coresight_ops_source tpdm_source_ops = {
	.enable		= tpdm_enable,
	.disable	= tpdm_disable,
};

static const struct coresight_ops tpdm_cs_ops = {
	.source_ops	= &tpdm_source_ops,
};

static void tpdm_datasets_setup(struct tpdm_drvdata *drvdata)
{
	u32 pidr;

	/*  Get the datasets present on the TPDM. */
	pidr = readl_relaxed(drvdata->base + CORESIGHT_PERIPHIDR0);
	drvdata->datasets |= pidr & GENMASK(TPDM_DATASETS - 1, 0);
}

static int tpdm_datasets_alloc(struct tpdm_drvdata *drvdata)
{
	if (drvdata->datasets & TPDM_PIDR0_DS_DSB) {
		drvdata->dsb = devm_kzalloc(drvdata->dev, sizeof(*drvdata->dsb),
					    GFP_KERNEL);
		if (!drvdata->dsb)
			return -ENOMEM;
	}

	if (drvdata->datasets & TPDM_PIDR0_DS_CMB) {
		drvdata->cmb = devm_kzalloc(drvdata->dev, sizeof(*drvdata->cmb),
					    GFP_KERNEL);
		if (!drvdata->cmb)
			return -ENOMEM;
	}

	if (drvdata->datasets & TPDM_PIDR0_DS_TC) {
		drvdata->tc = devm_kzalloc(drvdata->dev, sizeof(*drvdata->tc),
					    GFP_KERNEL);
		if (!drvdata->tc)
			return -ENOMEM;
	}

	return 0;
}

static void tpdm_init_default_data(struct tpdm_drvdata *drvdata)
{
	u32 devid;

	devid = readl_relaxed(drvdata->base + CORESIGHT_DEVID);

	if (drvdata->datasets & TPDM_PIDR0_DS_DSB) {
		drvdata->dsb->trig_ts = true;
		drvdata->dsb->trig_type = false;
	}

	if (drvdata->datasets & TPDM_PIDR0_DS_TC) {
		drvdata->tc->retrieval_mode = TPDM_MODE_ATB;
		/*
		 * This field is set to a binary value that is number of
		 * TC counters supported minus 1.
		 */
		drvdata->tc->tc_counters_avail = FIELD_GET(TPDM_DEVID_TC_COUNTERS, devid) + 1;
		drvdata->tc->tc_trig_type = FIELD_GET(TPDM_DEVID_TC_LVL_TRIG, devid);
	}
}

static ssize_t reset_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf,
					  size_t size)
{
	int ret = 0;
	unsigned long val;
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	ret = kstrtoul(buf, 0, &val);
	if (ret || (val != 1))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	/* Reset all datasets to ZERO */
	if (drvdata->dsb != NULL)
		memset(drvdata->dsb, 0, sizeof(struct dsb_dataset));

	/* Init the default data */
	tpdm_init_default_data(drvdata);

	spin_unlock(&drvdata->spinlock);

	/* Disable tpdm if enabled */
	if (drvdata->enable)
		coresight_disable(drvdata->csdev);

	return size;
}
static DEVICE_ATTR_WO(reset);

/*
 * value 1: 64 bits test data
 * value 2: 32 bits test data
 */
static ssize_t integration_test_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf,
					  size_t size)
{
	int i, ret = 0;
	unsigned long val;
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	if (val != 1 && val != 2)
		return -EINVAL;

	if (!drvdata->enable)
		return -EINVAL;

	if (val == 1)
		val = ATBCNTRL_VAL_64;
	else
		val = ATBCNTRL_VAL_32;
	CS_UNLOCK(drvdata->base);
	writel_relaxed(0x1, drvdata->base + TPDM_ITCNTRL);

	for (i = 0; i < INTEGRATION_TEST_CYCLE; i++)
		writel_relaxed(val, drvdata->base + TPDM_ITATBCNTRL);

	writel_relaxed(0, drvdata->base + TPDM_ITCNTRL);
	CS_LOCK(drvdata->base);
	return size;
}
static DEVICE_ATTR_WO(integration_test);

static struct attribute *tpdm_attrs[] = {
	&dev_attr_reset.attr,
	&dev_attr_integration_test.attr,
	NULL,
};

static struct attribute_group tpdm_attr_grp = {
	.attrs = tpdm_attrs,
};

static ssize_t dsb_mode_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return sysfs_emit(buf, "%lx\n",
			 (unsigned long)drvdata->dsb->mode);
}

static ssize_t dsb_mode_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf,
				   size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if ((kstrtoul(buf, 0, &val)) || val < 0)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->dsb->mode = val & TPDM_MODE_ALL;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_mode);

static ssize_t dsb_edge_ctrl_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_EDCR; i++) {
		size += sysfs_emit_at(buf, size,
				  "Index:0x%x Val:0x%x\n", i,
				  drvdata->dsb->edge_ctrl[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}

/*
 * value 1: Start EDCR register number
 * value 2: End EDCR register number
 * value 3: The value need to be written
 * The EDCR registers can include up to 16 32-bit registers, and each
 * one can be configured to control up to 16 edge detections(2 bits
 * control one edge detection). So a total 256 edge detections can be
 * configured. So the starting number(value 1) and ending number(value 2)
 * cannot be greater than 256, and value 1 should be less than value 2.
 * The following values are the rage of value 3.
 * 0 - Rising edge detection
 * 1 - Falling edge detection
 * 2 - Rising and falling edge detection (toggle detection)
 */
static ssize_t dsb_edge_ctrl_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long start, end, edge_ctrl;
	uint32_t val;
	int i, index, bit, reg;

	if (sscanf(buf, "%lx %lx %lx", &start, &end, &edge_ctrl) != 3)
		return -EINVAL;
	if ((start >= TPDM_DSB_MAX_LINES) || (end >= TPDM_DSB_MAX_LINES) ||
	    edge_ctrl > 0x2)
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	for (i = start; i <= end; i++) {
		/*
		 * The 32-bit register has 32 bits(NUM_OF_BITS).
		 * Each one register can be configured to control 16
		 * (NUM_OF_BITS / 2) edge detectioins.
		 */
		reg = i / (NUM_OF_BITS / 2);
		index = i % (NUM_OF_BITS / 2);
		bit = index * 2;

		val = drvdata->dsb->edge_ctrl[reg];
		val &= ~GENMASK((bit + 1), bit);
		val |= (edge_ctrl << bit);
		drvdata->dsb->edge_ctrl[reg] = val;
	}
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_RW(dsb_edge_ctrl);

static ssize_t dsb_edge_ctrl_mask_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_EDCR / 2; i++) {
		size += sysfs_emit_at(buf, size,
				  "Index:0x%x Val:0x%x\n", i,
				  drvdata->dsb->edge_ctrl_mask[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}

/*
 * value 1: Start EDCMR register number
 * value 2: End EDCMR register number
 * value 3: The value need to be written
 */
static ssize_t dsb_edge_ctrl_mask_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf,
					     size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long start, end, val;
	u32 set;
	int i, index, reg;

	if (sscanf(buf, "%lx %lx %lx", &start, &end, &val) != 3)
		return -EINVAL;
	if ((start >= TPDM_DSB_MAX_LINES) || (end >= TPDM_DSB_MAX_LINES)
		|| (val < 0) || (val > 1))
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	for (i = start; i <= end; i++) {
		/*
		 * The 32-bit register has 32 bits(NUM_OF_BITS).
		 * Each one register can be configured to control 32
		 * (NUM_OF_BITS) edge detectioin masks.
		 */
		reg = i / NUM_OF_BITS;
		index = (i % NUM_OF_BITS);

		set = drvdata->dsb->edge_ctrl_mask[reg];
		if (val)
			set |= BIT(index);
		else
			set &= ~BIT(index);
		drvdata->dsb->edge_ctrl_mask[reg] = set;
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_edge_ctrl_mask);

static ssize_t dsb_patt_val_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_PATT; i++) {
		size += sysfs_emit_at(buf, size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->dsb->patt_val[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}

/*
 * value 1: Index of TPR register
 * value 2: Value need to be written
 */
static ssize_t dsb_patt_val_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf,
				       size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_DSB_MAX_PATT)
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	drvdata->dsb->patt_val[index] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_patt_val);

static ssize_t dsb_patt_mask_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_PATT; i++) {
		size += sysfs_emit_at(buf, size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->dsb->patt_mask[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}

/*
 * value 1: Index of TPMR register
 * value 2: Value need to be written
 */
static ssize_t dsb_patt_mask_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_DSB_MAX_PATT)
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	drvdata->dsb->patt_mask[index] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_patt_mask);

static ssize_t dsb_patt_ts_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return sysfs_emit(buf, "%u\n",
			 (unsigned int)drvdata->dsb->patt_ts);
}

/*
 * value 1: Enable/Disable DSB pattern timestamp
 */
static ssize_t dsb_patt_ts_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;
	if (!(drvdata->datasets & TPDM_PIDR0_DS_DSB))
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	if (val)
		drvdata->dsb->patt_ts = true;
	else
		drvdata->dsb->patt_ts = false;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_patt_ts);

static ssize_t dsb_patt_type_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);


	return sysfs_emit(buf, "%u\n",
			 (unsigned int)drvdata->dsb->patt_type);
}

/*
 * value 1: Set DSB pattern type
 */
static ssize_t dsb_patt_type_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (val)
		drvdata->dsb->patt_type = true;
	else
		drvdata->dsb->patt_type = false;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_patt_type);

static ssize_t dsb_trig_patt_val_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_PATT; i++) {
		size += sysfs_emit_at(buf, size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->dsb->trig_patt_val[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}

static ssize_t dsb_trig_patt_val_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf,
					    size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_DSB_MAX_PATT)
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	drvdata->dsb->trig_patt_val[index] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_trig_patt_val);

static ssize_t dsb_trig_patt_mask_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_PATT; i++) {
		size += sysfs_emit_at(buf, size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->dsb->trig_patt_mask[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}

static ssize_t dsb_trig_patt_mask_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf,
					     size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_DSB_MAX_PATT)
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	drvdata->dsb->trig_patt_mask[index] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_trig_patt_mask);

static ssize_t dsb_trig_type_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return sysfs_emit(buf, "%u\n",
			 (unsigned int)drvdata->dsb->trig_type);
}

/*
 * value 0: set trigger type as enablement
 * value 1: set trigger type as disablement
 */
static ssize_t dsb_trig_type_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if ((kstrtoul(buf, 0, &val)) || val < 0 || val > 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (val)
		drvdata->dsb->trig_type = true;
	else
		drvdata->dsb->trig_type = false;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_trig_type);

static ssize_t dsb_trig_ts_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return sysfs_emit(buf, "%u\n",
			 (unsigned int)drvdata->dsb->trig_ts);
}

/*
 * value 0: set trigger timestamp as enablement
 * value 1: set trigger timestamp as disablement
 */
static ssize_t dsb_trig_ts_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if ((kstrtoul(buf, 0, &val)) || val < 0 || val > 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (val)
		drvdata->dsb->trig_ts = true;
	else
		drvdata->dsb->trig_ts = false;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_trig_ts);

static ssize_t cmb_mode_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "trace_mode: %s \n",
			 drvdata->cmb->trace_mode ?
			 "trace_on_change" : "continuous");
}

static ssize_t cmb_mode_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf,
				   size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int trace_mode;
	int nval;

	nval = sscanf(buf, "%u", &trace_mode);
	if (nval != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->cmb->trace_mode = trace_mode;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(cmb_mode);

static ssize_t cmb_patt_val_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_CMB_MAX_PATT; i++) {
		size += scnprintf(buf + size, PAGE_SIZE - size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->cmb->patt_val[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}

static ssize_t cmb_patt_val_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_CMB_MAX_PATT)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->cmb->patt_val[index] = val;
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_RW(cmb_patt_val);

static ssize_t cmb_patt_mask_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_CMB_MAX_PATT; i++) {
		size += scnprintf(buf + size, PAGE_SIZE - size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->cmb->patt_mask[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;

}

static ssize_t cmb_patt_mask_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_CMB_MAX_PATT)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->cmb->patt_mask[index] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(cmb_patt_mask);

static ssize_t cmb_trig_patt_val_show(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_CMB_MAX_PATT; i++) {
		size += scnprintf(buf + size, PAGE_SIZE - size,
				"Index: 0x%x Value: 0x%x\n", i,
				drvdata->cmb->trig_patt_val[i]);
	}
	spin_unlock(&drvdata->spinlock);

	return size;
}

static ssize_t cmb_trig_patt_val_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_CMB_MAX_PATT)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->cmb->trig_patt_val[index] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(cmb_trig_patt_val);

static ssize_t cmb_trig_patt_mask_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_CMB_MAX_PATT; i++) {
		size += scnprintf(buf + size, PAGE_SIZE - size,
				"Index: 0x%x Value: 0x%x\n", i,
				drvdata->cmb->trig_patt_mask[i]);
	}
	spin_unlock(&drvdata->spinlock);

	return size;
}

static ssize_t cmb_trig_patt_mask_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_CMB_MAX_PATT)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->cmb->trig_patt_mask[index] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(cmb_trig_patt_mask);

static ssize_t cmb_patt_ts_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);


	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 (unsigned int)drvdata->cmb->patt_ts);
}

static ssize_t cmb_patt_ts_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (val)
		drvdata->cmb->patt_ts = true;
	else
		drvdata->cmb->patt_ts = false;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(cmb_patt_ts);

static ssize_t cmb_ts_all_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);


	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 (unsigned int)drvdata->cmb->ts_all);
}

static ssize_t cmb_ts_all_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (val)
		drvdata->cmb->ts_all = true;
	else
		drvdata->cmb->ts_all = false;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(cmb_ts_all);

static ssize_t cmb_trig_ts_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 (unsigned int)drvdata->cmb->trig_ts);
}

static ssize_t cmb_trig_ts_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (val)
		drvdata->cmb->trig_ts = true;
	else
		drvdata->cmb->trig_ts = false;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(cmb_trig_ts);

static ssize_t tc_retrieval_mode_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);


	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 drvdata->tc->retrieval_mode == TPDM_MODE_ATB ?
			 "ATB" : "APB");
}

static ssize_t tc_retrieval_mode_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf,
					    size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	char str[20] = "";

	if (size >= 20)
		return -EINVAL;
	if (sscanf(buf, "%s", str) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (drvdata->enable) {
		spin_unlock(&drvdata->spinlock);
		return -EPERM;
	}

	if (!strcmp(str, "ATB")) {
		drvdata->tc->retrieval_mode = TPDM_MODE_ATB;
	} else if (!strcmp(str, "APB")) {
		drvdata->tc->retrieval_mode = TPDM_MODE_APB;
	} else {
		spin_unlock(&drvdata->spinlock);
		return -EINVAL;
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(tc_retrieval_mode);

static ssize_t tc_capture_mode_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 drvdata->tc->capture_mode == TPDM_MODE_ATB ?
			 "ATB" : "APB");
}

static ssize_t tc_capture_mode_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf,
					  size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	char str[20] = "";
	u32 val;

	if (size >= 20)
		return -EINVAL;
	if (sscanf(buf, "%s", str) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	/* APB access is only possible when the TC subunit is enabled. */
	if (!drvdata->enable) {
		spin_unlock(&drvdata->spinlock);
		return -EPERM;
	}

	if (!strcmp(str, "ATB")) {
		drvdata->tc->capture_mode = TPDM_MODE_ATB;
	} else if (!strcmp(str, "APB") &&
		   drvdata->tc->retrieval_mode == TPDM_MODE_APB) {
		drvdata->tc->capture_mode = TPDM_MODE_APB;
		CS_UNLOCK(drvdata->base);
		val = readl_relaxed(drvdata->base + TPDM_TC_CR);
		val = val | TPDM_TC_CR_CAPTURE;
		writel_relaxed(val, drvdata->base + TPDM_TC_CR);
		CS_LOCK(drvdata->base);
	} else {
		spin_unlock(&drvdata->spinlock);
		return -EINVAL;
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(tc_capture_mode);

static ssize_t tc_sat_mode_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 (unsigned int)drvdata->tc->sat_mode);
}

static ssize_t tc_sat_mode_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (val)
		drvdata->tc->sat_mode = true;
	else
		drvdata->tc->sat_mode = false;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(tc_sat_mode);

static ssize_t tc_enable_counters_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%lx\n",
			 (unsigned long)drvdata->tc->enable_counters);
}

static ssize_t tc_enable_counters_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf,
					     size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;
	if (val >> drvdata->tc->tc_counters_avail)
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	drvdata->tc->enable_counters = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(tc_enable_counters);

static ssize_t tc_clear_counters_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%lx\n",
			 (unsigned long)drvdata->tc->clear_counters);
}

static ssize_t tc_clear_counters_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf,
					    size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;
	if (val >> drvdata->tc->tc_counters_avail)
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	drvdata->tc->clear_counters = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(tc_clear_counters);

static ssize_t tc_enable_irq_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%lx\n",
			 (unsigned long)drvdata->tc->enable_irq);
}

static ssize_t tc_enable_irq_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->tc->enable_irq = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(tc_enable_irq);

static ssize_t tc_clear_irq_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%lx\n",
			 (unsigned long)drvdata->tc->clear_irq);
}

static ssize_t tc_clear_irq_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf,
				       size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->tc->clear_irq = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(tc_clear_irq);

static ssize_t tc_trig_sel_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_TC_MAX_TRIG; i++) {
		size += scnprintf(buf + size, PAGE_SIZE - size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->tc->trig_sel[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}

static ssize_t tc_trig_sel_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_TC_MAX_TRIG ||
	    drvdata->tc->tc_trig_type == TPDM_SUPPORT_TYPE_NO ||
	    (drvdata->tc->tc_trig_type == TPDM_SUPPORT_TYPE_PARTIAL && index > 0))
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	drvdata->tc->trig_sel[index] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(tc_trig_sel);

static ssize_t tc_trig_val_lo_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_TC_MAX_TRIG; i++) {
		size += scnprintf(buf + size, PAGE_SIZE - size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->tc->trig_val_lo[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}

static ssize_t tc_trig_val_lo_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf,
					 size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_TC_MAX_TRIG ||
	    drvdata->tc->tc_trig_type == TPDM_SUPPORT_TYPE_NO ||
	    (drvdata->tc->tc_trig_type == TPDM_SUPPORT_TYPE_PARTIAL && index > 0))
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	drvdata->tc->trig_val_lo[index] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(tc_trig_val_lo);

static ssize_t tc_trig_val_hi_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	ssize_t size = 0;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_TC_MAX_TRIG; i++) {
		size += scnprintf(buf + size, PAGE_SIZE - size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->tc->trig_val_hi[i]);
	}
	spin_unlock(&drvdata->spinlock);
	return size;
}

static ssize_t tc_trig_val_hi_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf,
					 size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long index, val;

	if (sscanf(buf, "%lx %lx", &index, &val) != 2)
		return -EINVAL;
	if (index >= TPDM_TC_MAX_TRIG ||
	    drvdata->tc->tc_trig_type == TPDM_SUPPORT_TYPE_NO ||
	    (drvdata->tc->tc_trig_type == TPDM_SUPPORT_TYPE_PARTIAL && index > 0))
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	drvdata->tc->trig_val_hi[index] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(tc_trig_val_hi);

static struct attribute *tpdm_dsb_attrs[] = {
	&dev_attr_dsb_mode.attr,
	&dev_attr_dsb_edge_ctrl.attr,
	&dev_attr_dsb_edge_ctrl_mask.attr,
	&dev_attr_dsb_patt_val.attr,
	&dev_attr_dsb_patt_mask.attr,
	&dev_attr_dsb_patt_ts.attr,
	&dev_attr_dsb_patt_type.attr,
	&dev_attr_dsb_trig_patt_val.attr,
	&dev_attr_dsb_trig_patt_mask.attr,
	&dev_attr_dsb_trig_ts.attr,
	&dev_attr_dsb_trig_type.attr,
	NULL,
};

static struct attribute *tpdm_cmb_attrs[] = {
	&dev_attr_cmb_mode.attr,
	&dev_attr_cmb_patt_val.attr,
	&dev_attr_cmb_patt_mask.attr,
	&dev_attr_cmb_trig_patt_val.attr,
	&dev_attr_cmb_trig_patt_mask.attr,
	&dev_attr_cmb_patt_ts.attr,
	&dev_attr_cmb_ts_all.attr,
	&dev_attr_cmb_trig_ts.attr,
	NULL,
};

static struct attribute *tpdm_tc_attrs[] = {
	&dev_attr_tc_retrieval_mode.attr,
	&dev_attr_tc_capture_mode.attr,
	&dev_attr_tc_sat_mode.attr,
	&dev_attr_tc_enable_counters.attr,
	&dev_attr_tc_clear_counters.attr,
	&dev_attr_tc_enable_irq.attr,
	&dev_attr_tc_clear_irq.attr,
	&dev_attr_tc_trig_sel.attr,
	&dev_attr_tc_trig_val_lo.attr,
	&dev_attr_tc_trig_val_hi.attr,
	NULL,
};

static struct attribute_group tpdm_dsb_attr_grp = {
	.attrs = tpdm_dsb_attrs,
	.is_visible = tpdm_dsb_is_visible,
};

static struct attribute_group tpdm_cmb_attr_grp = {
	.attrs = tpdm_cmb_attrs,
	.is_visible = tpdm_cmb_is_visible,
};

static struct attribute_group tpdm_tc_attr_grp = {
	.attrs = tpdm_tc_attrs,
	.is_visible = tpdm_tc_is_visible,
};

static const struct attribute_group *tpdm_attr_grps[] = {
	&tpdm_attr_grp,
	&tpdm_dsb_attr_grp,
	&tpdm_cmb_attr_grp,
	&tpdm_tc_attr_grp,
	NULL,
};

static int tpdm_probe(struct amba_device *adev, const struct amba_id *id)
{
	void __iomem *base;
	struct device *dev = &adev->dev;
	struct coresight_platform_data *pdata;
	struct tpdm_drvdata *drvdata;
	struct coresight_desc desc = { 0 };
	int ret;

	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	adev->dev.platform_data = pdata;

	/* driver data*/
	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;
	drvdata->dev = &adev->dev;
	dev_set_drvdata(dev, drvdata);

	base = devm_ioremap_resource(dev, &adev->res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	drvdata->base = base;

	tpdm_datasets_setup(drvdata);

	/* Set up coresight component description */
	desc.name = coresight_alloc_device_name(&tpdm_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	desc.type = CORESIGHT_DEV_TYPE_SOURCE;
	desc.subtype.source_subtype = CORESIGHT_DEV_SUBTYPE_SOURCE_OTHERS;
	desc.ops = &tpdm_cs_ops;
	desc.pdata = adev->dev.platform_data;
	desc.dev = &adev->dev;
	desc.access = CSDEV_ACCESS_IOMEM(base);
	desc.groups = tpdm_attr_grps;
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev))
		return PTR_ERR(drvdata->csdev);

	spin_lock_init(&drvdata->spinlock);
	ret = tpdm_datasets_alloc(drvdata);
	if (ret) {
		coresight_unregister(drvdata->csdev);
		return ret;
	}
	tpdm_init_default_data(drvdata);

	/* Decrease pm refcount when probe is done.*/
	pm_runtime_put(&adev->dev);

	return 0;
}

static void tpdm_remove(struct amba_device *adev)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(&adev->dev);

	coresight_unregister(drvdata->csdev);
}

/*
 * Different TPDM has different periph id.
 * The difference is 0-7 bits' value. So ignore 0-7 bits.
 */
static struct amba_id tpdm_ids[] = {
	{
		.id = 0x000f0e00,
		.mask = 0x000fff00,
	},
	{ 0, 0},
};

static struct amba_driver tpdm_driver = {
	.drv = {
		.name   = "coresight-tpdm",
		.owner	= THIS_MODULE,
		.suppress_bind_attrs = true,
	},
	.probe          = tpdm_probe,
	.id_table	= tpdm_ids,
	.remove		= tpdm_remove,
};

module_amba_driver(tpdm_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Trace, Profiling & Diagnostic Monitor driver");
