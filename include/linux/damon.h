/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON api
 *
 * Copyright 2019-2020 Amazon.com, Inc. or its affiliates.
 *
 * Author: SeongJae Park <sjpark@amazon.de>
 */

#ifndef _DAMON_H_
#define _DAMON_H_

#include <linux/random.h>
#include <linux/types.h>

/**
 * struct damon_region - Represents a monitoring target region of
 * [@vm_start, @vm_end).
 *
 * @vm_start:		Start address of the region (inclusive).
 * @vm_end:		End address of the region (exclusive).
 * @sampling_addr:	Address of the sample for the next access check.
 * @nr_accesses:	Access frequency of this region.
 * @list:		List head for siblings.
 */
struct damon_region {
	unsigned long vm_start;
	unsigned long vm_end;
	unsigned long sampling_addr;
	unsigned int nr_accesses;
	struct list_head list;
};

/**
 * struct damon_task - Represents a monitoring target task.
 * @pid:		Process id of the task.
 * @regions_list:	Head of the monitoring target regions of this task.
 * @list:		List head for siblings.
 */
struct damon_task {
	int pid;
	struct list_head regions_list;
	struct list_head list;
};

/**
 * struct damon_ctx - Represents a context for each monitoring.
 * @tasks_list:		Head of monitoring target tasks (&damon_task) list.
 */
struct damon_ctx {
	struct list_head tasks_list;	/* 'damon_task' objects */
};

#endif
