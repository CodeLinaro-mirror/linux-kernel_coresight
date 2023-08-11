// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/slab.h>

#include "coresight-priv.h"
#include "coresight-csr.h"
#include <dt-bindings/arm/coresight-csr-dt.h>

static LIST_HEAD(csr_list);

DEFINE_CORESIGHT_DEVLIST(csr_devs, "csr");

#define csdev_to_csr_drvdata(csdev)	\
	dev_get_drvdata(csdev->dev.parent)

static ssize_t etr_byte_cntr_val_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct csr_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", drvdata->etr_byte_cntr_value);
}

static ssize_t etr_byte_cntr_val_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf,
				size_t size)
{
	unsigned long val, flags;
	int ret;
	struct csr_drvdata *drvdata = dev_get_drvdata(dev->parent);

	ret = clk_prepare_enable(drvdata->clk);
	if (ret)
		return ret;

	spin_lock_irqsave(&drvdata->spin_lock, flags);

	if (kstrtoul(buf, 16, &val)) {
		spin_unlock_irqrestore(&drvdata->spin_lock, flags);
		return -EINVAL;
	}

	drvdata->etr_byte_cntr_value = val;
	CS_UNLOCK(drvdata->base);
	/*
	 * A zero setting disables the interrupt. A one
	 * setting means 8 bytes, two 16 bytes, etc. In
	 * other words, the value in this register is the
	 * interrupt threshold times 8 bytes.
	 */
	writel_relaxed(val / 8, drvdata->base + CSR_ETR_BYTECNTVAL);
	CS_LOCK(drvdata->base);

	spin_unlock_irqrestore(&drvdata->spin_lock, flags);

	clk_disable_unprepare(drvdata->clk);
	return size;
}
static DEVICE_ATTR_RW(etr_byte_cntr_val);

static struct attribute *csr_attrs[] = {
	&dev_attr_etr_byte_cntr_val.attr,
	NULL,
};

static struct attribute_group csr_attr_grp = {
	.attrs = csr_attrs,
};

static const struct attribute_group *csr_attr_grps[] = {
	&csr_attr_grp,
	NULL,
};

static int csr_read_bytecntr_value(struct csr_drvdata *drvdata, void __iomem * address)
{
	int ret;
	int byte_cntr_value;

	ret = clk_prepare_enable(drvdata->clk);
	if (ret)
		return ret;

	byte_cntr_value = readl_relaxed(address);
	clk_disable_unprepare(drvdata->clk);

	return byte_cntr_value;
}

static int csr_enable(struct coresight_device *csdev, enum cs_mode mode,
		       void *data)
{
	struct csr_drvdata *drvdata;
	struct coresight_device *assoc_csdev = (struct coresight_device *)data;
	struct csr_assoc_data *assoc_data = NULL;
	u32 byte_cntr_value;

	drvdata = csdev_to_csr_drvdata(csdev);
	list_for_each_entry(assoc_data, &drvdata->csr_assoc, node) {
		/* Get the assocated data of the device by the device name */
		if (!strcmp(dev_name(&assoc_csdev->dev),
						dev_name(&assoc_data->assoc_csdev->dev))) {
			/* If it is ETR device, start byte-cntr function. */
			if (assoc_data->assoc_csdev->type == CORESIGHT_DEV_TYPE_SINK) {
				byte_cntr_value = csr_read_bytecntr_value(drvdata,
							drvdata->base + assoc_data->byte_cntr_data.byte_cntr_offset);
				if (byte_cntr_value > 0) {
					assoc_data->byte_cntr_data.block_size = byte_cntr_value * 8;
					csr_byte_cntr_start(&assoc_data->byte_cntr_data);
				}
			}
			break;
		}
	}

	return 0;
}

