// SPDX-License-Identifier: GPL-2.0
/*
 * Data Access Monitor
 *
 * Author: SeongJae Park <sjpark@amazon.de>
 */

#define pr_fmt(fmt) "damon: " fmt

#include <linux/damon.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/slab.h>

/* Minimal region size.  Every damon_region is aligned by this. */
#define MIN_REGION PAGE_SIZE

/*
 * Functions and macros for DAMON data structures
 */

static DEFINE_MUTEX(damon_lock);
static int nr_running_ctxs;

/*
 * Construct a damon_region struct
 *
 * Returns the pointer to the new struct if success, or NULL otherwise
 */
struct damon_region *damon_new_region(unsigned long start, unsigned long end)
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
inline void damon_insert_region(struct damon_region *r,
		struct damon_region *prev, struct damon_region *next)
{
	__list_add(&r->list, &prev->list, &next->list);
}

void damon_add_region(struct damon_region *r, struct damon_target *t)
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

void damon_destroy_region(struct damon_region *r)
{
	damon_del_region(r);
	damon_free_region(r);
}

/*
 * Construct a damon_target struct
 *
 * Returns the pointer to the new struct if success, or NULL otherwise
 */
struct damon_target *damon_new_target(unsigned long id)
{
	struct damon_target *t;

	t = kmalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return NULL;

	t->id = id;
	INIT_LIST_HEAD(&t->regions_list);

	return t;
}

void damon_add_target(struct damon_ctx *ctx, struct damon_target *t)
{
	list_add_tail(&t->list, &ctx->targets_list);
}

static void damon_del_target(struct damon_target *t)
{
	list_del(&t->list);
}

void damon_free_target(struct damon_target *t)
{
	struct damon_region *r, *next;

	damon_for_each_region_safe(r, next, t)
		damon_free_region(r);
	kfree(t);
}

void damon_destroy_target(struct damon_target *t)
{
	damon_del_target(t);
	damon_free_target(t);
}

unsigned int damon_nr_regions(struct damon_target *t)
{
	struct damon_region *r;
	unsigned int nr_regions = 0;

	damon_for_each_region(r, t)
		nr_regions++;

	return nr_regions;
}

/**
 * damon_set_targets() - Set monitoring targets.
 * @ctx:	monitoring context
 * @ids:	array of target ids
 * @nr_ids:	number of entries in @ids
 *
 * This function should not be called while the kdamond is running.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_set_targets(struct damon_ctx *ctx,
		      unsigned long *ids, ssize_t nr_ids)
{
	ssize_t i;
	struct damon_target *t, *next;

	damon_for_each_target_safe(t, next, ctx)
		damon_destroy_target(t);

	for (i = 0; i < nr_ids; i++) {
		t = damon_new_target(ids[i]);
		if (!t) {
			pr_err("Failed to alloc damon_target\n");
			return -ENOMEM;
		}
		damon_add_target(ctx, t);
	}

	return 0;
}

/**
 * damon_set_attrs() - Set attributes for the monitoring.
 * @ctx:		monitoring context
 * @sample_int:		time interval between samplings
 * @aggr_int:		time interval between aggregations
 * @nr_reg:		number of regions
 *
 * This function should not be called while the kdamond is running.
 * Every time interval is in micro-seconds.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_set_attrs(struct damon_ctx *ctx, unsigned long sample_int,
		    unsigned long aggr_int, unsigned long nr_reg)
{
	if (nr_reg < 3) {
		pr_err("nr_regions (%lu) must be at least 3\n",
				nr_reg);
		return -EINVAL;
	}

	ctx->sample_interval = sample_int;
	ctx->aggr_interval = aggr_int;
	ctx->nr_regions = nr_reg;

	return 0;
}

static bool damon_kdamond_running(struct damon_ctx *ctx)
{
	bool running;

	mutex_lock(&ctx->kdamond_lock);
	running = ctx->kdamond != NULL;
	mutex_unlock(&ctx->kdamond_lock);

	return running;
}

static int kdamond_fn(void *data);

/*
 * __damon_start() - Starts monitoring with given context.
 * @ctx:	monitoring context
 *
 * This function should be called while damon_lock is hold.
 *
 * Return: 0 on success, negative error code otherwise.
 */
static int __damon_start(struct damon_ctx *ctx)
{
	int err = -EBUSY;

	mutex_lock(&ctx->kdamond_lock);
	if (!ctx->kdamond) {
		err = 0;
		ctx->kdamond_stop = false;
		ctx->kdamond = kthread_create(kdamond_fn, ctx, "kdamond.%d",
				nr_running_ctxs);
		if (IS_ERR(ctx->kdamond))
			err = PTR_ERR(ctx->kdamond);
		else
			wake_up_process(ctx->kdamond);
	}
	mutex_unlock(&ctx->kdamond_lock);

	return err;
}

