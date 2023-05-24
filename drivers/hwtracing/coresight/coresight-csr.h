/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_CSR_H
#define _CORESIGHT_CSR_H
#include <linux/clk.h>
#include <linux/coresight.h>
#include <linux/kernel.h>
#include <linux/of.h>

/**
 * struct csr_drvdata - specifics for the CSR device.
 * @base:	Memory mapped base address for this component.
 * @pbase:	Physical address base.
 * @dev:	The device entity associated to this component.
 * @csdev:	Data struct for coresight device.
 * @clk:	Clock of this component.
 * @spin_lock:	Spin lock for the data.
 */
struct csr_drvdata {
	void __iomem		*base;
	phys_addr_t		pbase;
	struct device		*dev;
	struct coresight_device	*csdev;
	struct clk		*clk;
	spinlock_t		spin_lock;
};

#endif

