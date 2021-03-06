#ifndef _BCACHE_SUPER_IO_H
#define _BCACHE_SUPER_IO_H

#include "extents.h"
#include "super_types.h"

#include <asm/byteorder.h>

struct bch_sb_field *bch_sb_field_get(struct bch_sb *, enum bch_sb_field_type);
struct bch_sb_field *bch_sb_field_resize(struct bcache_superblock *,
					 enum bch_sb_field_type, unsigned);
struct bch_sb_field *bch_fs_sb_field_resize(struct bch_fs *,
					 enum bch_sb_field_type, unsigned);

#define field_to_type(_f, _name)					\
	container_of_or_null(_f, struct bch_sb_field_##_name, field)

#define BCH_SB_FIELD_TYPE(_name)					\
static inline struct bch_sb_field_##_name *				\
bch_sb_get_##_name(struct bch_sb *sb)					\
{									\
	return field_to_type(bch_sb_field_get(sb,			\
				BCH_SB_FIELD_##_name), _name);		\
}									\
									\
static inline struct bch_sb_field_##_name *				\
bch_sb_resize_##_name(struct bcache_superblock *sb, unsigned u64s)	\
{									\
	return field_to_type(bch_sb_field_resize(sb,			\
				BCH_SB_FIELD_##_name, u64s), _name);	\
}									\
									\
static inline struct bch_sb_field_##_name *				\
bch_fs_sb_resize_##_name(struct bch_fs *c, unsigned u64s)		\
{									\
	return field_to_type(bch_fs_sb_field_resize(c,			\
				BCH_SB_FIELD_##_name, u64s), _name);	\
}

BCH_SB_FIELD_TYPE(journal);
BCH_SB_FIELD_TYPE(members);
BCH_SB_FIELD_TYPE(crypt);

static inline bool bch_sb_test_feature(struct bch_sb *sb,
				       enum bch_sb_features f)
{
	unsigned w = f / 64;
	unsigned b = f % 64;

	return le64_to_cpu(sb->features[w]) & (1ULL << b);
}

static inline void bch_sb_set_feature(struct bch_sb *sb,
				      enum bch_sb_features f)
{
	if (!bch_sb_test_feature(sb, f)) {
		unsigned w = f / 64;
		unsigned b = f % 64;

		le64_add_cpu(&sb->features[w], 1ULL << b);
	}
}

static inline __le64 bch_sb_magic(struct bch_fs *c)
{
	__le64 ret;
	memcpy(&ret, &c->sb.uuid, sizeof(ret));
	return ret;
}

static inline __u64 jset_magic(struct bch_fs *c)
{
	return __le64_to_cpu(bch_sb_magic(c) ^ JSET_MAGIC);
}

static inline __u64 pset_magic(struct bch_fs *c)
{
	return __le64_to_cpu(bch_sb_magic(c) ^ PSET_MAGIC);
}

static inline __u64 bset_magic(struct bch_fs *c)
{
	return __le64_to_cpu(bch_sb_magic(c) ^ BSET_MAGIC);
}

static inline struct bch_member_cpu bch_mi_to_cpu(struct bch_member *mi)
{
	return (struct bch_member_cpu) {
		.nbuckets	= le64_to_cpu(mi->nbuckets),
		.first_bucket	= le16_to_cpu(mi->first_bucket),
		.bucket_size	= le16_to_cpu(mi->bucket_size),
		.state		= BCH_MEMBER_STATE(mi),
		.tier		= BCH_MEMBER_TIER(mi),
		.has_metadata	= BCH_MEMBER_HAS_METADATA(mi),
		.has_data	= BCH_MEMBER_HAS_DATA(mi),
		.replacement	= BCH_MEMBER_REPLACEMENT(mi),
		.discard	= BCH_MEMBER_DISCARD(mi),
		.valid		= !bch_is_zero(mi->uuid.b, sizeof(uuid_le)),
	};
}

int bch_sb_to_fs(struct bch_fs *, struct bch_sb *);
int bch_sb_from_fs(struct bch_fs *, struct bch_dev *);

void bch_free_super(struct bcache_superblock *);
int bch_super_realloc(struct bcache_superblock *, unsigned);

const char *bch_validate_journal_layout(struct bch_sb *,
					struct bch_member_cpu);
const char *bch_validate_cache_super(struct bcache_superblock *);

const char *bch_read_super(struct bcache_superblock *,
			   struct bch_opts, const char *);
void bch_write_super(struct bch_fs *);

void bch_check_mark_super_slowpath(struct bch_fs *,
				   const struct bkey_i *, bool);

static inline bool bch_check_super_marked(struct bch_fs *c,
					  const struct bkey_i *k, bool meta)
{
	struct bkey_s_c_extent e = bkey_i_to_s_c_extent(k);
	const struct bch_extent_ptr *ptr;
	unsigned nr_replicas = 0;
	bool ret = true;

	extent_for_each_ptr(e, ptr) {
		struct bch_dev *ca = c->devs[ptr->dev];

		if (ptr->cached)
			continue;

		if (!(meta
		      ? ca->mi.has_metadata
		      : ca->mi.has_data)) {
			ret = false;
			break;
		}

		nr_replicas++;
	}

	if (nr_replicas <
	    (meta ? c->sb.meta_replicas_have : c->sb.data_replicas_have))
		ret = false;

	return ret;
}

static inline void bch_check_mark_super(struct bch_fs *c,
					const struct bkey_i *k, bool meta)
{
	if (bch_check_super_marked(c, k, meta))
		return;

	bch_check_mark_super_slowpath(c, k, meta);
}

#endif /* _BCACHE_SUPER_IO_H */
