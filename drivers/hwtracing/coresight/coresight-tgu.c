/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/amba/bus.h>
#include <linux/topology.h>
#include <linux/of.h>
#include <linux/coresight.h>

#include "coresight-priv.h"
#include "coresight-tgu.h"

DEFINE_CORESIGHT_DEVLIST(tgu_devs, "tgu");

static void tgu_unlock(struct tgu_drvdata *drvdata) {
    mb();
    tgu_writel(drvdata, 0x0, CORESIGHT_LAR);
}

static void tgu_lock(struct tgu_drvdata *drvdata) {
    tgu_writel(drvdata, CORESIGHT_UNLOCK, CORESIGHT_LAR);
    mb();
}

/* enable_tgu_store - Configure Trace and Gating Unit (TGU) triggers. */
static ssize_t enable_tgu_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long value;
	int ret, i, j;

	if (kstrtoul(buf, 16, &value))
		return -EINVAL;

	/* Enable clock */
	ret = pm_runtime_get_sync(drvdata->dev);
	if (ret < 0) {
		pm_runtime_put(drvdata->dev);
		return ret;
	}

	spin_lock(&drvdata->spinlock);
	tgu_unlock(drvdata);

	if (value) {
		/* Disable TGU to program the triggers */
		tgu_writel(drvdata, 0, TGU_CONTROL);

		/* Program the TGU Group data for the desired use case */
		for (i = 0; i <= drvdata->cnt_group; i++)
			tgu_writel(drvdata, drvdata->group_data[i].value,
				   drvdata->group_data[i].addr);

		/* Program the unused Condition Decode registers NOT bits to 1*/
		for (i = 0; i <= drvdata->max_condition; i++) {
			for (j = 0; j <= drvdata->max_step; j++)
				tgu_writel(drvdata, 0x1000000,
						CONDITION_DECODE_STEP(i, j));
		}

		/* Program the TGU Condition Decode for the desired use case */
		for (i = 0; i <= drvdata->cnt_condition; i++)
			tgu_writel(drvdata, drvdata->condition_data[i].value,
				   drvdata->condition_data[i].addr);

		/* Program the TGU Condition Select for the desired use case */
		for (i = 0; i <= drvdata->cnt_select; i++)
			tgu_writel(drvdata, drvdata->select_data[i].value,
				   drvdata->select_data[i].addr);

		/* Timer and Counter Check */
		for (i = 0; i <= drvdata->cnt_timer; i++)
			tgu_writel(drvdata, drvdata->timer_data[i].value,
				   drvdata->timer_data[i].addr);

		for (i = 0; i <= drvdata->cnt_counter; i++)
			tgu_writel(drvdata, drvdata->counter_data[i].value,
			drvdata->counter_data[i].addr);

		/* Enable TGU to program the triggers */
		tgu_writel(drvdata, 1, TGU_CONTROL);

		drvdata->enable = true;
		dev_dbg(dev, "Coresight-TGU enabled\n");
	} else {
		/* Disable TGU to program the triggers */
		tgu_writel(drvdata, 0, TGU_CONTROL);
		pm_runtime_put(drvdata->dev);
		dev_dbg(dev, "Coresight-TGU disabled\n");
	}

	/* Lock the TGU LAR */
	tgu_lock(drvdata);
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_WO(enable_tgu);

/* reset_tgu_store - Reset Trace and Gating Unit (TGU) configuration.*/
static ssize_t reset_tgu_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t size)
{
	unsigned long value;
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	int ret;

	if (kstrtoul(buf, 16, &value))
		return -EINVAL;

	if (!drvdata->enable) {
		ret = pm_runtime_get_sync(drvdata->dev);
		if (ret < 0) {
			pm_runtime_put(drvdata->dev);
			return ret;
		}
	}

	spin_lock(&drvdata->spinlock);
	tgu_unlock(drvdata);

	if (value) {
		tgu_writel(drvdata, 0, TGU_CONTROL);

		drvdata->cnt_group = 0;
		drvdata->cnt_condition = 0;
		drvdata->cnt_select = 0;
		drvdata->cnt_timer = 0;
		drvdata->cnt_counter = 0;

		dev_dbg(dev, "Coresight-TGU reset complete\n");
	} else {
		dev_dbg(dev, "Coresight-TGU invalid input\n");
	}

	tgu_lock(drvdata);
	pm_runtime_put(drvdata->dev);

	return size;
}
static DEVICE_ATTR_WO(reset_tgu);

