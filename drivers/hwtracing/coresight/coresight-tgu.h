/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_TGU_H
#define _CORESIGHT_TGU_H

// Register addresses
#define TGU_CONTROL			0x0000
#define TIMER0_STATUS			0x0004
#define COUNTER0_STATUS			0x000C
#define TGU_STATUS			0x0014
#define TGU_LAR				0x0FB0
#define GROUP0				0x0074
#define GROUP1				0x00D4
#define GROUP2				0x0134
#define GROUP3				0x0194

// Maximum values
#define MAX_GROUPS			4
#define MAX_GROUP_SETS			256
#define MAX_CONDITION_SETS		64
#define MAX_SELECT_SETS			64
#define MAX_TIMER_COUNTER_SETS		8

// Calculate compare step addresses
#define COUNTER0_COMPARE_STEP(n)	(0x0048 + 0x1D8 * n)
#define TIMER0_COMPARE_STEP(n)		(0x0040 + 0x1D8 * n)
#define CONDITION_DECODE_STEP(m, n)	(0x0050 + 0x4 * m + 0x1D8 * n)
#define CONDITION_SELECT_STEP(m, n)	(0x0060 + 0x4 * m + 0x1D8 * n)
#define GROUP_REG_STEP(grp, reg, step)	(0x0074 + 0x60 * grp + 0x4 * reg + \
	       				 0x1D8 * step)

#define tgu_writel(drvdata, val, off)	__raw_writel((val), drvdata->base + off)
#define tgu_readl(drvdata, off)		__raw_readl(drvdata->base + off)

// Convert a pointer to custom data structure
#define to_tgu_drvdata(c)		container_of(c, struct tgu_drvdata, tgu)


/**
 * struct trigger_data - Structure for storing trigger data
 * @value: Value of the trigger
 * @index: Index of the trigger data
 * @reg: Register associated with the trigger
 * @step: Step of the trigger
 * @addr: Address of the trigger
 *
 * This structure is used to store data related to TGU, including value, index,
 * associated register, step size, and address.
 */
struct trigger_data {
	unsigned int 			value;
	unsigned int			index;
	unsigned int			reg;
	unsigned int			step;
	unsigned int			addr;
};

/**
 * struct tgu_drvdata - Data structure for a TGU (Trigger Generator Unit) device
 * @base: Memory-mapped base address of the TGU device
 * @dev: Pointer to the associated device structure
 * @csdev: Pointer to the associated coresight device
 * @clk: Pointer to the clock associated with the TGU device
 * @spinlock: Spinlock for handling concurrent access
 * @group_data: Pointer to trigger group data
 * @condition_data: Pointer to trigger condition data
 * @select_data: Pointer to trigger select data
 * @timer_data: Pointer to trigger timer data
 * @counter_data: Pointer to trigger counter data
 * @max_reg: Maximum number of registers
 * @max_step: Maximum step size
 * @max_condition: Maximum number of conditions
 * @max_timer: Maximum number of timers
 * @max_counter: Maximum number of counters
 * @cnt_group: Current count of trigger groups
 * @cnt_select: Current count of trigger selects
 * @cnt_condition: Current count of trigger conditions
 * @cnt_timer: Current count of trigger timers
 * @cnt_counter: Current count of trigger counters
 * @enable: Flag indicating whether the TGU device is enabled
 *
 * This structure defines the data associated with a TGU device, including its base
 * address, device pointers, clock, spinlock for synchronization, trigger data pointers,
 * maximum limits for various trigger-related parameters, and enable status.
 */
struct tgu_drvdata {
	void __iomem			*base;
	struct device			*dev;
	struct coresight_device		*csdev;
	struct clk			*clk;
	spinlock_t			spinlock;
	struct trigger_data		*group_data;
	struct trigger_data		*condition_data;
	struct trigger_data		*select_data;
	struct trigger_data		*timer_data;
	struct trigger_data		*counter_data;
	unsigned int			max_reg;
	unsigned int			max_step;
	unsigned int			max_condition;
	unsigned int			max_timer;
	unsigned int			max_counter;
	unsigned int			cnt_group;
	unsigned int			cnt_select;
	unsigned int			cnt_condition;
	unsigned int			cnt_timer;
	unsigned int			cnt_counter;
	bool				enable;
};

#endif
