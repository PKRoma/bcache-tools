#ifndef _CRYPTO_H
#define _CRYPTO_H

#include "super-io.h"
#include "tools-util.h"

char *read_passphrase(const char *);
void derive_passphrase(struct bch_sb_field_crypt *,
		       struct bch_key *, const char *);
void add_bcache_key(struct bch_sb *, const char *);
void bch_sb_crypt_init(struct bch_sb *sb, struct bch_sb_field_crypt *,
		       const char *);

#endif /* _CRYPTO_H */