static ssize_t group_index_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	spin_lock(&drvdata->spinlock);
	value = drvdata->group_data[drvdata->cnt_group].index;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t group_index_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf,
				 size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (value <= MAX_GROUPS)
		drvdata->group_data[drvdata->cnt_group].index = value;
	else
		dev_err(drvdata->dev, "Invalid group index data\n");

	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(group_index);

static ssize_t group_reg_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;
    
	spin_lock(&drvdata->spinlock);
	value = drvdata->group_data[drvdata->cnt_group].reg;
	spin_unlock(&drvdata->spinlock);
    
	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t group_reg_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;
    
	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;
    
	spin_lock(&drvdata->spinlock);
    
	if (value <= drvdata->max_reg)
		drvdata->group_data[drvdata->cnt_group].reg = value;
	else
		dev_err(drvdata->dev, "Invalid group reg data\n");
 
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(group_reg);

static ssize_t group_step_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;
    
	spin_lock(&drvdata->spinlock);
	value = drvdata->group_data[drvdata->cnt_group].step;
	spin_unlock(&drvdata->spinlock);
    
	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t group_step_store(struct device *dev,
				struct device_attribute *attr,
			       	const char *buf,
			       	size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;
    
	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;
    
	spin_lock(&drvdata->spinlock);

	if (value <= drvdata->max_step)
		drvdata->group_data[drvdata->cnt_group].step = value;
	else
 		dev_err(drvdata->dev, "Invalid group step data\n");
    
	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(group_step);

static ssize_t group_value_show(struct device *dev,
	       			struct device_attribute *attr,
			       	char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;
 
	spin_lock(&drvdata->spinlock);
	value = drvdata->group_data[drvdata->cnt_group].value;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t group_value_store(struct device *dev,
	       			 struct device_attribute *attr,
				 const char *buf,
				 size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;
 
	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->group_data[drvdata->cnt_group].value = value;
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_RW(group_value);

static ssize_t condition_index_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	spin_lock(&drvdata->spinlock);
	value = drvdata->condition_data[drvdata->cnt_condition].index;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t condition_index_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf,
				     size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);

	if (value <= drvdata->max_condition) 
		drvdata->condition_data[drvdata->cnt_condition].index = value;
	else
		dev_err(drvdata->dev, "Invalid condition index data\n");

	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(condition_index);

static ssize_t condition_step_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	spin_lock(&drvdata->spinlock);
	value = drvdata->condition_data[drvdata->cnt_condition].step;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t condition_step_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);

	if (value <= drvdata->max_step)
		drvdata->condition_data[drvdata->cnt_condition].step = value;
	else
		dev_err(drvdata->dev, "Invalid condition step data\n");

	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(condition_step);

static ssize_t condition_value_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	spin_lock(&drvdata->spinlock);
	value = drvdata->condition_data[drvdata->cnt_condition].value;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t condition_value_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf,
				     size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->condition_data[drvdata->cnt_condition].value = value;
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_RW(condition_value);

static ssize_t select_index_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int index;

	spin_lock(&drvdata->spinlock);
	index = drvdata->select_data[drvdata->cnt_select].index;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", index);
}

static ssize_t select_index_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int index;

	if (sscanf(buf, "%x", &index) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);

	if (index <= drvdata->max_condition)
		drvdata->select_data[drvdata->cnt_select].index = index;
	else
		dev_err(drvdata->dev, "Invalid condition index data\n");

	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(select_index);

static ssize_t select_step_show(struct device *dev,
				struct device_attribute *attr,
			       	char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int step;

	spin_lock(&drvdata->spinlock);
	step = drvdata->select_data[drvdata->cnt_select].step;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", step);
}

static ssize_t select_step_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf,
				 size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int step;

	if (sscanf(buf, "%x", &step) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);

	if (step <= drvdata->max_step)
		drvdata->select_data[drvdata->cnt_select].step = step;
	else
		dev_err(drvdata->dev, "Invalid select step data\n");

	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(select_step);

static ssize_t select_value_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	spin_lock(&drvdata->spinlock);
	value = drvdata->select_data[drvdata->cnt_select].value;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t select_value_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->select_data[drvdata->cnt_select].value = value;
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_RW(select_value);

static ssize_t timer_step_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	spin_lock(&drvdata->spinlock);
	value = drvdata->timer_data[drvdata->cnt_timer].step;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t timer_step_store(struct device *dev,
				struct device_attribute *attr,
			       	const char *buf,
			       	size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);

	if (value <= drvdata->max_timer)
		drvdata->timer_data[drvdata->cnt_timer].step = value;
	else
		dev_err(drvdata->dev, "Invalid timer step data\n");

	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(timer_step);

