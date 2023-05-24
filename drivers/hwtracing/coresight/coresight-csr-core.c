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

DEFINE_CORESIGHT_DEVLIST(csr_devs, "csr");

static int csr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct coresight_platform_data *pdata;
	struct csr_drvdata *drvdata;
	struct resource *res;
	struct coresight_desc desc = { 0 };

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

	desc.type = CORESIGHT_DEV_TYPE_HELPER;
	desc.pdata = pdev->dev.platform_data;
	desc.dev = &pdev->dev;

	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev))
		return PTR_ERR(drvdata->csdev);

	spin_lock_init(&drvdata->spin_lock);

	dev_dbg(dev, "CSR initialized: %s\n", dev_name(dev));
	return 0;
}

static int csr_remove(struct platform_device *pdev)
{
	struct csr_drvdata *drvdata = platform_get_drvdata(pdev);

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
	return platform_driver_register(&csr_driver);
}
module_init(csr_init);

static void __exit csr_exit(void)
{
	platform_driver_unregister(&csr_driver);
}
module_exit(csr_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight CSR driver");
