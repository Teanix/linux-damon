/*
 * test_gcma.c - test guaranteed contiguous memory allocator
 *
 * gcma is a contiguous memory allocator which guarantees success and
 * maximum wait time for allocation request.
 * It secure large amount of memory and let it be allocated to the
 * contiguous memory request while it can be used as backend for
 * frontswap and cleancache concurrently.
 *
 * Copyright (C) 2014   Minchan Kim <minchan@kernel.org>
 *                      SeongJae Park <sj38.park@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/gcma.h>
#include <linux/mm.h>

/*********************************
* tunables
**********************************/
/* Enable/disable gcma (disabled by default, fixed at boot for now) */
static bool enabled __read_mostly;
module_param_named(enabled, enabled, bool, 0);

extern void gcma_frontswap_init(unsigned type);
extern int gcma_frontswap_store(unsigned type, pgoff_t offset,
		struct page *page);
extern int gcma_frontswap_load(unsigned type, pgoff_t offset,
		struct page *page);
extern void gcma_frontswap_invalidate_page(unsigned type, pgoff_t offset);

static int test_frontswap(void)
{
	struct page *store_page, *load_page;
	void *store_page_va, *load_page_va;

	gcma_frontswap_init(0);
	store_page = alloc_page(GFP_KERNEL);
	if (!store_page) {
		pr_info("alloc_page failed\n");
	}
	store_page_va = page_address(store_page);
	memset(store_page_va, 1, PAGE_SIZE);
	if (gcma_frontswap_store(0, 17, store_page)) {
		pr_info("failed gcma_frontswap_store call\n");
		return -1;
	}

	load_page = alloc_page(GFP_KERNEL);
	if (!load_page) {
		pr_info("alloc_page for frontswap load op check failed\n");
		return -1;
	}
	if (gcma_frontswap_load(0, 17, load_page)) {
		pr_info("failed gcma_frontswap_load call\n");
		return -1;
	}

	load_page_va = page_address(load_page);
	if (memcmp(store_page_va, load_page_va, PAGE_SIZE)) {
		pr_info("data corrupted\n");
		return -1;
	}

	gcma_frontswap_invalidate_page(0, 17);
	if (!gcma_frontswap_load(0, 17, load_page)) {
		pr_info("invalidated page still alive. test fail\n");
		return -1;
	}

	free_page((unsigned long)store_page_va);
	free_page((unsigned long)load_page_va);
	return 0;
}

static int test_alloc_release_contig(void)
{
	struct page *cma1, *cma2, *cma3;
	cma1 = gcma_alloc_contig(0,5);
	if (!cma1) {
		pr_err("failed to alloc 5 contig pages\n");
		return -1;
	}
	cma2 = gcma_alloc_contig(0, 10);
	if (!cma2) {
		pr_err("failed to alloc 10 contig pages\n");
		return -1;
	}
	cma3 = gcma_alloc_contig(0, 16);
	if (!cma3) {
		pr_err("failed to alloc 16 contig pages\n");
		return -1;
	}

	if (!gcma_release_contig(0, cma2, 10)) {
		pr_err("failed to release 2nd cma\n");
		return -1;
	}
	if (!gcma_release_contig(0, cma1, 5)) {
		pr_err("failed to release 1st cma\n");
		return -1;
	}
	if (!gcma_release_contig(0, cma3, 16)) {
		pr_err("failed to release 3rd cma\n");
		return -1;
	}

	return 0;
}

#define do_test(test)					\
	do {						\
		if (test()) {				\
			pr_err("[FAIL] " #test "\n");	\
			return -1;			\
		}					\
		pr_info("[SUCCESS] " #test "\n");	\
	} while (0)

/*********************************
* module init and exit
**********************************/
static int __init init_gcma(void)
{
	if (!enabled)
		return 0;
	pr_info("test gcma\n");

	do_test(test_alloc_release_contig);
	do_test(test_frontswap);

	return 0;
}

module_init(init_gcma);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Minchan Kim <minchan@kernel.org>");
MODULE_AUTHOR("SeongJae Park <sj38.park@gmail.com>");
MODULE_DESCRIPTION("Test for Guaranteed contiguous memory allocator");
