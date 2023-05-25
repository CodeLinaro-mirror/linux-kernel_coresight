// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/of_irq.h>
#include <linux/moduleparam.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/property.h>

#include "coresight-csr.h"
#include "coresight-byte-cntr.h"

/* Read the data from ETR's DDR buffer */
static void tmc_etr_read_bytes(struct byte_cntr *byte_cntr_data, long offset,
			       size_t bytes, size_t *len, char **bufp)
{
	struct tmc_drvdata *tmcdrvdata = byte_cntr_data->tmcdrvdata;
	struct etr_buf *etr_buf = tmcdrvdata->sysfs_buf;
	size_t actual;

	if (*len >= bytes)
		*len = bytes;
	else if (((uint32_t)offset % bytes) + *len > bytes)
		*len = bytes - ((uint32_t)offset % bytes);

	actual = tmc_etr_buf_get_data(etr_buf, offset, *len, bufp);
	*len = actual;
	if (actual == bytes || (actual + (uint32_t)offset) % bytes == 0)
		atomic_dec(&byte_cntr_data->irq_cnt);
}


static irqreturn_t etr_handler(int irq, void *data)
{
	struct byte_cntr *byte_cntr_data = data;

	atomic_inc(&byte_cntr_data->irq_cnt);
	wake_up(&byte_cntr_data->wq);

	return IRQ_HANDLED;
}

/* Read function for /dev/byte-cntr */
static ssize_t tmc_etr_byte_cntr_read(struct file *fp, char __user *data,
			       size_t len, loff_t *ppos)
{
	struct byte_cntr *byte_cntr_data = fp->private_data;
	struct tmc_drvdata *tmcdrvdata = byte_cntr_data->tmcdrvdata;
	char *bufp = NULL;
	int ret = 0;

	if (!data)
		return -EINVAL;

	mutex_lock(&byte_cntr_data->byte_cntr_lock);

	if (byte_cntr_data->enable) {
		if (!atomic_read(&byte_cntr_data->irq_cnt)) {
			mutex_unlock(&byte_cntr_data->byte_cntr_lock);
			if (wait_event_interruptible(byte_cntr_data->wq,
				atomic_read(&byte_cntr_data->irq_cnt) > 0
				|| !byte_cntr_data->enable))
				return -ERESTARTSYS;
			mutex_lock(&byte_cntr_data->byte_cntr_lock);
		}

		tmc_etr_read_bytes(byte_cntr_data, byte_cntr_data->offset,
				   byte_cntr_data->block_size, &len, &bufp);
	} else {
		ret = -EINVAL;
		goto err0;
	}

	if (copy_to_user(data, bufp, len)) {
		mutex_unlock(&byte_cntr_data->byte_cntr_lock);
		dev_dbg(&tmcdrvdata->csdev->dev,
			"%s: copy_to_user failed\n", __func__);
		return -EFAULT;
	}

	if (byte_cntr_data->offset + len >= tmcdrvdata->size)
		byte_cntr_data->offset = 0;
	else
		byte_cntr_data->offset += len;

	goto out;

err0:
	mutex_unlock(&byte_cntr_data->byte_cntr_lock);
	return ret;
out:
	mutex_unlock(&byte_cntr_data->byte_cntr_lock);
	return len;
}

/* Start byte-cntr function. */
void tmc_etr_byte_cntr_start(struct byte_cntr *byte_cntr_data)
{
	if (!byte_cntr_data)
		return;

	mutex_lock(&byte_cntr_data->byte_cntr_lock);

	/*
	 * When block_size is not set or /dev/byte-cntr
	 * is being read, don't start byte-cntr function.
	 */
	if (byte_cntr_data->block_size == 0
		|| byte_cntr_data->read_active) {
		mutex_unlock(&byte_cntr_data->byte_cntr_lock);
		return;
	}

	atomic_set(&byte_cntr_data->irq_cnt, 0);
	byte_cntr_data->enable = true;
	mutex_unlock(&byte_cntr_data->byte_cntr_lock);
}

/* Stop byte-cntr function */
void tmc_etr_byte_cntr_stop(struct byte_cntr *byte_cntr_data)
{
	struct tmc_drvdata *tmcdrvdata;

	if (!byte_cntr_data)
		return;

	tmcdrvdata = byte_cntr_data->tmcdrvdata;

	mutex_lock(&byte_cntr_data->byte_cntr_lock);
	byte_cntr_data->enable = false;
	byte_cntr_data->read_active = false;
	atomic_set(&byte_cntr_data->irq_cnt, 0);
	wake_up(&byte_cntr_data->wq);
	coresight_csr_set_byte_cntr(tmcdrvdata->csr, 0);
	mutex_unlock(&byte_cntr_data->byte_cntr_lock);
}

static int tmc_etr_byte_cntr_release(struct inode *in, struct file *fp)
{
	struct byte_cntr *byte_cntr_data = fp->private_data;
	struct tmc_drvdata *tmcdrvdata = byte_cntr_data->tmcdrvdata;

	mutex_lock(&byte_cntr_data->byte_cntr_lock);
	byte_cntr_data->read_active = false;

	atomic_set(&byte_cntr_data->irq_cnt, 0);

	if (byte_cntr_data->enable)
		coresight_csr_set_byte_cntr(tmcdrvdata->csr, 0);

	disable_irq_wake(byte_cntr_data->byte_cntr_irq);

	mutex_unlock(&byte_cntr_data->byte_cntr_lock);

	return 0;
}

