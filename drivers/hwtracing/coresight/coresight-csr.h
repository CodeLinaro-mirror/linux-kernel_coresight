/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_CSR_H
#define _CORESIGHT_CSR_H

#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/coresight.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/property.h>

#include "coresight-tmc.h"

#define CSR_DT_ASSOC_DEVICE		"assoc_device"
#define CSR_DT_CSDEV_ASSOC		"qcom,cs-dev-assoc"
#define CSR_DT_CSDEV_ASSOC_DEV_TYPE		"qcom,cs-dev-type"
#define CSR_DT_BYTECNTVAL_OFFSET	"qcom,csr-bytecntr-offset"
#define CSR_DT_BYTECNTR_NAME		"qcom,csr-bytecntr-name"
#define CSR_DT_BYTECNTR_CLASS_NAME	"qcom,csr-bytecntr-class-name"
#define CSR_DT_BYTECNTR_IRQ		"byte-cntr-irq"

#define CSR_ETR_BYTECNTVAL		0x06C

/**
 * struct byte_cntr
 * @dev:	cdev for byte_cntr.
 * @driver_class:	device classes.
 * @enable:	Indicates that byte_cntr function is enabled or not.
 * @read_active:	Indicates that byte-cntr node is opened or not.
 * @block_size:	Byte counter value of IRQ.
 * @byte_cntr_irq: IRQ number.
 * @irq_cnt: The irq counter number for the ETR data.
 * @wq:	Workqueue of reading ETR data.
 * @read_work:	Work of reading ETR data.
 * @byte_cntr_lock:	lock of byte_cntr data.
 * @offset: Offset of reading pointer.
 * @byte_cntr_offset:	BYTECNTR IRQ Register offset to the CSR base.
 * @name:	The name of byte cntr device node.
 * @class_name:	Device class name configured in DT.
 */
struct byte_cntr {
	struct cdev		dev;
	struct class	*driver_class;
	bool			enable;
	bool			read_active;
	u32			block_size;
	int			byte_cntr_irq;
	atomic_t		irq_cnt;
	wait_queue_head_t	wq;
	struct work_struct	read_work;
	struct mutex		byte_cntr_lock;
	unsigned long	offset;
	u32 byte_cntr_offset;
	const char		*name;
	const char		*class_name;
};

/**
 * struct csr_assoc_data - Data of assocated device to CSR device.
 * @assoc_dev_name:	Assocated device's name.
 * @assoc_csdev: Assocated csdev.
 * @node:	List node for CSR assocated data list.
 * @dev_type: Type of the assocated device.
 * @fnode:	fwnode of the child node.
 * @byte_cntr_data: byte_cntr_data of the assocated device.
 */
struct csr_assoc_data {
	const char *assoc_dev_name;
	struct coresight_device	*assoc_csdev;
	struct list_head node;
	u32 dev_type;
	struct fwnode_handle *fnode;
	struct byte_cntr byte_cntr_data;
};

/**
 * struct csr_drvdata - specifics for the CSR device.
 * @base:	Memory mapped base address for this component.
 * @pbase:	Physical address base.
 * @dev:	The device entity associated to this component.
 * @csdev:	Data struct for coresight device.
 * @node: List node of the CSR data list.
 * @csr_assoc: Assocated device data list.
 * @clk:	Clock of this component.
 * @spin_lock:	Spin lock for the data.
 * @etr_byte_cntr_value: Byte cntr value of the ETR.
 */
struct csr_drvdata {
	void __iomem		*base;
	phys_addr_t		pbase;
	struct device		*dev;
	struct coresight_device	*csdev;
	struct list_head node;
	struct list_head csr_assoc;
	struct clk		*clk;
	spinlock_t		spin_lock;
	u32		etr_byte_cntr_value;
};

extern void csr_byte_cntr_start(struct byte_cntr *byte_cntr_data);
extern void csr_byte_cntr_stop(struct byte_cntr *byte_cntr_data);
extern int byte_cntr_init(struct csr_drvdata *drvdata, struct csr_assoc_data *assoc_data);
extern void byte_cntr_remove(struct byte_cntr *byte_cntr_data);
#endif

