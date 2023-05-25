/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_BYTE_CNTR_H
#define _CORESIGHT_BYTE_CNTR_H

#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include "coresight-priv.h"
#include "coresight-tmc.h"

/**
 * struct byte_cntr - Data of ETR's byte_cntr config
 * @dev: cdev of byte_cntr node.
 * @driver_class: class data for the dev node.
 * @enable: byte_cntr enable or not.
 * @read_active: Indicate that data is read from /dev/byte-cntr.
 * @block_size: The counter value of byte_cntr irq.
 * @byte_cntr_irq: irq number.
 * @byte_cntr_lock: lock of the byte_cntr data.
 * @offset: The offset of current read point.
 * @wq: byte_cntr read work queue.
 * @irq_cnt: counter number of the byte_cntr irq.
 * @tmcdrvdata: ETR drvdata.
 */
struct byte_cntr {
	struct cdev		dev;
	struct class	*driver_class;
	bool			enable;
	bool			read_active;
	u32			block_size;
	int			byte_cntr_irq;
	struct mutex		byte_cntr_lock;
	unsigned long		offset;
	wait_queue_head_t	wq;
	atomic_t		irq_cnt;
	struct tmc_drvdata		*tmcdrvdata;
};

struct byte_cntr *byte_cntr_init(struct amba_device *adev,
				 struct tmc_drvdata *drvdata);
void tmc_etr_byte_cntr_start(struct byte_cntr *byte_cntr_data);
void tmc_etr_byte_cntr_stop(struct byte_cntr *byte_cntr_data);


#endif
