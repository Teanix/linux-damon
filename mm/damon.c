// SPDX-License-Identifier: GPL-2.0
/*
 * Data Access Monitor
 *
 * Copyright 2019-2020 Amazon.com, Inc. or its affiliates.
 *
 * Author: SeongJae Park <sjpark@amazon.de>
 *
 * This file is constructed in below parts.
 *
 * - Functions and macros for DAMON data structures
 * - Functions for DAMON core logics and features
 * - Functions for the DAMON programming interface
 * - Functions for the module loading/unloading
 */

#define pr_fmt(fmt) "damon: " fmt

#include <linux/damon.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/page_idle.h>
#include <linux/random.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/slab.h>

/* Minimal region size.  Every damon_region is aligned by this. */
#define MIN_REGION PAGE_SIZE

/*
 * Functions and macros for DAMON data structures
 */

#define damon_get_task_struct(t) \
	(get_pid_task(find_vpid(t->pid), PIDTYPE_PID))

#define damon_next_region(r) \
	(container_of(r->list.next, struct damon_region, list))

#define damon_prev_region(r) \
	(container_of(r->list.prev, struct damon_region, list))

#define damon_for_each_region(r, t) \
	list_for_each_entry(r, &t->regions_list, list)

#define damon_for_each_region_safe(r, next, t) \
	list_for_each_entry_safe(r, next, &t->regions_list, list)

#define damon_for_each_task(t, ctx) \
	list_for_each_entry(t, &(ctx)->tasks_list, list)

#define damon_for_each_task_safe(t, next, ctx) \
	list_for_each_entry_safe(t, next, &(ctx)->tasks_list, list)

/* Get a random number in [l, r) */
#define damon_rand(l, r) (l + prandom_u32() % (r - l))

/*
 * Construct a damon_region struct
 *
 * Returns the pointer to the new struct if success, or NULL otherwise
 */
static struct damon_region *damon_new_region(struct damon_ctx *ctx,
				unsigned long start, unsigned long end)
{
	struct damon_region *region;

	region = kmalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return NULL;

	region->ar.start = start;
	region->ar.end = end;
	region->nr_accesses = 0;
	INIT_LIST_HEAD(&region->list);

	return region;
}

/*
 * Add a region between two other regions
 */
static inline void damon_insert_region(struct damon_region *r,
		struct damon_region *prev, struct damon_region *next)
{
	__list_add(&r->list, &prev->list, &next->list);
}

static void damon_add_region(struct damon_region *r, struct damon_task *t)
{
	list_add_tail(&r->list, &t->regions_list);
}

static void damon_del_region(struct damon_region *r)
{
	list_del(&r->list);
}

static void damon_free_region(struct damon_region *r)
{
	kfree(r);
}

static void damon_destroy_region(struct damon_region *r)
{
	damon_del_region(r);
	damon_free_region(r);
}

/*
 * Construct a damon_task struct
 *
 * Returns the pointer to the new struct if success, or NULL otherwise
 */
static struct damon_task *damon_new_task(int pid)
{
	struct damon_task *t;

	t = kmalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return NULL;

	t->pid = pid;
	INIT_LIST_HEAD(&t->regions_list);

	return t;
}

static void damon_add_task(struct damon_ctx *ctx, struct damon_task *t)
{
	list_add_tail(&t->list, &ctx->tasks_list);
}

static void damon_del_task(struct damon_task *t)
{
	list_del(&t->list);
}

static void damon_free_task(struct damon_task *t)
{
	struct damon_region *r, *next;

	damon_for_each_region_safe(r, next, t)
		damon_free_region(r);
	kfree(t);
}

static void damon_destroy_task(struct damon_task *t)
{
	damon_del_task(t);
	damon_free_task(t);
}

static unsigned int nr_damon_tasks(struct damon_ctx *ctx)
{
	struct damon_task *t;
	unsigned int nr_tasks = 0;

	damon_for_each_task(t, ctx)
		nr_tasks++;

	return nr_tasks;
}

