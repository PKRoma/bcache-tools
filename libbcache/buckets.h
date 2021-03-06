/*
 * Code for manipulating bucket marks for garbage collection.
 *
 * Copyright 2014 Datera, Inc.
 */

#ifndef _BUCKETS_H
#define _BUCKETS_H

#include "buckets_types.h"
#include "super.h"

#define for_each_bucket(b, ca)					\
	for (b = (ca)->buckets + (ca)->mi.first_bucket;		\
	     b < (ca)->buckets + (ca)->mi.nbuckets; b++)

#define bucket_cmpxchg(g, new, expr)				\
({								\
	u64 _v = READ_ONCE((g)->_mark.counter);			\
	struct bucket_mark _old;				\
								\
	do {							\
		(new).counter = _old.counter = _v;		\
		expr;						\
	} while ((_v = cmpxchg(&(g)->_mark.counter,		\
			       _old.counter,			\
			       (new).counter)) != _old.counter);\
	_old;							\
})

/*
 * bucket_gc_gen() returns the difference between the bucket's current gen and
 * the oldest gen of any pointer into that bucket in the btree.
 */

static inline u8 bucket_gc_gen(struct bch_dev *ca, struct bucket *g)
{
	unsigned long r = g - ca->buckets;
	return g->mark.gen - ca->oldest_gens[r];
}

static inline size_t PTR_BUCKET_NR(const struct bch_dev *ca,
				   const struct bch_extent_ptr *ptr)
{
	return sector_to_bucket(ca, ptr->offset);
}

/*
 * Returns 0 if no pointers or device offline - only for tracepoints!
 */
static inline size_t PTR_BUCKET_NR_TRACE(const struct bch_fs *c,
					 const struct bkey_i *k,
					 unsigned ptr)
{
	size_t bucket = 0;
#if 0
	if (bkey_extent_is_data(&k->k)) {
		const struct bch_extent_ptr *ptr;

		extent_for_each_ptr(bkey_i_to_s_c_extent(k), ptr) {
			const struct bch_dev *ca = c->devs[ptr->dev];
			bucket = PTR_BUCKET_NR(ca, ptr);
			break;
		}
	}
#endif
	return bucket;
}

static inline struct bucket *PTR_BUCKET(const struct bch_dev *ca,
					const struct bch_extent_ptr *ptr)
{
	return ca->buckets + PTR_BUCKET_NR(ca, ptr);
}

static inline u8 __gen_after(u8 a, u8 b)
{
	u8 r = a - b;

	return r > 128U ? 0 : r;
}

static inline u8 gen_after(u8 a, u8 b)
{
	u8 r = a - b;

	BUG_ON(r > 128U);

	return r;
}

/**
 * ptr_stale() - check if a pointer points into a bucket that has been
 * invalidated.
 */
static inline u8 ptr_stale(const struct bch_dev *ca,
			   const struct bch_extent_ptr *ptr)
{
	return gen_after(PTR_BUCKET(ca, ptr)->mark.gen, ptr->gen);
}

/* bucket heaps */

static inline bool bucket_min_cmp(struct bucket_heap_entry l,
				  struct bucket_heap_entry r)
{
	return l.val < r.val;
}

static inline bool bucket_max_cmp(struct bucket_heap_entry l,
				  struct bucket_heap_entry r)
{
	return l.val > r.val;
}

static inline void bucket_heap_push(struct bch_dev *ca, struct bucket *g,
				    unsigned long val)
{
	struct bucket_heap_entry new = { g, val };

	if (!heap_full(&ca->heap))
		heap_add(&ca->heap, new, bucket_min_cmp);
	else if (bucket_min_cmp(new, heap_peek(&ca->heap))) {
		ca->heap.data[0] = new;
		heap_sift(&ca->heap, 0, bucket_min_cmp);
	}
}

/* bucket gc marks */

/* The dirty and cached sector counts saturate. If this occurs,
 * reference counting alone will not free the bucket, and a btree
 * GC must be performed. */
#define GC_MAX_SECTORS_USED ((1U << 15) - 1)

static inline bool bucket_unused(struct bucket *g)
{
	return !g->mark.counter;
}

static inline unsigned bucket_sectors_used(struct bucket *g)
{
	return g->mark.dirty_sectors + g->mark.cached_sectors;
}

/* Per device stats: */

