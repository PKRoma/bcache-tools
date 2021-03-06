#ifndef _BCACHE_STATS_H_
#define _BCACHE_STATS_H_

#include "stats_types.h"

struct bch_fs;
struct cached_dev;
struct bcache_device;

#ifndef NO_BCACHE_ACCOUNTING

void bch_cache_accounting_init(struct cache_accounting *, struct closure *);
int bch_cache_accounting_add_kobjs(struct cache_accounting *, struct kobject *);
void bch_cache_accounting_clear(struct cache_accounting *);
void bch_cache_accounting_destroy(struct cache_accounting *);

#else

static inline void bch_cache_accounting_init(struct cache_accounting *acc,
					     struct closure *cl) {}
static inline int bch_cache_accounting_add_kobjs(struct cache_accounting *acc,
						 struct kobject *cl)
{
	return 0;
}
static inline void bch_cache_accounting_clear(struct cache_accounting *acc) {}
static inline void bch_cache_accounting_destroy(struct cache_accounting *acc) {}

#endif

static inline void mark_cache_stats(struct cache_stat_collector *stats,
				    bool hit, bool bypass)
{
	atomic_inc(&stats->cache_hit_array[!bypass][!hit]);
}

static inline void bch_mark_cache_accounting(struct bch_fs *c,
					     struct cached_dev *dc,
					     bool hit, bool bypass)
{
	mark_cache_stats(&dc->accounting.collector, hit, bypass);
	mark_cache_stats(&c->accounting.collector, hit, bypass);
}

static inline void bch_mark_sectors_bypassed(struct bch_fs *c,
					     struct cached_dev *dc,
					     unsigned sectors)
{
	atomic_add(sectors, &dc->accounting.collector.sectors_bypassed);
	atomic_add(sectors, &c->accounting.collector.sectors_bypassed);
}

static inline void bch_mark_gc_write(struct bch_fs *c, int sectors)
{
	atomic_add(sectors, &c->accounting.collector.gc_write_sectors);
}

static inline void bch_mark_foreground_write(struct bch_fs *c, int sectors)
{
	atomic_add(sectors, &c->accounting.collector.foreground_write_sectors);
}

static inline void bch_mark_discard(struct bch_fs *c, int sectors)
{
	atomic_add(sectors, &c->accounting.collector.discard_sectors);
}

#endif /* _BCACHE_STATS_H_ */