static unsigned int nr_damon_regions(struct damon_task *t)
{
	struct damon_region *r;
	unsigned int nr_regions = 0;

	damon_for_each_region(r, t)
		nr_regions++;

	return nr_regions;
}

/*
 * Functions for DAMON core logics and features
 */

/*
 * damon_check_reset_time_interval() - Check if a time interval is elapsed.
 * @baseline:	the time to check whether the interval has elapsed since
 * @interval:	the time interval (microseconds)
 *
 * See whether the given time interval has passed since the given baseline
 * time.  If so, it also updates the baseline to current time for next check.
 *
 * Return:	true if the time interval has passed, or false otherwise.
 */
static bool damon_check_reset_time_interval(struct timespec64 *baseline,
		unsigned long interval)
{
	struct timespec64 now;

	ktime_get_coarse_ts64(&now);
	if ((timespec64_to_ns(&now) - timespec64_to_ns(baseline)) <
			interval * 1000)
		return false;
	*baseline = now;
	return true;
}

/*
 * Check whether it is time to flush the aggregated information
 */
static bool kdamond_aggregate_interval_passed(struct damon_ctx *ctx)
{
	return damon_check_reset_time_interval(&ctx->last_aggregation,
			ctx->aggr_interval);
}

/*
 * Reset the aggregated monitoring results
 */
static void kdamond_reset_aggregated(struct damon_ctx *c)
{
	struct damon_task *t;
	struct damon_region *r;

	damon_for_each_task(t, c) {
		damon_for_each_region(r, t)
			r->nr_accesses = 0;
	}
}

#define sz_damon_region(r) (r->ar.end - r->ar.start)

/*
 * Merge two adjacent regions into one region
 */
static void damon_merge_two_regions(struct damon_region *l,
				struct damon_region *r)
{
	l->nr_accesses = (l->nr_accesses * sz_damon_region(l) +
			r->nr_accesses * sz_damon_region(r)) /
			(sz_damon_region(l) + sz_damon_region(r));
	l->ar.end = r->ar.end;
	damon_destroy_region(r);
}

#define diff_of(a, b) (a > b ? a - b : b - a)

/*
 * Merge adjacent regions having similar access frequencies
 *
 * t		task affected by merge operation
 * thres	'->nr_accesses' diff threshold for the merge
 */
static void damon_merge_regions_of(struct damon_task *t, unsigned int thres)
{
	struct damon_region *r, *prev = NULL, *next;

	damon_for_each_region_safe(r, next, t) {
		if (prev && prev->ar.end == r->ar.start &&
		    diff_of(prev->nr_accesses, r->nr_accesses) <= thres)
			damon_merge_two_regions(prev, r);
		else
			prev = r;
	}
}

/*
 * Merge adjacent regions having similar access frequencies
 *
 * threshold	'->nr_accesses' diff threshold for the merge
 *
 * This function merges monitoring target regions which are adjacent and their
 * access frequencies are similar.  This is for minimizing the monitoring
 * overhead under the dynamically changeable access pattern.  If a merge was
 * unnecessarily made, later 'kdamond_split_regions()' will revert it.
 */
static void kdamond_merge_regions(struct damon_ctx *c, unsigned int threshold)
{
	struct damon_task *t;

	damon_for_each_task(t, c)
		damon_merge_regions_of(t, threshold);
}

/*
 * Split a region in two
 *
 * r		the region to be split
 * sz_r		size of the first sub-region that will be made
 */
static void damon_split_region_at(struct damon_ctx *ctx,
				  struct damon_region *r, unsigned long sz_r)
{
	struct damon_region *new;

	new = damon_new_region(ctx, r->ar.start + sz_r, r->ar.end);
	r->ar.end = new->ar.start;

	damon_insert_region(new, r, damon_next_region(r));
}

/* Split every region in the given task into 'nr_subs' regions */
static void damon_split_regions_of(struct damon_ctx *ctx,
				     struct damon_task *t, int nr_subs)
{
	struct damon_region *r, *next;
	unsigned long sz_region, sz_sub = 0;
	int i;