struct bch_dev_usage __bch_dev_usage_read(struct bch_dev *);
struct bch_dev_usage bch_dev_usage_read(struct bch_dev *);

static inline u64 __dev_buckets_available(struct bch_dev *ca,
					  struct bch_dev_usage stats)
{
	return max_t(s64, 0,
		     ca->mi.nbuckets - ca->mi.first_bucket -
		     stats.buckets_dirty -
		     stats.buckets_alloc -
		     stats.buckets_meta);
}

/*
 * Number of reclaimable buckets - only for use by the allocator thread:
 */
static inline u64 dev_buckets_available(struct bch_dev *ca)
{
	return __dev_buckets_available(ca, bch_dev_usage_read(ca));
}

static inline u64 __dev_buckets_free(struct bch_dev *ca,
				       struct bch_dev_usage stats)
{
	return __dev_buckets_available(ca, stats) +
		fifo_used(&ca->free[RESERVE_NONE]) +
		fifo_used(&ca->free_inc);
}

static inline u64 dev_buckets_free(struct bch_dev *ca)
{
	return __dev_buckets_free(ca, bch_dev_usage_read(ca));
}

/* Cache set stats: */

struct bch_fs_usage __bch_fs_usage_read(struct bch_fs *);
struct bch_fs_usage bch_fs_usage_read(struct bch_fs *);
void bch_fs_usage_apply(struct bch_fs *, struct bch_fs_usage *,
			struct disk_reservation *, struct gc_pos);

static inline u64 __bch_fs_sectors_used(struct bch_fs *c)
{
	struct bch_fs_usage stats = __bch_fs_usage_read(c);
	u64 reserved = stats.persistent_reserved +
		stats.online_reserved;

	return stats.s[S_COMPRESSED][S_META] +
		stats.s[S_COMPRESSED][S_DIRTY] +
		reserved +
		(reserved >> 7);
}

static inline u64 bch_fs_sectors_used(struct bch_fs *c)
{
	return min(c->capacity, __bch_fs_sectors_used(c));
}

/* XXX: kill? */
static inline u64 sectors_available(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned i;
	u64 ret = 0;

	rcu_read_lock();
	for_each_member_device_rcu(ca, c, i)
		ret += dev_buckets_available(ca) << ca->bucket_bits;
	rcu_read_unlock();

	return ret;
}

static inline bool is_available_bucket(struct bucket_mark mark)
{
	return (!mark.owned_by_allocator &&
		mark.data_type == BUCKET_DATA &&
		!mark.dirty_sectors &&
		!mark.nouse);
}

static inline bool bucket_needs_journal_commit(struct bucket_mark m,
					       u16 last_seq_ondisk)
{
	return m.journal_seq_valid &&
		((s16) m.journal_seq - (s16) last_seq_ondisk > 0);
}

void bch_bucket_seq_cleanup(struct bch_fs *);

void bch_invalidate_bucket(struct bch_dev *, struct bucket *);
void bch_mark_free_bucket(struct bch_dev *, struct bucket *);
void bch_mark_alloc_bucket(struct bch_dev *, struct bucket *, bool);
void bch_mark_metadata_bucket(struct bch_dev *, struct bucket *,
			      enum bucket_data_type, bool);

void __bch_gc_mark_key(struct bch_fs *, struct bkey_s_c, s64, bool,
		       struct bch_fs_usage *);
void bch_gc_mark_key(struct bch_fs *, struct bkey_s_c, s64, bool);
void bch_mark_key(struct bch_fs *, struct bkey_s_c, s64, bool,
		  struct gc_pos, struct bch_fs_usage *, u64);

void bch_recalc_sectors_available(struct bch_fs *);

void bch_disk_reservation_put(struct bch_fs *,
			      struct disk_reservation *);

#define BCH_DISK_RESERVATION_NOFAIL		(1 << 0)
#define BCH_DISK_RESERVATION_METADATA		(1 << 1)
#define BCH_DISK_RESERVATION_GC_LOCK_HELD	(1 << 2)
#define BCH_DISK_RESERVATION_BTREE_LOCKS_HELD	(1 << 3)

int bch_disk_reservation_add(struct bch_fs *,
			     struct disk_reservation *,
			     unsigned, int);
int bch_disk_reservation_get(struct bch_fs *,
			     struct disk_reservation *,
			     unsigned, int);

#endif /* _BUCKETS_H */
