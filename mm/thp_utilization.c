// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2022  Meta, Inc.
 *  Authors: Alexander Zhu, Johannes Weiner, Rik van Riel
 */

#include <linux/mm.h>
#include <linux/debugfs.h>
#include <linux/highmem.h>
/*
 * The number of utilization buckets THPs will be grouped in
 * under /sys/kernel/debug/thp_utilization.
 */
#define THP_UTIL_BUCKET_NR 10
/*
 * The number of hugepages to scan through on each periodic
 * run of the scanner that generates /sys/kernel/debug/thp_utilization.
 */
#define THP_UTIL_SCAN_SIZE 256

static void thp_utilization_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(thp_utilization_work, thp_utilization_workfn);

struct thp_scan_info_bucket {
	int nr_thps;
	int nr_zero_pages;
};

struct thp_scan_info {
	struct thp_scan_info_bucket buckets[THP_UTIL_BUCKET_NR];
	struct zone *scan_zone;
	struct timespec64 last_scan_duration;
	struct timespec64 last_scan_time;
	unsigned long pfn;
};

/*
 * thp_scan_debugfs is referred to when /sys/kernel/debug/thp_utilization
 * is opened. thp_scan is used to keep track fo the current scan through
 * physical memory.
 */
static struct thp_scan_info thp_scan_debugfs;
static struct thp_scan_info thp_scan;

