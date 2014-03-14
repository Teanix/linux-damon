/*
 * gcma.c - guaranteed contiguous memory allocator
 *
 * gcma is a contiguous memory allocator which guarantees success and
 * maximum wait time for allocation request.
 * It secure large amount of memory and let it be allocated to the
 * contiguous memory request while it can be used as backend for
 * frontswap and cleancache concurrently.
 *
 * Copyright (C) 2014  Minchan Kim <minchan@kernel.org>
 *                     SeongJae Park <sj38.park@gmail.com>
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
#include <linux/slab.h>
#include <linux/memblock.h>

/*********************************
* tunables
**********************************/
/* Enable/disable gcma (disabled by default, fixed at boot for now) */
static bool gcma_enabled __read_mostly;
module_param_named(enabled, gcma_enabled, bool, 0);

/* Default size of contiguous memory area */
static unsigned long def_cma_bytes __read_mostly = 10000000;

static int __init early_gcma(char *p)
{
	pr_debug("set %s as default cma size\n", p);
	def_cma_bytes = memparse(p, &p);
	return 0;
}
early_param("gcma.def_cma_bytes", early_gcma);

/*********************************
* contiguous memory allocations
**********************************/
#define MAX_CMA	32

/* bitmap of contiguous memory areas.
 * 1 means alloced, 0 means free.
 */
static unsigned long *cma_bitmaps[MAX_CMA];
static unsigned long cma_pfns[MAX_CMA];
static phys_addr_t cma_sizes[MAX_CMA];
static int nr_reserved_cma = 0;

/**
 * gcma_reserve_cma() - Reserve contiguous memory area
 * @size: Size of the reserved area (in bytes), 0 for default size
 *
 * This function reserves memory from early allocator, memblock. It can be
 * called several times(under MAX_CMA) during boot for client code's usage.
 *
 * Returns id of reserved contiguous memory area if success,
 * Returns negative number if failed
 */
int __init gcma_reserve_cma(phys_addr_t size)
{
	phys_addr_t addr;

	pr_debug("%s called with %ld. will alloc %ld bytes as cma\n",
			__func__, (unsigned long)size, def_cma_bytes);
	if (nr_reserved_cma == MAX_CMA) {
		pr_warn("already %d cmas reserved. can't reserve more\n",
				MAX_CMA);
		return -1;
	}
	if (size == 0) {
		pr_debug("size is 0. use default size, %ld bytes\n",
				def_cma_bytes);
		size = def_cma_bytes;
	}

	size = ALIGN(size, PAGE_SIZE << max(MAX_ORDER - 1, pageblock_order));
	pr_debug("aligned size is: %ld bytes\n", (unsigned long)size);

	addr = __memblock_alloc_base(size, PAGE_SIZE,
			MEMBLOCK_ALLOC_ACCESSIBLE);
	if (!addr) {
		pr_debug("failed to reserve cma\n");
		return -ENOMEM;
	}
	pr_debug("%ld bytes cma reserved\n", (unsigned long)size);

	cma_sizes[nr_reserved_cma] = size;
	cma_pfns[nr_reserved_cma] = PFN_DOWN(addr);

	return nr_reserved_cma++;
}

/*********************************
* module init and exit
**********************************/
static int __init init_gcma(void)
{
	int i, bitmap_bytes;

	if (!gcma_enabled)
		return 0;
	pr_info("loading gcma\n");

	for (i = 0; i < nr_reserved_cma; i++) {
		bitmap_bytes = cma_sizes[i] / PAGE_SIZE / sizeof(*cma_bitmaps);
		if ((cma_sizes[i] / PAGE_SIZE) % sizeof(*cma_bitmaps))
			bitmap_bytes += 1;
		cma_bitmaps[i] = kzalloc(bitmap_bytes, GFP_KERNEL);
		if (!cma_bitmaps[i]) {
			pr_debug("failed to alloc bitmap\n");
			return -ENOMEM;
		}
	}
	return 0;
}

module_init(init_gcma);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Minchan Kim <minchan@kernel.org>");
MODULE_AUTHOR("SeongJae Park <sj38.park@gmail.com>");
MODULE_DESCRIPTION("Guaranteed contiguous memory allocator");
