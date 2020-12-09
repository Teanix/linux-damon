// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Primitives for The Physical Address Space
 *
 * Author: SeongJae Park <sjpark@amazon.de>
 */

#define pr_fmt(fmt) "damon-pa: " fmt

#include <linux/rmap.h>

#include "prmtv-common.h"

/*
 * Primitives for the physical address space
 */

/*
 * The initial regions construction function for the physical address space.
 *
 * This default version does nothing in actual.  Users should set the initial
 * regions by themselves before passing their damon_ctx to 'damon_start()', or
 * implement their version of this and set '->init_target_regions' of their
 * damon_ctx to point it.
 */
void damon_pa_init_regions(struct damon_ctx *ctx)
{
}

/*
 * The dynamic monitoring target regions update function for the physical
 * address space.
 *
 * This default version does nothing in actual.  Users should update the
 * regions in other callbacks such as '->after_aggregation', or implement their
 * version of this and set the '->init_target_regions' of their damon_ctx to
 * point it.
 */
void damon_pa_update_regions(struct damon_ctx *ctx)
{
}

/* access check functions for physical address based regions */

/*
 * Get a page by pfn if it is in the LRU list.  Otherwise, returns NULL.
 *
 * The body of this function is stollen from the 'page_idle_get_page()'.  We
 * steal rather than reuse it because the code is quite simple.
 */
static struct page *damon_pa_get_page(unsigned long pfn)
{
	struct page *page = pfn_to_online_page(pfn);
	pg_data_t *pgdat;

	if (!page || !PageLRU(page) ||
	    !get_page_unless_zero(page))
		return NULL;

	pgdat = page_pgdat(page);
	spin_lock_irq(&pgdat->lru_lock);
	if (unlikely(!PageLRU(page))) {
		put_page(page);
		page = NULL;
	}
	spin_unlock_irq(&pgdat->lru_lock);
	return page;
}

static bool __damon_pa_mkold(struct page *page, struct vm_area_struct *vma,
		unsigned long addr, void *arg)
{
	damon_va_mkold(vma->vm_mm, addr);
	return true;
}

static void damon_pa_mkold(unsigned long paddr)
{
	struct page *page = damon_pa_get_page(PHYS_PFN(paddr));
	struct rmap_walk_control rwc = {
		.rmap_one = __damon_pa_mkold,
		.anon_lock = page_lock_anon_vma_read,
	};
	bool need_lock;

	if (!page)
		return;

	if (!page_mapped(page) || !page_rmapping(page)) {
		set_page_idle(page);
		put_page(page);
		return;
	}

	need_lock = !PageAnon(page) || PageKsm(page);
	if (need_lock && !trylock_page(page)) {
		put_page(page);
		return;
	}

	rmap_walk(page, &rwc);

	if (need_lock)
		unlock_page(page);
	put_page(page);
}

static void __damon_pa_prepare_access_check(struct damon_ctx *ctx,
					    struct damon_region *r)
{
	r->sampling_addr = damon_rand(r->ar.start, r->ar.end);

	damon_pa_mkold(r->sampling_addr);
}

void damon_pa_prepare_access_checks(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t)
			__damon_pa_prepare_access_check(ctx, r);
	}
}

struct damon_pa_access_chk_result {
	unsigned long page_sz;
	bool accessed;
};

static bool damon_pa_accessed(struct page *page, struct vm_area_struct *vma,
		unsigned long addr, void *arg)
{
	struct damon_pa_access_chk_result *result = arg;

	result->accessed = damon_va_young(vma->vm_mm, addr, &result->page_sz);

	/* If accessed, stop walking */
	return !result->accessed;
}

static bool damon_pa_young(unsigned long paddr, unsigned long *page_sz)
{
	struct page *page = damon_pa_get_page(PHYS_PFN(paddr));
	struct damon_pa_access_chk_result result = {
		.page_sz = PAGE_SIZE,
		.accessed = false,
	};
	struct rmap_walk_control rwc = {
		.arg = &result,
		.rmap_one = damon_pa_accessed,
		.anon_lock = page_lock_anon_vma_read,
	};
	bool need_lock;

	if (!page)
		return false;

	if (!page_mapped(page) || !page_rmapping(page)) {
		if (page_is_idle(page))
			result.accessed = false;
		else
			result.accessed = true;
		put_page(page);
		goto out;
	}

	need_lock = !PageAnon(page) || PageKsm(page);
	if (need_lock && !trylock_page(page)) {
		put_page(page);
		return NULL;
	}

	rmap_walk(page, &rwc);

	if (need_lock)
		unlock_page(page);
	put_page(page);

out:
	*page_sz = result.page_sz;
	return result.accessed;
}

/*
 * Check whether the region was accessed after the last preparation
 *
 * mm	'mm_struct' for the given virtual address space
 * r	the region of physical address space that needs to be checked
 */
static void __damon_pa_check_access(struct damon_ctx *ctx,
				    struct damon_region *r)
{
	static unsigned long last_addr;
	static unsigned long last_page_sz = PAGE_SIZE;
	static bool last_accessed;

	/* If the region is in the last checked page, reuse the result */
	if (ALIGN_DOWN(last_addr, last_page_sz) ==
				ALIGN_DOWN(r->sampling_addr, last_page_sz)) {
		if (last_accessed)
			r->nr_accesses++;
		return;
	}

	last_accessed = damon_pa_young(r->sampling_addr, &last_page_sz);
	if (last_accessed)
		r->nr_accesses++;

	last_addr = r->sampling_addr;
}

unsigned int damon_pa_check_accesses(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;
	unsigned int max_nr_accesses = 0;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t) {
			__damon_pa_check_access(ctx, r);
			max_nr_accesses = max(r->nr_accesses, max_nr_accesses);
		}
	}

	return max_nr_accesses;
}

bool damon_pa_target_valid(void *t)
{
	return true;
}

void damon_pa_set_primitives(struct damon_ctx *ctx)
{
	ctx->primitive.init_target_regions = damon_pa_init_regions;
	ctx->primitive.update_target_regions = damon_pa_update_regions;
	ctx->primitive.prepare_access_checks = damon_pa_prepare_access_checks;
	ctx->primitive.check_accesses = damon_pa_check_accesses;
	ctx->primitive.reset_aggregated = NULL;
	ctx->primitive.target_valid = damon_pa_target_valid;
	ctx->primitive.cleanup = NULL;
	ctx->primitive.apply_scheme = NULL;
}