	damon_for_each_region_safe(r, next, t) {
		sz_region = r->ar.end - r->ar.start;

		for (i = 0; i < nr_subs - 1 &&
				sz_region > 2 * MIN_REGION; i++) {
			/*
			 * Randomly select size of left sub-region to be at
			 * least 10 percent and at most 90% of original region
			 */
			sz_sub = ALIGN_DOWN(damon_rand(1, 10) *
					sz_region / 10, MIN_REGION);
			/* Do not allow blank region */
			if (sz_sub == 0 || sz_sub >= sz_region)
				continue;

			damon_split_region_at(ctx, r, sz_sub);
			sz_region = sz_sub;
		}
	}
}

/*
 * splits every target region into two randomly-sized regions
 *
 * This function splits every target region into two random-sized regions if
 * current total number of the regions is equal or smaller than half of the
 * user-specified maximum number of regions.  This is for maximizing the
 * monitoring accuracy under the dynamically changeable access patterns.  If a
 * split was unnecessarily made, later 'kdamond_merge_regions()' will revert
 * it.
 */
static void kdamond_split_regions(struct damon_ctx *ctx)
{
	struct damon_task *t;
	unsigned int nr_regions = 0;
	static unsigned int last_nr_regions;
	int nr_subregions = 2;

	damon_for_each_task(t, ctx)
		nr_regions += nr_damon_regions(t);

	if (nr_regions > ctx->max_nr_regions / 2)
		return;

	/* If number of regions is not changed, we are maybe in corner case */
	if (last_nr_regions == nr_regions &&
			nr_regions < ctx->max_nr_regions / 3)
		nr_subregions = 3;

	damon_for_each_task(t, ctx)
		damon_split_regions_of(ctx, t, nr_subregions);

	last_nr_regions = nr_regions;
}

/*
 * Check whether current monitoring should be stopped
 *
 * The monitoring is stopped when either the user requested to stop, or all
 * monitoring target tasks are dead.
 *
 * Returns true if need to stop current monitoring.
 */
static bool kdamond_need_stop(struct damon_ctx *ctx)
{
	struct damon_task *t;
	struct task_struct *task;
	bool stop;

	mutex_lock(&ctx->kdamond_lock);
	stop = ctx->kdamond_stop;
	mutex_unlock(&ctx->kdamond_lock);
	if (stop)
		return true;

	damon_for_each_task(t, ctx) {
		/* -1 is reserved for non-process bounded monitoring */
		if (t->pid == -1)
			return false;

		task = damon_get_task_struct(t);
		if (task) {
			put_task_struct(task);
			return false;
		}
	}

	return true;
}

/*
 * The monitoring daemon that runs as a kernel thread
 */
static int kdamond_fn(void *data)
{
	struct damon_ctx *ctx = (struct damon_ctx *)data;
	struct damon_task *t;
	struct damon_region *r, *next;
	unsigned int max_nr_accesses = 0;

	pr_info("kdamond (%d) starts\n", ctx->kdamond->pid);
	if (ctx->init_target_regions)
		ctx->init_target_regions(ctx);
	while (!kdamond_need_stop(ctx)) {
		if (ctx->prepare_access_checks)
			ctx->prepare_access_checks(ctx);
		if (ctx->sample_cb)
			ctx->sample_cb(ctx);

		usleep_range(ctx->sample_interval, ctx->sample_interval + 1);

		if (ctx->check_accesses)
			max_nr_accesses = ctx->check_accesses(ctx);

		if (kdamond_aggregate_interval_passed(ctx)) {
			if (ctx->aggregate_cb)
				ctx->aggregate_cb(ctx);
			kdamond_merge_regions(ctx, max_nr_accesses / 10);
			kdamond_reset_aggregated(ctx);
			kdamond_split_regions(ctx);
		}
	}
	damon_for_each_task(t, ctx) {
		damon_for_each_region_safe(r, next, t)
			damon_destroy_region(r);
	}
	pr_debug("kdamond (%d) finishes\n", ctx->kdamond->pid);
	mutex_lock(&ctx->kdamond_lock);
	ctx->kdamond = NULL;
	mutex_unlock(&ctx->kdamond_lock);

	do_exit(0);
}