static ssize_t timer_value_show(struct device *dev,
				struct device_attribute *attr,
			       	char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	spin_lock(&drvdata->spinlock);
	value = drvdata->timer_data[drvdata->cnt_timer].value;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t timer_value_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf,
				 size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->timer_data[drvdata->cnt_timer].value = value;
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_RW(timer_value);

static ssize_t counter_step_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	spin_lock(&drvdata->spinlock);
	value = drvdata->counter_data[drvdata->cnt_counter].step;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t counter_step_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);

	if (value <= drvdata->max_counter)
		drvdata->counter_data[drvdata->cnt_counter].step = value;
	else
		dev_err(drvdata->dev, "Invalid counter step data\n");

	spin_unlock(&drvdata->spinlock);
	return size;
}
static DEVICE_ATTR_RW(counter_step);

static ssize_t counter_value_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	spin_lock(&drvdata->spinlock);
	value = drvdata->counter_data[drvdata->cnt_counter].value;
	spin_unlock(&drvdata->spinlock);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", value);
}

static ssize_t counter_value_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf,
				   size_t size)
{
	struct tgu_drvdata* drvdata = dev_get_drvdata(dev->parent);
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	drvdata->counter_data[drvdata->cnt_counter].value = value;
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_RW(counter_value);

static ssize_t group_config_write_store(struct device *dev,
					struct device_attribute *attr,
				       	const char *buf,
				       	size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long value;
	int ret = 0;
	struct trigger_data* group_data = 
			&drvdata->group_data[drvdata->cnt_group];

	if (drvdata->cnt_group >= MAX_GROUP_SETS) {
		dev_err(drvdata->dev, "Too many groups are configured\n");
		return -EINVAL;
	}

	ret = kstrtoul(buf, 16, &value);
	if (ret)
		return ret;

	if (value) {
		spin_lock(&drvdata->spinlock);
		group_data->addr = GROUP_REG_STEP(group_data->index,
						    group_data->reg,
						    group_data->step);
		drvdata->cnt_group++;
		spin_unlock(&drvdata->spinlock);
	}
	return size;
}
static DEVICE_ATTR_WO(group_config_write);

static ssize_t condition_config_write_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf,
					    size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long value;
	int ret = 0;
	struct trigger_data* condition_data =
			&drvdata->condition_data[drvdata->cnt_condition];

	if (drvdata->cnt_condition >= MAX_CONDITION_SETS) {
		dev_err(drvdata->dev, "Too many conditions are configured\n");
		return -EINVAL;
	}

	ret = kstrtoul(buf, 16, &value);
	if (ret)
		return ret;

	if (value) {
		spin_lock(&drvdata->spinlock);
		condition_data->addr = 
			CONDITION_DECODE_STEP(condition_data->index,
				       condition_data->step);
		drvdata->cnt_condition++;
		spin_unlock(&drvdata->spinlock);
	}
	return size;
}
static DEVICE_ATTR_WO(condition_config_write);

static ssize_t select_config_write_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf,
					 size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long value;
	int ret = 0;
	struct trigger_data* select_data =
			&drvdata->select_data[drvdata->cnt_select];

	if (drvdata->cnt_select >= MAX_CONDITION_SETS) {
		dev_err(drvdata->dev, "Too many selects are configured\n");
		return -EINVAL;
	}

	ret = kstrtoul(buf, 16, &value);
	if (ret)
		return ret;

	if (value) {
		spin_lock(&drvdata->spinlock);
		select_data->addr = CONDITION_SELECT_STEP(select_data->index,
						      select_data->step);
		drvdata->cnt_select++;
		spin_unlock(&drvdata->spinlock);
	}
	return size;
}
static DEVICE_ATTR_WO(select_config_write);

static ssize_t timer_config_write_store(struct device *dev,
					struct device_attribute *attr,
				       	const char *buf,
				       	size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long value;
	int ret = 0;
	struct trigger_data* timer_data =
	       		&drvdata->timer_data[drvdata->cnt_timer];

	if (drvdata->cnt_timer >= MAX_TIMER_COUNTER_SETS) {
		dev_err(drvdata->dev, "Too many timers are configured\n");
		return -EINVAL;
	}

	ret = kstrtoul(buf, 16, &value);
	if (ret)
		return ret;

	if (value) {
		spin_lock(&drvdata->spinlock);
		timer_data->addr = TIMER0_COMPARE_STEP(timer_data->step);
		drvdata->cnt_timer++;
		spin_unlock(&drvdata->spinlock);
	}
	return size;
}
static DEVICE_ATTR_WO(timer_config_write);

