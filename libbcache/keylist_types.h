#ifndef _BCACHE_KEYLIST_TYPES_H
#define _BCACHE_KEYLIST_TYPES_H

struct keylist {
	union {
		struct bkey_i		*keys;
		u64			*keys_p;
	};
	union {
		struct bkey_i		*top;
		u64			*top_p;
	};
};

#endif /* _BCACHE_KEYLIST_TYPES_H */