/**
 * damon_start() - Starts the monitorings for a given group of contexts.
 * @ctxs:	an array of the pointers for contexts to start monitoring
 * @nr_ctxs:	size of @ctxs
 *
 * This function starts a group of monitoring threads for a group of monitoring
 * contexts.  One thread per each context is created and run in parallel.  The
 * caller should handle synchronization between the threads by itself.  If a
 * group of threads that created by other 'damon_start()' call is currently
 * running, this function does nothing but returns -EBUSY.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_start(struct damon_ctx **ctxs, int nr_ctxs)
{
	int i;
	int err = 0;

	mutex_lock(&damon_lock);
	if (nr_running_ctxs) {
		mutex_unlock(&damon_lock);
		return -EBUSY;
	}

	for (i = 0; i < nr_ctxs; i++) {
		err = __damon_start(ctxs[i]);
		if (err)
			break;
		nr_running_ctxs++;
	}
	mutex_unlock(&damon_lock);

	return err;
}

/*
 * __damon_stop() - Stops monitoring of given context.
 * @ctx:	monitoring context
 *
 * Return: 0 on success, negative error code otherwise.
 */
static int __damon_stop(struct damon_ctx *ctx)
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
 * damon_stop() - Stops the monitorings for a given group of contexts.
 * @ctxs:	an array of the pointers for contexts to stop monitoring
 * @nr_ctxs:	size of @ctxs
 *
 * Return: 0 on success, negative error code otherwise.
 */
int damon_stop(struct damon_ctx **ctxs, int nr_ctxs)
{
	int i, err = 0;

	for (i = 0; i < nr_ctxs; i++) {
		/* nr_running_ctxs is decremented in kdamond_fn */
		err = __damon_stop(ctxs[i]);
		if (err)
			return err;
	}

	return err;
}

/*
 * Functions for DAMON core logics
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
 * Reset the aggregated monitoring results ('nr_accesses' of each region).
 */
static void kdamond_reset_aggregated(struct damon_ctx *c)
{
	struct damon_target *t;

	damon_for_each_target(t, c) {
		struct damon_region *r;

		damon_for_each_region(r, t)
			r->nr_accesses = 0;
	}
}

/*
 * Check whether current monitoring should be stopped
 *
 * The monitoring is stopped when either the user requested to stop, or all
 * monitoring targets are invalid.
 *
 * Returns true if need to stop current monitoring.
 */
static bool kdamond_need_stop(struct damon_ctx *ctx)
{
	struct damon_target *t;
	bool stop;

	mutex_lock(&ctx->kdamond_lock);
	stop = ctx->kdamond_stop;
	mutex_unlock(&ctx->kdamond_lock);
	if (stop)
		return true;

	if (!ctx->primitive.target_valid)
		return false;

	damon_for_each_target(t, ctx) {
		if (ctx->primitive.target_valid(t))
			return false;
	}

	return true;
}

static void set_kdamond_stop(struct damon_ctx *ctx, bool stop)
{
	mutex_lock(&ctx->kdamond_lock);
	ctx->kdamond_stop = stop;
	mutex_unlock(&ctx->kdamond_lock);
}

#define kdamond_call_prmt(ctx, fn)			\
	do {						\
		if (ctx->primitive.fn)			\
			ctx->primitive.fn(ctx);		\
	} while (0)

#define kdamond_callback(ctx, fn)				\
	do {							\
		if (ctx->callback.fn && ctx->callback.fn(ctx))	\
			set_kdamond_stop(ctx, true);		\
	} while (0)

/*
 * The monitoring daemon that runs as a kernel thread
 */
static int kdamond_fn(void *data)
{
	struct damon_ctx *ctx = (struct damon_ctx *)data;
	struct damon_target *t;
	struct damon_region *r, *next;

	pr_info("kdamond (%d) starts\n", ctx->kdamond->pid);

	kdamond_call_prmt(ctx, init_target_regions);
	kdamond_callback(ctx, before_start);

	while (!kdamond_need_stop(ctx)) {
		kdamond_call_prmt(ctx, prepare_access_checks);
		kdamond_callback(ctx, after_sampling);

		usleep_range(ctx->sample_interval, ctx->sample_interval + 1);

		kdamond_call_prmt(ctx, check_accesses);

		if (kdamond_aggregate_interval_passed(ctx)) {
			kdamond_callback(ctx, after_aggregation);
			kdamond_reset_aggregated(ctx);
		}
	}
	damon_for_each_target(t, ctx) {
		damon_for_each_region_safe(r, next, t)
			damon_destroy_region(r);
	}

	kdamond_callback(ctx, before_terminate);
	kdamond_call_prmt(ctx, cleanup);

	pr_debug("kdamond (%d) finishes\n", ctx->kdamond->pid);
	mutex_lock(&ctx->kdamond_lock);
	ctx->kdamond = NULL;
	mutex_unlock(&ctx->kdamond_lock);

	mutex_lock(&damon_lock);
	nr_running_ctxs--;
	mutex_unlock(&damon_lock);

	do_exit(0);
}