static ssize_t counter_config_write_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf,
					  size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long value;
	int ret = 0;
	struct trigger_data* counter_data =
			&drvdata->counter_data[drvdata->cnt_counter];

	if (drvdata->cnt_counter >= MAX_TIMER_COUNTER_SETS) {
		dev_err(drvdata->dev, "Too many counters are configured\n");
		return -EINVAL;
	}

	ret = kstrtoul(buf, 16, &value);
	if (ret)
		return ret;

	if (value) {
		spin_lock(&drvdata->spinlock);
		counter_data->addr =
				COUNTER0_COMPARE_STEP(counter_data->step);
		drvdata->cnt_counter++;
		spin_unlock(&drvdata->spinlock);
	}
	return size;
}
static DEVICE_ATTR_WO(counter_config_write);

static ssize_t group_config_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int i;
	ssize_t len = 0;

	for (i = 0; i < drvdata->cnt_group; i++) {
        	len += scnprintf(buf + len, PAGE_SIZE - len,
				 "%u %#x %#x %#x %#x\n", i,
				 drvdata->group_data[i].index,
				 drvdata->group_data[i].reg,
				 drvdata->group_data[i].step,
				 drvdata->group_data[i].value);
	}
	return len;
}
static DEVICE_ATTR_RO(group_config);

static ssize_t condition_config_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int i;
	ssize_t len = 0;

	for (i = 0; i < drvdata->cnt_condition; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len,
				 "%u %#x %#x %#x\n", i,
				 drvdata->condition_data[i].index,
				 drvdata->condition_data[i].step,
				 drvdata->condition_data[i].value);
    	}
	return len;
}
static DEVICE_ATTR_RO(condition_config);

static ssize_t select_config_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int i;
	ssize_t len = 0;

	for (i = 0; i < drvdata->cnt_select; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len,
				 "%u %#x %#x %#x\n", i,
				 drvdata->select_data[i].index,
				 drvdata->select_data[i].step,
				 drvdata->select_data[i].value);
	}
	return len;
}
static DEVICE_ATTR_RO(select_config);

static ssize_t timer_config_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int i;
	ssize_t len = 0;

	for (i = 0; i < drvdata->cnt_timer; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len,
				 "%u %#x %#x\n", i,
				 drvdata->timer_data[i].step,
				 drvdata->timer_data[i].value);
	}
	return len;
}
static DEVICE_ATTR_RO(timer_config);

static ssize_t counter_config_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned int i;
	ssize_t len = 0;

	for (i = 0; i < drvdata->cnt_counter; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len,
				 "%u %#x %#x\n", i,
				 drvdata->counter_data[i].step,
				 drvdata->counter_data[i].value);
	}
	return len;
}
static DEVICE_ATTR_RO(counter_config);

static struct attribute *tgu_common_attrs[] = {
	&dev_attr_enable_tgu.attr,
	&dev_attr_reset_tgu.attr,
	NULL,
};

static struct attribute *tgu_group_attrs[] = {
	&dev_attr_group_value.attr,
	&dev_attr_group_index.attr,
	&dev_attr_group_reg.attr,
	&dev_attr_group_step.attr,
	&dev_attr_group_config_write.attr,
	&dev_attr_group_config.attr,
	NULL,
};

static struct attribute *tgu_condition_attrs[] = {
	&dev_attr_condition_value.attr,
	&dev_attr_condition_index.attr,
	&dev_attr_condition_step.attr,
	&dev_attr_condition_config_write.attr,
	&dev_attr_condition_config.attr,
	NULL,
};

static struct attribute *tgu_select_attrs[] = {
	&dev_attr_select_value.attr,
	&dev_attr_select_index.attr,
	&dev_attr_select_step.attr,
	&dev_attr_select_config_write.attr,
	&dev_attr_select_config.attr,
	NULL,
};

static struct attribute *tgu_timer_attrs[] = {
	&dev_attr_timer_value.attr,
	&dev_attr_timer_step.attr,
	&dev_attr_timer_config_write.attr,
	&dev_attr_timer_config.attr,
	NULL,
};

static struct attribute *tgu_counter_attrs[] = {
	&dev_attr_counter_value.attr,
	&dev_attr_counter_step.attr,
	&dev_attr_counter_config_write.attr,
	&dev_attr_counter_config.attr,
	NULL,
};

static struct attribute_group tgu_common_grp = {
	.attrs = tgu_common_attrs,
	NULL,
};

