/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON api
 *
 * Author: SeongJae Park <sjpark@amazon.de>
 */

#ifndef _DAMON_H_
#define _DAMON_H_

#include <linux/mutex.h>
#include <linux/time64.h>
#include <linux/types.h>

/**
 * struct damon_addr_range - Represents an address region of [@start, @end).
 * @start:	Start address of the region (inclusive).
 * @end:	End address of the region (exclusive).
 */
struct damon_addr_range {
	unsigned long start;
	unsigned long end;
};

/**
 * struct damon_region - Represents a monitoring target region.
 * @ar:			The address range of the region.
 * @sampling_addr:	Address of the sample for the next access check.
 * @nr_accesses:	Access frequency of this region.
 * @list:		List head for siblings.
 */
struct damon_region {
	struct damon_addr_range ar;
	unsigned long sampling_addr;
	unsigned int nr_accesses;
	struct list_head list;
};

/**
 * struct damon_target - Represents a monitoring target.
 * @id:			Unique identifier for this target.
 * @regions_list:	Head of the monitoring target regions of this target.
 * @list:		List head for siblings.
 *
 * Each monitoring context could have multiple targets.  For example, a context
 * for virtual memory address spaces could have multiple target processes.  The
 * @id of each target should be unique among the targets of the context.  For
 * example, in the virtual address monitoring context, it could be a pidfd or
 * an address of an mm_struct.
 */
struct damon_target {
	unsigned long id;
	struct list_head regions_list;
	struct list_head list;
};

/**
 * struct damon_ctx - Represents a context for each monitoring.  This is the
 * main interface that allows users to set the attributes and get the results
 * of the monitoring.
 *
 * @sample_interval:		The time between access samplings.
 * @aggr_interval:		The time between monitor results aggregations.
 * @nr_regions:			The number of monitoring regions.
 *
 * For each @sample_interval, DAMON checks whether each region is accessed or
 * not.  It aggregates and keeps the access information (number of accesses to
 * each region) for @aggr_interval time.  All time intervals are in
 * micro-seconds.
 *
 * @kdamond:		Kernel thread who does the monitoring.
 * @kdamond_stop:	Notifies whether kdamond should stop.
 * @kdamond_lock:	Mutex for the synchronizations with @kdamond.
 *
 * For each monitoring context, one kernel thread for the monitoring is
 * created.  The pointer to the thread is stored in @kdamond.
 *
 * Once started, the monitoring thread runs until explicitly required to be
 * terminated or every monitoring target is invalid.  The validity of the
 * targets is checked via the @target_valid callback.  The termination can also
 * be explicitly requested by writing non-zero to @kdamond_stop.  The thread
 * sets @kdamond to NULL when it terminates.  Therefore, users can know whether
 * the monitoring is ongoing or terminated by reading @kdamond.  Reads and
 * writes to @kdamond and @kdamond_stop from outside of the monitoring thread
 * must be protected by @kdamond_lock.
 *
 * Note that the monitoring thread protects only @kdamond and @kdamond_stop via
 * @kdamond_lock.  Accesses to other fields must be protected by themselves.
 *
 * @targets_list:	Head of monitoring targets (&damon_target) list.
 *
 * @init_target_regions:	Constructs initial monitoring target regions.
 * @prepare_access_checks:	Prepares next access check of target regions.
 * @check_accesses:		Checks the access of target regions.
 * @target_valid:		Determine if the target is valid.
 * @cleanup:			Cleans up the context.
 * @sample_cb:			Called for each sampling interval.
 * @aggregate_cb:		Called for each aggregation interval.
 *
 * DAMON can be extended for various address spaces by users.  For this, users
 * can register the target address space dependent low level functions for
 * their usecases via the callback pointers of the context.  The monitoring
 * thread calls @init_target_regions before starting the monitoring, and
 * @prepare_access_checks, @check_accesses, and @target_valid for each
 * @sample_interval.
 *
 * @init_target_regions should construct proper monitoring target regions and
 * link those to the DAMON context struct.
 * @prepare_access_checks should manipulate the monitoring regions to be
 * prepare for the next access check.
 * @check_accesses should check the accesses to each region that made after the
 * last preparation and update the `->nr_accesses` of each region.
 * @target_valid should check whether the target is still valid for the
 * monitoring.
 * @cleanup is called from @kdamond just before its termination.  After this
 * call, only @kdamond_lock and @kdamond will be touched.
 *
 * @sample_cb and @aggregate_cb are called from @kdamond for each of the
 * sampling intervals and aggregation intervals, respectively.  Therefore,
 * users can safely access to the monitoring results via @targets_list without
 * additional protection of @kdamond_lock.  For the reason, users are
 * recommended to use these callback for the accesses to the results.
 */
struct damon_ctx {
	unsigned long sample_interval;
	unsigned long aggr_interval;
	unsigned long nr_regions;

	struct timespec64 last_aggregation;

	struct task_struct *kdamond;
	bool kdamond_stop;
	struct mutex kdamond_lock;

	struct list_head targets_list;	/* 'damon_target' objects */

	/* callbacks */
	void (*init_target_regions)(struct damon_ctx *context);
	void (*prepare_access_checks)(struct damon_ctx *context);
	unsigned int (*check_accesses)(struct damon_ctx *context);
	bool (*target_valid)(struct damon_target *target);
	void (*cleanup)(struct damon_ctx *context);
	void (*sample_cb)(struct damon_ctx *context);
	void (*aggregate_cb)(struct damon_ctx *context);
};

#define damon_next_region(r) \
	(container_of(r->list.next, struct damon_region, list))

#define damon_prev_region(r) \
	(container_of(r->list.prev, struct damon_region, list))

#define damon_for_each_region(r, t) \
	list_for_each_entry(r, &t->regions_list, list)

#define damon_for_each_region_safe(r, next, t) \
	list_for_each_entry_safe(r, next, &t->regions_list, list)

#define damon_for_each_target(t, ctx) \
	list_for_each_entry(t, &(ctx)->targets_list, list)

#define damon_for_each_target_safe(t, next, ctx) \
	list_for_each_entry_safe(t, next, &(ctx)->targets_list, list)

#ifdef CONFIG_DAMON

struct damon_region *damon_new_region(unsigned long start, unsigned long end);
inline void damon_insert_region(struct damon_region *r,
		struct damon_region *prev, struct damon_region *next);
void damon_add_region(struct damon_region *r, struct damon_target *t);
void damon_destroy_region(struct damon_region *r);

struct damon_target *damon_new_target(unsigned long id);
void damon_add_target(struct damon_ctx *ctx, struct damon_target *t);
void damon_free_target(struct damon_target *t);
void damon_destroy_target(struct damon_target *t);
unsigned int damon_nr_regions(struct damon_target *t);

int damon_set_targets(struct damon_ctx *ctx,
		unsigned long *ids, ssize_t nr_ids);
int damon_set_attrs(struct damon_ctx *ctx, unsigned long sample_int,
		unsigned long aggr_int, unsigned long nr_reg);
int damon_start(struct damon_ctx **ctxs, int nr_ctxs);
int damon_stop(struct damon_ctx **ctxs, int nr_ctxs);

#endif	/* CONFIG_DAMON */

#endif
