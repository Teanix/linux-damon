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
#include <linux/mutex.h>
#include <linux/time64.h>
#include <linux/types.h>

/* Represents a region of [start, end) */
struct damon_addr_range {
	unsigned long start;
	unsigned long end;
};

/* Represents a monitoring target region on the virtual address space */
struct damon_region {
	struct damon_addr_range ar;
	unsigned long sampling_addr;
	unsigned int nr_accesses;
	struct list_head list;

	unsigned int age;
	struct damon_addr_range last_ar;
	unsigned int last_nr_accesses;
};

/* Represents a monitoring target task */
struct damon_task {
	int pid;
	struct list_head regions_list;
	struct list_head list;
};

/* Data Access Monitoring-based Operation Scheme */
enum damos_action {
	DAMOS_WILLNEED,
	DAMOS_COLD,
	DAMOS_PAGEOUT,
	DAMOS_HUGEPAGE,
	DAMOS_NOHUGEPAGE,
	DAMOS_STAT,		/* Do nothing but only record the stat */
	DAMOS_ACTION_LEN,
};

struct damos {
	unsigned int min_sz_region;
	unsigned int max_sz_region;
	unsigned int min_nr_accesses;
	unsigned int max_nr_accesses;
	unsigned int min_age_region;
	unsigned int max_age_region;
	enum damos_action action;
	unsigned long stat_count;
	unsigned long stat_sz;
	struct list_head list;
};

/*
 * For each 'sample_interval', DAMON checks whether each region is accessed or
 * not.  It aggregates and keeps the access information (number of accesses to
 * each region) for 'aggr_interval' time.  DAMON also checks whether the memory
 * mapping of the target tasks has changed (e.g., by mmap() calls from the
 * application) and applies the changes for each 'regions_update_interval'.
 *
 * All time intervals are in micro-seconds.
 */
struct damon_ctx {
	unsigned long sample_interval;
	unsigned long aggr_interval;
	unsigned long regions_update_interval;
	unsigned long min_nr_regions;
	unsigned long max_nr_regions;

	struct timespec64 last_aggregation;
	struct timespec64 last_regions_update;

	unsigned char *rbuf;
	unsigned int rbuf_len;
	unsigned int rbuf_offset;
	char *rfile_path;

	struct task_struct *kdamond;
	bool kdamond_stop;
	struct mutex kdamond_lock;

	struct list_head tasks_list;	/* 'damon_task' objects */
	struct list_head schemes_list;	/* 'damos' objects */

	/* callbacks */
	void (*init_target_regions)(struct damon_ctx *context);
	void (*update_target_regions)(struct damon_ctx *context);
	void (*prepare_access_checks)(struct damon_ctx *context);
	unsigned int (*check_accesses)(struct damon_ctx *context);
	void (*sample_cb)(struct damon_ctx *context);
	void (*aggregate_cb)(struct damon_ctx *context);
};

int damon_set_pids(struct damon_ctx *ctx, int *pids, ssize_t nr_pids);
int damon_set_attrs(struct damon_ctx *ctx, unsigned long sample_int,
		unsigned long aggr_int, unsigned long regions_update_int,
		unsigned long min_nr_reg, unsigned long max_nr_reg);
int damon_set_schemes(struct damon_ctx *ctx,
			struct damos **schemes, ssize_t nr_schemes);
int damon_set_recording(struct damon_ctx *ctx,
				unsigned int rbuf_len, char *rfile_path);
int damon_start(struct damon_ctx *ctx);
int damon_stop(struct damon_ctx *ctx);

#endif