#ifdef CONFIG_DEBUG_FS
static int thp_utilization_show(struct seq_file *seqf, void *pos)
{
	int i;
	int start;
	int end;

	for (i = 0; i < THP_UTIL_BUCKET_NR; i++) {
		start = i * HPAGE_PMD_NR / THP_UTIL_BUCKET_NR;
		end = (i + 1 == THP_UTIL_BUCKET_NR)
			   ? HPAGE_PMD_NR
			   : ((i + 1) * HPAGE_PMD_NR / THP_UTIL_BUCKET_NR - 1);
		/* The last bucket will need to contain 100 */
		seq_printf(seqf, "Utilized[%d-%d]: %d %d\n", start, end,
			   thp_scan_debugfs.buckets[i].nr_thps,
			   thp_scan_debugfs.buckets[i].nr_zero_pages);
	}

	seq_printf(seqf, "Last Scan Time: %lu.%02lus\n",
		   (unsigned long)thp_scan_debugfs.last_scan_time.tv_sec,
		   (thp_scan_debugfs.last_scan_time.tv_nsec / (NSEC_PER_SEC / 100)));

	seq_printf(seqf, "Last Scan Duration: %lu.%02lus\n",
		   (unsigned long)thp_scan_debugfs.last_scan_duration.tv_sec,
		   (thp_scan_debugfs.last_scan_duration.tv_nsec / (NSEC_PER_SEC / 100)));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(thp_utilization);

static int __init thp_utilization_debugfs(void)
{
	debugfs_create_file("thp_utilization", 0200, NULL, NULL,
			    &thp_utilization_fops);
	return 0;
}
late_initcall(thp_utilization_debugfs);
#endif

static int thp_utilization_bucket(int num_utilized_pages)
{
	int bucket;

	if (num_utilized_pages < 0 || num_utilized_pages > HPAGE_PMD_NR)
		return -1;

	/* Group THPs into utilization buckets */
	bucket = num_utilized_pages * THP_UTIL_BUCKET_NR / HPAGE_PMD_NR;
	return min(bucket, THP_UTIL_BUCKET_NR - 1);
}

static int thp_number_utilized_pages(struct folio *folio)
{
	int thp_nr_utilized_pages = HPAGE_PMD_NR;
	void *kaddr;
	int i;
	bool zero_page;

	if (!folio || !folio_test_anon(folio) || !folio_test_transhuge(folio))
		return -1;

	for (i = 0; i < folio_nr_pages(folio); i++) {
		kaddr = kmap_local_folio(folio, i);
		zero_page = !memchr_inv(kaddr, 0, PAGE_SIZE);

		if (zero_page)
			thp_nr_utilized_pages--;

		kunmap_local(kaddr);
	}

	return thp_nr_utilized_pages;
}

bool can_shrink_thp(struct folio *folio)
{
	int bucket, num_utilized_pages;

	if (!folio || !folio_test_anon(folio) || !folio_test_transhuge(folio))
		return false;

	num_utilized_pages = thp_number_utilized_pages(folio);
	bucket = thp_utilization_bucket(num_utilized_pages);

	return bucket >= 0 && bucket < THP_UTIL_BUCKET_NR - 1;
}

static void thp_scan_next_zone(void)
{
	struct timespec64 current_time;
	bool update_debugfs;
	/*
	 * THP utilization worker thread has reached the end
	 * of the memory zone. Proceed to the next zone.
	 */
	thp_scan.scan_zone = next_zone(thp_scan.scan_zone);
	update_debugfs = !thp_scan.scan_zone;
	thp_scan.scan_zone = update_debugfs ? (first_online_pgdat())->node_zones
			: thp_scan.scan_zone;
	thp_scan.pfn = (thp_scan.scan_zone->zone_start_pfn + HPAGE_PMD_NR - 1)
			& ~(HPAGE_PMD_SIZE - 1);
	if (!update_debugfs)
		return;

	/*
	 * If the worker has scanned through all of physical memory then
	 * update information displayed in /sys/kernel/debug/thp_utilization
	 */
	ktime_get_ts64(&current_time);
	thp_scan_debugfs.last_scan_duration = timespec64_sub(current_time,
							     thp_scan_debugfs.last_scan_time);
	thp_scan_debugfs.last_scan_time = current_time;

	memcpy(&thp_scan_debugfs.buckets, &thp_scan.buckets, sizeof(thp_scan.buckets));
	memset(&thp_scan.buckets, 0, sizeof(thp_scan.buckets));
}

static void thp_util_scan(unsigned long pfn_end)
{
	struct page *page = NULL;
	int bucket, current_pfn, num_utilized_pages;
	int i;
	/*
	 * Scan through each memory zone in chunks of THP_UTIL_SCAN_SIZE
	 * PFNs every second looking for anonymous THPs.
	 */
	for (i = 0; i < THP_UTIL_SCAN_SIZE; i++) {
		current_pfn = thp_scan.pfn;
		thp_scan.pfn += HPAGE_PMD_NR;
		if (current_pfn >= pfn_end)
			return;

		page = pfn_to_online_page(current_pfn);
		if (!page)
			continue;

		num_utilized_pages = thp_number_utilized_pages(page_folio(page));
		bucket = thp_utilization_bucket(num_utilized_pages);
		if (bucket < 0)
			continue;

		if (bucket < THP_UTIL_BUCKET_NR - 1)
			add_underutilized_thp(page);

		thp_scan.buckets[bucket].nr_thps++;
		thp_scan.buckets[bucket].nr_zero_pages += (HPAGE_PMD_NR - num_utilized_pages);
	}
}

static void thp_utilization_workfn(struct work_struct *work)
{
	unsigned long pfn_end;
	/*
	 * Worker function that scans through all of physical memory
	 * for anonymous THPs.
	 */
	if (!thp_scan.scan_zone)
		thp_scan.scan_zone = (first_online_pgdat())->node_zones;

	pfn_end = zone_end_pfn(thp_scan.scan_zone);
	/* If we have reached the end of the zone or end of physical memory
	 * move on to the next zone. Otherwise, scan the next PFNs in the
	 * current zone.
	 */
	if (!managed_zone(thp_scan.scan_zone) || thp_scan.pfn >= pfn_end)
		thp_scan_next_zone();
	else
		thp_util_scan(pfn_end);

	schedule_delayed_work(&thp_utilization_work, HZ);
}

static int __init thp_scan_init(void)
{
	schedule_delayed_work(&thp_utilization_work, HZ);
	return 0;
}
subsys_initcall(thp_scan_init);
