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

	if (drvdata && (drvdata->datasets & TPDM_PIDR0_DS_DSB))
		return attr->mode;

	return 0;
}

static void tpdm_reset_datasets(struct tpdm_drvdata *drvdata)
{
	if (drvdata->datasets & TPDM_PIDR0_DS_DSB) {
		memset(drvdata->dsb, 0, sizeof(struct dsb_dataset));

		drvdata->dsb->trig_ts = true;
		drvdata->dsb->trig_type = false;
	}
}

static void set_dsb_test_mode(struct tpdm_drvdata *drvdata, u32 *val)
{
	u32 mode;

	mode = TPDM_DSB_MODE_TEST(drvdata->dsb->mode);
	*val &= ~TPDM_DSB_TEST_MODE;
	*val |= FIELD_PREP(TPDM_DSB_TEST_MODE, mode);
}

static void set_dsb_hpsel_mode(struct tpdm_drvdata *drvdata, u32 *val)
{
	u32 mode;

	mode = TPDM_DSB_MODE_HPBYTESEL(drvdata->dsb->mode);
	*val &= ~TPDM_DSB_HPSEL;
	*val |= FIELD_PREP(TPDM_DSB_HPSEL, mode);
}

static void set_dsb_perf_mode(struct tpdm_drvdata *drvdata, u32 *val)
{
	if (drvdata->dsb->mode & TPDM_DSB_MODE_PERF)
		*val |= TPDM_DSB_CR_MODE;
	else
		*val &= ~TPDM_DSB_CR_MODE;
}

static void set_dsb_tier(struct tpdm_drvdata *drvdata, u32 *val)
{
	/* Set pattern timestamp type and enablement */
	if (drvdata->dsb->patt_ts) {
		*val |= TPDM_DSB_TIER_PATT_TSENAB;
		if (drvdata->dsb->patt_type)
			*val |= TPDM_DSB_TIER_PATT_TYPE;
		else
			*val &= ~TPDM_DSB_TIER_PATT_TYPE;
	} else {
		*val &= ~TPDM_DSB_TIER_PATT_TSENAB;
	}

	/* Set trigger timestamp */
	if (drvdata->dsb->trig_ts)
		*val |= TPDM_DSB_TIER_XTRIG_TSENAB;
	else
		*val &= ~TPDM_DSB_TIER_XTRIG_TSENAB;

}

static void set_dsb_msr(struct tpdm_drvdata *drvdata)
{
	int i;

	if (drvdata->dsb->msr_num == 0)
		return;

	for (i = 0; i < drvdata->dsb->msr_num; i++)
		writel_relaxed(drvdata->dsb->msr[i],
			   drvdata->base + TPDM_DSB_MSR(i));
}

static void tpdm_enable_dsb(struct tpdm_drvdata *drvdata)
{
	u32 val, i;

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
		writel_relaxed(drvdata->dsb->trig_patt_val[i],
			    drvdata->base + TPDM_DSB_XPR(i));
		writel_relaxed(drvdata->dsb->trig_patt_mask[i],
			    drvdata->base + TPDM_DSB_XPMR(i));
	}

	val = readl_relaxed(drvdata->base + TPDM_DSB_TIER);
	set_dsb_tier(drvdata, &val);
	writel_relaxed(val, drvdata->base + TPDM_DSB_TIER);

	set_dsb_msr(drvdata);

	val = readl_relaxed(drvdata->base + TPDM_DSB_CR);
	/* Set the test accurate mode */
	set_dsb_test_mode(drvdata, &val);
	/* Set the byte lane for high-performance mode */
	set_dsb_hpsel_mode(drvdata, &val);
	/* Set the performance mode */
	set_dsb_perf_mode(drvdata, &val);
	/* Set trigger type */
	if (drvdata->dsb->trig_type)
		val |= TPDM_DSB_CR_TRIG_TYPE;
	else
		val &= ~TPDM_DSB_CR_TRIG_TYPE;
	/* Set the enable bit of DSB control register to 1 */
	val |= TPDM_DSB_CR_ENA;
	writel_relaxed(val, drvdata->base + TPDM_DSB_CR);
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

	/* Check if DSB datasets is present for TPDM. */
	if (drvdata->datasets & TPDM_PIDR0_DS_DSB)
		tpdm_enable_dsb(drvdata);

	CS_LOCK(drvdata->base);
}