/*
 * Functions for the DAMON programming interface
 */

static bool damon_kdamond_running(struct damon_ctx *ctx)
{
	bool running;

	mutex_lock(&ctx->kdamond_lock);
	running = ctx->kdamond != NULL;
	mutex_unlock(&ctx->kdamond_lock);

	return running;
}

/**
 * damon_start() - Starts monitoring with given context.
 * @ctx:	monitoring context
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_start(struct damon_ctx *ctx)
{
	int err = -EBUSY;

	mutex_lock(&ctx->kdamond_lock);
	if (!ctx->kdamond) {
		err = 0;
		ctx->kdamond_stop = false;
		ctx->kdamond = kthread_run(kdamond_fn, ctx, "kdamond");
		if (IS_ERR(ctx->kdamond))
			err = PTR_ERR(ctx->kdamond);
	}
	mutex_unlock(&ctx->kdamond_lock);

	return err;
}

/**
 * damon_stop() - Stops monitoring of given context.
 * @ctx:	monitoring context
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_stop(struct damon_ctx *ctx)
{
	mutex_lock(&ctx->kdamond_lock);
	if (ctx->kdamond) {
		ctx->kdamond_stop = true;
		mutex_unlock(&ctx->kdamond_lock);
		while (damon_kdamond_running(ctx))
			usleep_range(ctx->sample_interval,
					ctx->sample_interval * 2);
		return 0;
	}
	mutex_unlock(&ctx->kdamond_lock);

	return -EPERM;
}

/**
 * damon_set_pids() - Set monitoring target processes.
 * @ctx:	monitoring context
 * @pids:	array of target processes pids
 * @nr_pids:	number of entries in @pids
 *
 * This function should not be called while the kdamond is running.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_set_pids(struct damon_ctx *ctx, int *pids, ssize_t nr_pids)
{
	ssize_t i;
	struct damon_task *t, *next;

	damon_for_each_task_safe(t, next, ctx)
		damon_destroy_task(t);

	for (i = 0; i < nr_pids; i++) {
		t = damon_new_task(pids[i]);
		if (!t) {
			pr_err("Failed to alloc damon_task\n");
			return -ENOMEM;
		}
		damon_add_task(ctx, t);
	}

	return 0;
}

/**
 * damon_set_attrs() - Set attributes for the monitoring.
 * @ctx:		monitoring context
 * @sample_int:		time interval between samplings
 * @aggr_int:		time interval between aggregations
 * @min_nr_reg:		minimal number of regions
 * @max_nr_reg:		maximum number of regions
 *
 * This function should not be called while the kdamond is running.
 * Every time interval is in micro-seconds.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_set_attrs(struct damon_ctx *ctx,
		    unsigned long sample_int, unsigned long aggr_int,
		    unsigned long min_nr_reg, unsigned long max_nr_reg)
{
	if (min_nr_reg < 3) {
		pr_err("min_nr_regions (%lu) must be at least 3\n",
				min_nr_reg);
		return -EINVAL;
	}
	if (min_nr_reg > max_nr_reg) {
		pr_err("invalid nr_regions.  min (%lu) > max (%lu)\n",
				min_nr_reg, max_nr_reg);
		return -EINVAL;
	}

	ctx->sample_interval = sample_int;
	ctx->aggr_interval = aggr_int;
	ctx->min_nr_regions = min_nr_reg;
	ctx->max_nr_regions = max_nr_reg;

	return 0;
}

/*
 * Functions for the module loading/unloading
 */

static int __init damon_init(void)
{
	return 0;
}

static void __exit damon_exit(void)
{
}

module_init(damon_init);
module_exit(damon_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SeongJae Park <sjpark@amazon.de>");
MODULE_DESCRIPTION("DAMON: Data Access MONitor");
