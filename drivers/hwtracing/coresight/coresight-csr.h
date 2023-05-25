/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_CSR_H
#define _CORESIGHT_CSR_H
#include <linux/clk.h>
#include <linux/coresight.h>
#include <linux/kernel.h>
#include <linux/of.h>

#define CSR_BYTECNTVAL		(0x06C)

struct coresight_csr {
	const char *name;
	struct list_head link;
};

/**
 * struct csr_drvdata - specifics for the CSR device.
 * @base: Memory mapped base address for this component.
 * @pbase: Physical address base.
 * @dev: The device entity associated to this component.
 * @csdev: Data struct for coresight device.
 * @csr: CSR struct
 * @clk: Clock of this component.
 * @spin_lock: Spin lock for the data.
 * @set_byte_cntr_support: Support set byte contr value or not.
 */
struct csr_drvdata {
	void __iomem		*base;
	phys_addr_t		pbase;
	struct device		*dev;
	struct coresight_device	*csdev;
	struct coresight_csr	csr;
	struct clk		*clk;
	spinlock_t		spin_lock;
	bool			set_byte_cntr_support;
};

#if IS_ENABLED(CONFIG_CORESIGHT_CSR)
extern void coresight_csr_set_byte_cntr(struct coresight_csr *csr, uint32_t count);
extern struct coresight_csr *coresight_csr_get(const char *name);
#if IS_ENABLED(CONFIG_OF)
extern int of_get_coresight_csr_name(struct device_node *node,
				const char **csr_name);
#else
static inline int of_get_coresight_csr_name(struct device_node *node,
		const char **csr_name){ return -EINVAL; }
#endif
#else
static inline void coresight_csr_set_byte_cntr(struct coresight_csr *csr, int irqctrl_offset,
					   uint32_t count) {}
static inline struct coresight_csr *coresight_csr_get(const char *name)
					{ return NULL; }
#endif
#endif