static int csr_disable(struct coresight_device *csdev, void * data)
{
	struct csr_drvdata *drvdata;
	struct csr_assoc_data *assoc_data = NULL;
	struct coresight_device *assoc_csdev = (struct coresight_device *)data;
	u32 byte_cntr_value;

	drvdata = csdev_to_csr_drvdata(csdev);

	list_for_each_entry(assoc_data, &drvdata->csr_assoc, node) {
		if (!strcmp(dev_name(&assoc_csdev->dev),
					dev_name(&assoc_data->assoc_csdev->dev))) {
			if (assoc_data->assoc_csdev->type == CORESIGHT_DEV_TYPE_SINK) {
				byte_cntr_value = csr_read_bytecntr_value(drvdata,
							drvdata->base + assoc_data->byte_cntr_data.byte_cntr_offset);
				if (byte_cntr_value > 0) {
					assoc_data->byte_cntr_data.block_size = 0;
					csr_byte_cntr_stop(&assoc_data->byte_cntr_data);
				}
			}
			break;
		}
	}

	return 0;
}

static const struct coresight_ops_helper csr_helper_ops = {
	.enable = csr_enable,
	.disable = csr_disable,
};

static const struct coresight_ops csr_ops = {
	.helper_ops = &csr_helper_ops,
};

const char *csr_plat_get_node_name(struct fwnode_handle *fwnode)
{
	if (is_of_node(fwnode))
		return of_node_full_name(to_of_node(fwnode));
	return "unknown";
}

static bool csr_plat_node_name_eq(struct fwnode_handle *fwnode,
				  const char *name)
{
	if (is_of_node(fwnode))
		return of_node_name_eq(to_of_node(fwnode), name);
	return false;
}

static void csr_add_assoc_to_csdev(struct coresight_device *csdev)
{
	struct csr_drvdata *drvdata;
	struct csr_assoc_data *assoc_data = NULL;
	const char *name = NULL;

	name = csr_plat_get_node_name(dev_fwnode(csdev->dev.parent));
	list_for_each_entry(drvdata, &csr_list, node) {
		list_for_each_entry(assoc_data, &drvdata->csr_assoc, node) {
			if (!strcmp(assoc_data->assoc_dev_name, name)) {
				assoc_data->assoc_dev_name = dev_name(&csdev->dev);
				assoc_data->assoc_csdev = csdev;
				coresight_add_helper(csdev, drvdata->csdev);
				break;
			}
		}
	}
}

static void csr_remove_assoc_from_csdev(struct coresight_device *csdev)
{
	struct csr_drvdata *drvdata;
	struct csr_assoc_data *assoc_data = NULL;
	const char *name = NULL;

	name = csr_plat_get_node_name(dev_fwnode(csdev->dev.parent));
	list_for_each_entry(drvdata, &csr_list, node) {
		list_for_each_entry(assoc_data, &drvdata->csr_assoc, node) {
			if (!strcmp(assoc_data->assoc_dev_name, name)) {
				assoc_data->assoc_csdev = NULL;
				break;
			}
		}
	}
}

static struct csr_assoc_op csr_assoc_ops = {
	.add = csr_add_assoc_to_csdev,
	.remove = csr_remove_assoc_from_csdev
};

/**
 * Get the data of the assocated device to CSR.
 * @dev:	Device of CSR.
 * @drvdata:	Driver data of CSR.
 */