static int tmc_etr_byte_cntr_open(struct inode *in, struct file *fp)
{
	struct byte_cntr *byte_cntr_data =
			container_of(in->i_cdev, struct byte_cntr, dev);
	struct tmc_drvdata *tmcdrvdata = byte_cntr_data->tmcdrvdata;

	mutex_lock(&byte_cntr_data->byte_cntr_lock);

	if (byte_cntr_data->read_active) {
		mutex_unlock(&byte_cntr_data->byte_cntr_lock);
		return -EBUSY;
	}

	if (tmcdrvdata->mode != CS_MODE_SYSFS ||
			!byte_cntr_data->block_size) {
		mutex_unlock(&byte_cntr_data->byte_cntr_lock);
		return -EINVAL;
	}

	enable_irq_wake(byte_cntr_data->byte_cntr_irq);
	/*
	 * IRQ is a '8- byte' counter and to observe interrupt at
	 * block_size' bytes of data
	 */
	coresight_csr_set_byte_cntr(tmcdrvdata->csr, (byte_cntr_data->block_size) / 8);

	fp->private_data = byte_cntr_data;
	nonseekable_open(in, fp);
	byte_cntr_data->enable = true;
	byte_cntr_data->read_active = true;
	mutex_unlock(&byte_cntr_data->byte_cntr_lock);
	return 0;
}

static const struct file_operations byte_cntr_fops = {
	.owner		= THIS_MODULE,
	.open		= tmc_etr_byte_cntr_open,
	.read		= tmc_etr_byte_cntr_read,
	.release	= tmc_etr_byte_cntr_release,
	.llseek		= no_llseek,
};

static int byte_cntr_register_chardev(struct byte_cntr *byte_cntr_data)
{
	int ret;
	unsigned int baseminor = 0;
	unsigned int count = 1;
	struct device *device;
	dev_t dev;

	ret = alloc_chrdev_region(&dev, baseminor, count, "byte-cntr");
	if (ret < 0) {
		pr_err("alloc_chrdev_region failed %d\n", ret);
		return ret;
	}
	cdev_init(&byte_cntr_data->dev, &byte_cntr_fops);

	byte_cntr_data->dev.owner = THIS_MODULE;
	byte_cntr_data->dev.ops = &byte_cntr_fops;

	ret = cdev_add(&byte_cntr_data->dev, dev, 1);
	if (ret)
		goto exit_unreg_chrdev_region;

	byte_cntr_data->driver_class = class_create(THIS_MODULE,
						   "coresight-tmc-etr-stream");
	if (IS_ERR(byte_cntr_data->driver_class)) {
		ret = -ENOMEM;
		pr_err("class_create failed %d\n", ret);
		goto exit_unreg_chrdev_region;
	}

	device = device_create(byte_cntr_data->driver_class, NULL,
			       byte_cntr_data->dev.dev, byte_cntr_data,
			       "byte-cntr");

	if (IS_ERR(device)) {
		pr_err("class_device_create failed %d\n", ret);
		ret = -ENOMEM;
		goto exit_destroy_class;
	}

	return 0;

exit_destroy_class:
	class_destroy(byte_cntr_data->driver_class);
exit_unreg_chrdev_region:
	unregister_chrdev_region(byte_cntr_data->dev.dev, 1);
	return ret;
}

struct byte_cntr *byte_cntr_init(struct amba_device *adev,
				 struct tmc_drvdata *drvdata)
{
	struct device *dev = &adev->dev;
	struct device_node *np = adev->dev.of_node;
	int byte_cntr_irq;
	int ret;
	struct byte_cntr *byte_cntr_data;

	byte_cntr_irq = of_irq_get_byname(np, "byte-cntr-irq");
	if (byte_cntr_irq < 0)
		return NULL;

	byte_cntr_data = devm_kzalloc(dev, sizeof(*byte_cntr_data), GFP_KERNEL);
	if (!byte_cntr_data)
		return NULL;

	ret = devm_request_irq(dev, byte_cntr_irq, etr_handler,
			       IRQF_TRIGGER_RISING | IRQF_SHARED,
			       "tmc-etr", byte_cntr_data);
	if (ret) {
		devm_kfree(dev, byte_cntr_data);
		dev_err(dev, "Byte_cntr interrupt registration failed\n");
		return NULL;
	}

	ret = byte_cntr_register_chardev(byte_cntr_data);
	if (ret) {
		devm_free_irq(dev, byte_cntr_irq, byte_cntr_data);
		devm_kfree(dev, byte_cntr_data);
		dev_err(dev, "Byte_cntr char dev registration failed\n");
		return NULL;
	}

	byte_cntr_data->tmcdrvdata = drvdata;
	byte_cntr_data->byte_cntr_irq = byte_cntr_irq;
	atomic_set(&byte_cntr_data->irq_cnt, 0);
	init_waitqueue_head(&byte_cntr_data->wq);
	mutex_init(&byte_cntr_data->byte_cntr_lock);

	return byte_cntr_data;
}

void byte_cntr_remove(struct byte_cntr *byte_cntr_data)
{
	device_destroy(byte_cntr_data->driver_class,
				byte_cntr_data->dev.dev);
	class_destroy(byte_cntr_data->driver_class);
	unregister_chrdev_region(byte_cntr_data->dev.dev, 1);
}

