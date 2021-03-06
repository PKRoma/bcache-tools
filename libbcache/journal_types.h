#ifndef _BCACHE_JOURNAL_TYPES_H
#define _BCACHE_JOURNAL_TYPES_H

#include <linux/cache.h>
#include <linux/workqueue.h>

#include "alloc_types.h"
#include "fifo.h"

struct journal_res;

/*
 * We put two of these in struct journal; we used them for writes to the
 * journal that are being staged or in flight.
 */
struct journal_buf {
	struct jset		*data;
	struct closure_waitlist	wait;

	/*
	 * ugh, prio_buckets are stupid - need to convert them to new
	 * transaction machinery when it arrives
	 */
	unsigned		nr_prio_buckets;

	/* bloom filter: */
	unsigned long		has_inode[1024 / sizeof(unsigned long)];
};

/*
 * Something that makes a journal entry dirty - i.e. a btree node that has to be
 * flushed:
 */

struct journal_entry_pin_list {
	struct list_head		list;
	atomic_t			count;
};

struct journal;
struct journal_entry_pin;
typedef void (*journal_pin_flush_fn)(struct journal *j, struct journal_entry_pin *);

struct journal_entry_pin {
	struct list_head		list;
	journal_pin_flush_fn		flush;
	struct journal_entry_pin_list	*pin_list;
};

/* corresponds to a btree node with a blacklisted bset: */
struct blacklisted_node {
	__le64			seq;
	enum btree_id		btree_id;
	struct bpos		pos;
};

struct journal_seq_blacklist {
	struct list_head	list;
	u64			seq;
	bool			written;
	struct journal_entry_pin pin;

	struct blacklisted_node	*entries;
	size_t			nr_entries;
};

struct journal_res {
	bool			ref;
	u8			idx;
	u16			u64s;
	u32			offset;
	u64			seq;
};

union journal_res_state {
	struct {
		atomic64_t	counter;
	};

	struct {
		u64		v;
	};

	struct {
		u64		cur_entry_offset:20,
				idx:1,
				prev_buf_unwritten:1,
				buf0_count:21,
				buf1_count:21;
	};
};

/* 4 mb, in bytes: */
#define JOURNAL_ENTRY_SIZE_MAX		(4U << 20)

/*
 * We stash some journal state as sentinal values in cur_entry_offset:
 */
#define JOURNAL_ENTRY_OFFSET_MAX	((1U << 20) - 1)

#define JOURNAL_ENTRY_CLOSED_VAL	(JOURNAL_ENTRY_OFFSET_MAX - 1)
#define JOURNAL_ENTRY_ERROR_VAL		(JOURNAL_ENTRY_OFFSET_MAX)

/*
 * JOURNAL_NEED_WRITE - current (pending) journal entry should be written ASAP,
 * either because something's waiting on the write to complete or because it's
 * been dirty too long and the timer's expired.
 */

enum {
	JOURNAL_REPLAY_DONE,
	JOURNAL_STARTED,
	JOURNAL_NEED_WRITE,
};

/* Embedded in struct bch_fs */
struct journal {
	/* Fastpath stuff up front: */

	unsigned long		flags;

	union journal_res_state reservations;
	unsigned		cur_entry_u64s;
	unsigned		prev_buf_sectors;
	unsigned		cur_buf_sectors;
	unsigned		entry_size_max; /* bytes */

	/*
	 * Two journal entries -- one is currently open for new entries, the
	 * other is possibly being written out.
	 */
	struct journal_buf	buf[2];

	spinlock_t		lock;

	/* Used when waiting because the journal was full */
	wait_queue_head_t	wait;

	struct closure		io;
	struct delayed_work	write_work;

	/* Sequence number of most recent journal entry (last entry in @pin) */
	atomic64_t		seq;

	/* last_seq from the most recent journal entry written */
	u64			last_seq_ondisk;

	/*
	 * FIFO of journal entries whose btree updates have not yet been
	 * written out.
	 *
	 * Each entry is a reference count. The position in the FIFO is the
	 * entry's sequence number relative to @seq.
	 *
	 * The journal entry itself holds a reference count, put when the
	 * journal entry is written out. Each btree node modified by the journal
	 * entry also holds a reference count, put when the btree node is
	 * written.
	 *
	 * When a reference count reaches zero, the journal entry is no longer
	 * needed. When all journal entries in the oldest journal bucket are no
	 * longer needed, the bucket can be discarded and reused.
	 */
	DECLARE_FIFO(struct journal_entry_pin_list, pin);
	struct journal_entry_pin_list *cur_pin_list;

	/*
	 * Protects the pin lists - the fifo itself is still protected by
	 * j->lock though:
	 */
	spinlock_t		pin_lock;

	struct mutex		blacklist_lock;
	struct list_head	seq_blacklist;

	BKEY_PADDED(key);
	struct dev_group	devs;

	struct delayed_work	reclaim_work;
	unsigned long		last_flushed;

	/* protects advancing ja->last_idx: */
	struct mutex		reclaim_lock;

	/*
	 * ugh: need to get prio_buckets converted over to the eventual new
	 * transaction machinery
	 */
	__le64			prio_buckets[BCH_SB_MEMBERS_MAX];
	unsigned		nr_prio_buckets;

	unsigned		write_delay_ms;
	unsigned		reclaim_delay_ms;

	u64			res_get_blocked_start;
	u64			need_write_time;
	u64			write_start_time;

	struct time_stats	*write_time;
	struct time_stats	*delay_time;
	struct time_stats	*blocked_time;
	struct time_stats	*flush_seq_time;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	res_map;
#endif
};

/*
 * Embedded in struct bch_dev. First three fields refer to the array of journal
 * buckets, in bch_sb.
 */
struct journal_device {
	/*
	 * For each journal bucket, contains the max sequence number of the
	 * journal writes it contains - so we know when a bucket can be reused.
	 */
	u64			*bucket_seq;

	unsigned		sectors_free;

	/* Journal bucket we're currently writing to */
	unsigned		cur_idx;

	/* Last journal bucket that still contains an open journal entry */

	/*
	 * j->lock and j->reclaim_lock must both be held to modify, j->lock
	 * sufficient to read:
	 */
	unsigned		last_idx;
	unsigned		nr;
	u64			*buckets;

	/* Bio for journal reads/writes to this device */
	struct bio		*bio;

	/* for bch_journal_read_device */
	struct closure		read;
};

#endif /* _BCACHE_JOURNAL_TYPES_H */