static int csr_get_assoc_data(struct device *dev, struct csr_drvdata *drvdata)
{
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct fwnode_handle *child = NULL;
	struct fwnode_handle *cs_fwnode = NULL;
	struct csr_assoc_data *assoc_data = NULL;
	struct coresight_device *csdev = NULL;
	u32 dev_type;
	int ret;

	if (IS_ERR_OR_NULL(fwnode))
		return -EINVAL;

	/* Get the data from each child node of the CSR node. */
	fwnode_for_each_child_node(fwnode, child) {
		if (csr_plat_node_name_eq(child, CSR_DT_ASSOC_DEVICE)) {
			assoc_data = devm_kzalloc(dev, sizeof(struct csr_assoc_data), GFP_KERNEL);
			if (!assoc_data)
				return -ENOMEM;

			/* Assocated DT node */
			cs_fwnode = fwnode_find_reference(child, CSR_DT_CSDEV_ASSOC, 0);
			if (IS_ERR(cs_fwnode))
				return -EINVAL;

			/* Find the assocated csdev by fwnode. */
			csdev = coresight_find_csdev_by_fwnode(cs_fwnode);
			if(csdev) {
				assoc_data->assoc_dev_name = dev_name(&csdev->dev);
				assoc_data->assoc_csdev = csdev;
			} else {
				assoc_data->assoc_dev_name = csr_plat_get_node_name(cs_fwnode);
				assoc_data->assoc_csdev = NULL;
			}
			assoc_data->fnode = child;
			ret = fwnode_property_read_u32(child, CSR_DT_CSDEV_ASSOC_DEV_TYPE, &dev_type);
			if (ret)
				return ret;
			/* If assocated device is ETR device. Init all the byte counter data*/
			if (dev_type == CSR_ASSOC_DEV_ETR) {
				assoc_data->dev_type = CSR_ASSOC_DEV_ETR;
				byte_cntr_init(drvdata, assoc_data);
			}
			/* Add assocate data to the list */
			list_add_tail(&assoc_data->node, &drvdata->csr_assoc);
		}
	}

	fwnode_handle_put(child);
	return 0;
}

static int csr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct coresight_platform_data *pdata;
	struct csr_drvdata *drvdata;
	struct resource *res;
	struct coresight_desc desc = { 0 };
	int ret = 0;

	desc.name = coresight_alloc_device_name(&csr_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	pdev->dev.platform_data = pdata;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;
	drvdata->dev = &pdev->dev;
	platform_set_drvdata(pdev, drvdata);

	drvdata->clk = devm_clk_get(dev, "apb_pclk");
	if (IS_ERR(drvdata->clk))
		dev_dbg(dev, "csr not config clk\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "csr-base");
	if (!res)
		return -ENODEV;
	drvdata->pbase = res->start;

	drvdata->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!drvdata->base)
		return -ENOMEM;

	INIT_LIST_HEAD(&drvdata->csr_assoc);
	ret = csr_get_assoc_data(dev, drvdata);
	if (ret)
		return ret;

	desc.type = CORESIGHT_DEV_TYPE_HELPER;
	desc.subtype.helper_subtype = CORESIGHT_DEV_SUBTYPE_HELPER_CSR;
	desc.pdata = pdev->dev.platform_data;
	desc.dev = &pdev->dev;
	desc.groups = csr_attr_grps;
	desc.ops = &csr_ops;

	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev))
		return PTR_ERR(drvdata->csdev);

	list_add(&drvdata->node, &csr_list);
	spin_lock_init(&drvdata->spin_lock);

	dev_info(dev, "CSR initialized: %s\n", dev_name(dev));
	return ret;
}

static int csr_remove(struct platform_device *pdev)
{
	struct csr_drvdata *drvdata = platform_get_drvdata(pdev);
	struct csr_assoc_data *assoc_data = NULL;

	list_for_each_entry(drvdata, &csr_list, node) {
		list_for_each_entry(assoc_data, &drvdata->csr_assoc, node) {
			if (assoc_data->assoc_csdev->type == CORESIGHT_DEV_TYPE_SINK)
				byte_cntr_remove(&assoc_data->byte_cntr_data);
		}
	}

	coresight_unregister(drvdata->csdev);
	return 0;
}

static const struct of_device_id csr_match[] = {
	{.compatible = "qcom,coresight-csr"},
	{}
};

static struct platform_driver csr_driver = {
	.probe          = csr_probe,
	.remove         = csr_remove,
	.driver         = {
		.name   = "coresight-csr",
		.of_match_table = csr_match,
		.suppress_bind_attrs = true,
	},
};

static int __init csr_init(void)
{
	int ret;
	ret = platform_driver_register(&csr_driver);
	coresight_set_csr_ops(&csr_assoc_ops);

	return ret;
}
module_init(csr_init);

static void __exit csr_exit(void)
{
	coresight_remove_csr_ops();
	platform_driver_unregister(&csr_driver);
}
module_exit(csr_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight CSR driver");