static int tpdm_enable(struct coresight_device *csdev, struct perf_event *event,
		       enum cs_mode mode)
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

/* TPDM disable operations */
static void __tpdm_disable(struct tpdm_drvdata *drvdata)
{
	CS_UNLOCK(drvdata->base);

	/* Check if DSB datasets is present for TPDM. */
	if (drvdata->datasets & TPDM_PIDR0_DS_DSB)
		tpdm_disable_dsb(drvdata);

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

static int tpdm_datasets_setup(struct tpdm_drvdata *drvdata)
{
	u32 pidr;

	/*  Get the datasets present on the TPDM. */
	pidr = readl_relaxed(drvdata->base + CORESIGHT_PERIPHIDR0);
	drvdata->datasets |= pidr & GENMASK(TPDM_DATASETS - 1, 0);

	if (drvdata->datasets & TPDM_PIDR0_DS_DSB) {
		if (!drvdata->dsb) {
			drvdata->dsb = devm_kzalloc(drvdata->dev,
						    sizeof(*drvdata->dsb), GFP_KERNEL);
			if (!drvdata->dsb)
				return -ENOMEM;
		}
		if (!of_property_read_u32(drvdata->dev->of_node,
			   "qcom,dsb_msr_num", &drvdata->dsb->msr_num)) {
			drvdata->dsb->msr = devm_kzalloc(drvdata->dev,
				   (drvdata->dsb->msr_num * sizeof(*drvdata->dsb->msr)),
				   GFP_KERNEL);
			if (!drvdata->dsb->msr)
				return -ENOMEM;
		}
	}

	return 0;
}

static ssize_t reset_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf,
					  size_t size)
{
	int ret = 0;
	unsigned long val;
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	ret = kstrtoul(buf, 10, &val);
	if (ret || val != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	tpdm_reset_datasets(drvdata);
	spin_unlock(&drvdata->spinlock);

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
	drvdata->dsb->mode = val & TPDM_DSB_MODE_MASK;
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
	unsigned long bytes;
	int i;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_EDCR; i++) {
		bytes = sysfs_emit_at(buf, size,
				  "Index:0x%x Val:0x%x\n", i,
				  drvdata->dsb->edge_ctrl[i]);
		if (bytes <= 0)
			break;
		size += bytes;
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
	unsigned long val, mask, start, end, edge_ctrl;
	int i, reg;

	if (sscanf(buf, "%lx %lx %lx", &start, &end, &edge_ctrl) != 3)
		return -EINVAL;
	if ((start >= TPDM_DSB_MAX_LINES) || (end >= TPDM_DSB_MAX_LINES) ||
	    (start > end) || (edge_ctrl > 0x2))
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	for (i = start; i <= end; i++) {
		/*
		 * There are 2 bit per DSB Edge Control line.
		 * Thus we have 16 lines in a 32bit word.
		 */
		reg = EDCR_TO_WORD_IDX(i);
		mask = EDCR_TO_WORD_MASK(i);
		val = drvdata->dsb->edge_ctrl[reg];
		val &= ~EDCR_TO_WORD_MASK(i);
		val |= EDCR_TO_WORD_VAL(edge_ctrl, i);
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
	unsigned long bytes;
	int i;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_EDCMR; i++) {
		bytes = sysfs_emit_at(buf, size,
				  "Index:0x%x Val:0x%x\n", i,
				  drvdata->dsb->edge_ctrl_mask[i]);
		if (bytes <= 0)
			break;
		size += bytes;
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
	int i, reg;

	if (sscanf(buf, "%lx %lx %lx", &start, &end, &val) != 3)
		return -EINVAL;
	if ((start >= TPDM_DSB_MAX_LINES) || (end >= TPDM_DSB_MAX_LINES)
		|| (start > end) || (val & ~1UL))
		return -EPERM;

	spin_lock(&drvdata->spinlock);
	for (i = start; i <= end; i++) {
		/*
		 * There is 1 bit per DSB Edge Control Mark line.
		 * Thus we have 32 lines in a 32bit word.
		 */
		reg = EDCMR_TO_WORD_IDX(i);
		set = drvdata->dsb->edge_ctrl_mask[reg];
		if (val)
			set |= BIT(EDCR_TO_WORD_SHIFT(i));
		else
			set &= ~BIT(EDCR_TO_WORD_SHIFT(i));
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
	unsigned long bytes;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_PATT; i++) {
		bytes = sysfs_emit_at(buf, size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->dsb->patt_val[i]);
		if (bytes <= 0)
			break;
		size += bytes;
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
	unsigned long bytes;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_PATT; i++) {
		bytes = sysfs_emit_at(buf, size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->dsb->patt_mask[i]);
		if (bytes <= 0)
			break;
		size += bytes;
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

	if ((kstrtoul(buf, 0, &val)) || (val & ~1UL))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->dsb->patt_ts = !!val;
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

	if ((kstrtoul(buf, 0, &val)) || (val & ~1UL))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->dsb->patt_type = val;
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
	unsigned long bytes;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_PATT; i++) {
		bytes = sysfs_emit_at(buf, size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->dsb->trig_patt_val[i]);
		if (bytes <= 0)
			break;
		size += bytes;
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
	unsigned long bytes;
	int i = 0;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < TPDM_DSB_MAX_PATT; i++) {
		bytes = sysfs_emit_at(buf, size,
				  "Index: 0x%x Value: 0x%x\n", i,
				  drvdata->dsb->trig_patt_mask[i]);
		if (bytes <= 0)
			break;
		size += bytes;
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
				     struct device_attribute *attr, char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return sysfs_emit(buf, "%u\n",
			 (unsigned int)drvdata->dsb->trig_type);
}

/*
 * Trigger type (boolean):
 * false - Disable trigger type.
 * true  - Enable trigger type.
 */
static ssize_t dsb_trig_type_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if ((kstrtoul(buf, 0, &val)) || (val & ~1UL))
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
				     struct device_attribute *attr, char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return sysfs_emit(buf, "%u\n",
			 (unsigned int)drvdata->dsb->trig_ts);
}

/*
 * Trigger timestamp (boolean):
 * false - Disable trigger timestamp.
 * true  - Enable trigger timestamp.
 */
static ssize_t dsb_trig_ts_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if ((kstrtoul(buf, 0, &val)) || (val & ~1UL))
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

static ssize_t dsb_msr_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int i;
	unsigned long bytes;
	ssize_t size = 0;

	if (drvdata->dsb->msr_num == 0)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	for (i = 0; i < drvdata->dsb->msr_num; i++) {
		bytes = sysfs_emit_at(buf, size,
				  "%u 0x%x\n", i, drvdata->dsb->msr[i]);
		if (bytes <= 0)
			break;
		size += bytes;
	}
	spin_unlock(&drvdata->spinlock);

	return size;
}

static ssize_t dsb_msr_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t size)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int num, val;
	int nval;

	if (drvdata->dsb->msr_num == 0)
		return -EINVAL;

	nval = sscanf(buf, "%u %x", &num, &val);
	if ((nval != 2) || (num >= drvdata->dsb->msr_num))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->dsb->msr[num] = val;
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(dsb_msr);

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
	&dev_attr_dsb_msr.attr,
	NULL,
};

static struct attribute_group tpdm_dsb_attr_grp = {
	.attrs = tpdm_dsb_attrs,
	.is_visible = tpdm_dsb_is_visible,
};

static const struct attribute_group *tpdm_attr_grps[] = {
	&tpdm_attr_grp,
	&tpdm_dsb_attr_grp,
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

	ret = tpdm_datasets_setup(drvdata);
	if (ret)
		return ret;

	tpdm_reset_datasets(drvdata);

	/* Set up coresight component description */
	desc.name = coresight_alloc_device_name(&tpdm_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	desc.type = CORESIGHT_DEV_TYPE_SOURCE;
	desc.subtype.source_subtype = CORESIGHT_DEV_SUBTYPE_SOURCE_TPDM;
	desc.ops = &tpdm_cs_ops;
	desc.pdata = adev->dev.platform_data;
	desc.dev = &adev->dev;
	desc.access = CSDEV_ACCESS_IOMEM(base);
	desc.groups = tpdm_attr_grps;
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev))
		return PTR_ERR(drvdata->csdev);

	spin_lock_init(&drvdata->spinlock);

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
