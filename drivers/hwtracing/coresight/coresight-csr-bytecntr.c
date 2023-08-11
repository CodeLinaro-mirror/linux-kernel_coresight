// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "coresight-csr.h"

/* Read the data from ETR's DDR buffer */
static void tmc_etr_read_bytes(struct byte_cntr *byte_cntr_data, long offset,
			       size_t bytes, size_t *len, char **bufp)
{
	struct csr_assoc_data *assoc_data =
			container_of(byte_cntr_data, struct csr_assoc_data, byte_cntr_data);
	struct tmc_drvdata *tmcdrvdata = dev_get_drvdata(assoc_data->assoc_csdev->dev.parent);
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
	struct csr_assoc_data *assoc_data =
			container_of(byte_cntr_data, struct csr_assoc_data, byte_cntr_data);
	struct tmc_drvdata *tmcdrvdata = dev_get_drvdata(assoc_data->assoc_csdev->dev.parent);
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
void csr_byte_cntr_start(struct byte_cntr *byte_cntr_data)
{
	if (!byte_cntr_data)
		return;

	mutex_lock(&byte_cntr_data->byte_cntr_lock);

	/*
	 * When block_size is not set or /dev/byte-cntr
	 * is being read, don't start byte-cntr function.
	 */
	if (byte_cntr_data->block_size <= 0 || byte_cntr_data->read_active) {
		mutex_unlock(&byte_cntr_data->byte_cntr_lock);
		return;
	}

	atomic_set(&byte_cntr_data->irq_cnt, 0);
	byte_cntr_data->enable = true;
	mutex_unlock(&byte_cntr_data->byte_cntr_lock);
}

/* Stop byte-cntr function */
void csr_byte_cntr_stop(struct byte_cntr *byte_cntr_data)
{
	mutex_lock(&byte_cntr_data->byte_cntr_lock);
	byte_cntr_data->enable = false;
	byte_cntr_data->read_active = false;
	atomic_set(&byte_cntr_data->irq_cnt, 0);
	wake_up(&byte_cntr_data->wq);
	mutex_unlock(&byte_cntr_data->byte_cntr_lock);
}

static int tmc_etr_byte_cntr_release(struct inode *in, struct file *fp)
{
	struct byte_cntr *byte_cntr_data = fp->private_data;

	mutex_lock(&byte_cntr_data->byte_cntr_lock);
	byte_cntr_data->read_active = false;
	atomic_set(&byte_cntr_data->irq_cnt, 0);
	disable_irq_wake(byte_cntr_data->byte_cntr_irq);
	mutex_unlock(&byte_cntr_data->byte_cntr_lock);

	return 0;
}

static int tmc_etr_byte_cntr_open(struct inode *in, struct file *fp)
{
	struct byte_cntr *byte_cntr_data =
			container_of(in->i_cdev, struct byte_cntr, dev);
	struct csr_assoc_data *assoc_data =
			container_of(byte_cntr_data, struct csr_assoc_data, byte_cntr_data);
	struct tmc_drvdata *tmcdrvdata = dev_get_drvdata(assoc_data->assoc_csdev->dev.parent);

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

	ret = alloc_chrdev_region(&dev, baseminor, count, byte_cntr_data->name);
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

	byte_cntr_data->driver_class = class_create(byte_cntr_data->class_name);
	if (IS_ERR(byte_cntr_data->driver_class)) {
		ret = -ENOMEM;
		pr_err("class_create failed %d\n", ret);
		goto exit_unreg_chrdev_region;
	}

	device = device_create(byte_cntr_data->driver_class, NULL,
			       byte_cntr_data->dev.dev, byte_cntr_data,
			       byte_cntr_data->name);

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

int byte_cntr_init(struct csr_drvdata *drvdata, struct csr_assoc_data *assoc_data)
{
	int byte_cntr_irq;
	int ret = 0;
	struct device *dev = drvdata->dev;

	byte_cntr_irq = fwnode_irq_get_byname(assoc_data->fnode, CSR_DT_BYTECNTR_IRQ);
	if (byte_cntr_irq < 0)
		return byte_cntr_irq;

	ret = fwnode_property_read_string(assoc_data->fnode, CSR_DT_BYTECNTR_NAME,
				&assoc_data->byte_cntr_data.name);
	if (ret)
		assoc_data->byte_cntr_data.name = "byte-cntr";

	ret = fwnode_property_read_string(assoc_data->fnode, CSR_DT_BYTECNTR_CLASS_NAME,
				&assoc_data->byte_cntr_data.class_name);
	if (ret)
		assoc_data->byte_cntr_data.class_name = "coresight-tmc-etr-stream";

	ret = fwnode_property_read_u32(assoc_data->fnode, CSR_DT_BYTECNTVAL_OFFSET,
				&assoc_data->byte_cntr_data.byte_cntr_offset);
	if (ret)
		assoc_data->byte_cntr_data.byte_cntr_offset = CSR_ETR_BYTECNTVAL;

	ret = devm_request_irq(dev, byte_cntr_irq, etr_handler,
			       IRQF_TRIGGER_RISING | IRQF_SHARED,
			       assoc_data->assoc_dev_name, &assoc_data->byte_cntr_data);
	if (ret)
		return ret;

	ret = byte_cntr_register_chardev(&assoc_data->byte_cntr_data);
	if (ret)
		return ret;

	assoc_data->byte_cntr_data.byte_cntr_irq = byte_cntr_irq;
	atomic_set(&assoc_data->byte_cntr_data.irq_cnt, 0);
	init_waitqueue_head(&assoc_data->byte_cntr_data.wq);
	mutex_init(&assoc_data->byte_cntr_data.byte_cntr_lock);

	return ret;
}

void byte_cntr_remove(struct byte_cntr *byte_cntr_data)
{
	device_destroy(byte_cntr_data->driver_class,
				byte_cntr_data->dev.dev);
	class_destroy(byte_cntr_data->driver_class);
	unregister_chrdev_region(byte_cntr_data->dev.dev, 1);
}