static struct attribute_group tgu_group_grp = {
	.attrs = tgu_group_attrs,
	.name = "group",
};

static struct attribute_group tgu_condition_grp = {
	.attrs = tgu_condition_attrs,
	.name = "condition",
};

static struct attribute_group tgu_select_grp = {
	.attrs = tgu_select_attrs,
	.name = "select",
};

static struct attribute_group tgu_timer_grp = {
	.attrs = tgu_timer_attrs,
	.name = "timer",
};

static struct attribute_group tgu_counter_grp = {
	.attrs = tgu_counter_attrs,
	.name = "counter",
};

static const struct attribute_group *tgu_grps[] = {
	&tgu_common_grp,
	&tgu_group_grp,
	&tgu_condition_grp,
	&tgu_select_grp,
	&tgu_timer_grp,
	&tgu_counter_grp,
	NULL,
};

static int tgu_probe(struct amba_device *adev, const struct amba_id *id)
{
	int ret = 0;
	struct device *dev = &adev->dev;
	struct coresight_platform_data *pdata;
	struct tgu_drvdata *drvdata;
	struct coresight_desc desc = { 0 };

	desc.name = coresight_alloc_device_name(&tgu_devs, dev);
	if (!desc.name)
		return -ENOMEM;

	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	adev->dev.platform_data = pdata;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = &adev->dev;
	dev_set_drvdata(dev, drvdata);

	drvdata->base = devm_ioremap_resource(dev, &adev->res);
	if (!drvdata->base)
		return -ENOMEM;

	spin_lock_init(&drvdata->spinlock);

	ret = of_property_read_u32(adev->dev.of_node, "tgu-steps",
					&drvdata->max_step);
	if (ret)
		return -EINVAL;

	ret = of_property_read_u32(adev->dev.of_node, "tgu-conditions",
					&drvdata->max_condition);
	if (ret)
		return -EINVAL;

	ret = of_property_read_u32(adev->dev.of_node, "tgu-regs",
					&drvdata->max_reg);
	if (ret)
		return -EINVAL;

	ret = of_property_read_u32(adev->dev.of_node, "tgu-timer-counter",
					&drvdata->max_timer);
	if (ret)
		return -EINVAL;

	/* Alloc memory for Grps, Conditions, and Steps */
	drvdata->group_data = devm_kzalloc(dev, MAX_GROUP_SETS *
					sizeof(*drvdata->group_data),
					GFP_KERNEL);
	if (!drvdata->group_data)
		return -ENOMEM;

	drvdata->condition_data = devm_kzalloc(dev, MAX_CONDITION_SETS *
					sizeof(*drvdata->condition_data),
					GFP_KERNEL);
	if (!drvdata->condition_data)
		return -ENOMEM;

	drvdata->select_data = devm_kzalloc(dev, MAX_CONDITION_SETS *
				       sizeof(*drvdata->select_data),
				       GFP_KERNEL);
	if (!drvdata->select_data)
		return -ENOMEM;

	drvdata->timer_data = devm_kzalloc(dev, MAX_TIMER_COUNTER_SETS *
				       sizeof(*drvdata->timer_data),
				       GFP_KERNEL);
	if (!drvdata->timer_data)
		return -ENOMEM;

	drvdata->counter_data = devm_kzalloc(dev, MAX_TIMER_COUNTER_SETS *
				       sizeof(*drvdata->counter_data),
				       GFP_KERNEL);
	if (!drvdata->counter_data)
		return -ENOMEM;

	drvdata->enable = false;

	desc.type = CORESIGHT_DEV_TYPE_HELPER;
	desc.pdata = adev->dev.platform_data;
	desc.dev = &adev->dev;
	desc.groups = tgu_grps;
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev)) {
		ret = PTR_ERR(drvdata->csdev);
		goto err;
	}

	pm_runtime_put(&adev->dev);
	dev_dbg(dev, "TGU initialized\n");
	return 0;
err:
	pm_runtime_put(&adev->dev);
	return ret;
}

static struct amba_id tgu_ids[] = {
	{
		.id	=	0x0003b999,
		.mask	=	0x0003ffff,
		.data	=	"TGU",
	},
	{0, 0},
};

static struct amba_driver tgu_driver = {
	.drv = {
		.name			=	"coresight-tgu",
		.owner			=	THIS_MODULE,
		.suppress_bind_attrs	=	true,
	},
	.probe		=	tgu_probe,
	.id_table	=	tgu_ids,
};

builtin_amba_driver(tgu_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight TGU driver");
