#include </usr/include/dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <attr/xattr.h>

#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/stat.h>

#include <uuid/uuid.h>

#include "cmds.h"
#include "crypto.h"
#include "libbcache.h"
#include "linux/bcache.h"

#include <linux/dcache.h>
#include <linux/generic-radix-tree.h>
#include <linux/xattr.h>
#include "btree_update.h"
#include "buckets.h"
#include "dirent.h"
#include "fs.h"
#include "inode.h"
#include "io.h"
#include "str_hash.h"
#include "super.h"
#include "xattr.h"

static char *dev_t_to_path(dev_t dev)
{
	char link[PATH_MAX], *p;
	int ret;

	char *sysfs_dev = mprintf("/sys/dev/block/%u:%u",
				  major(dev), minor(dev));
	ret = readlink(sysfs_dev, link, sizeof(link));
	free(sysfs_dev);

	if (ret < 0 || ret >= sizeof(link))
		die("readlink error while looking up block device: %s", strerror(errno));

	link[ret] = '\0';

	p = strrchr(link, '/');
	if (!p)
		die("error looking up device name");
	p++;

	return mprintf("/dev/%s", p);
}

static bool path_is_fs_root(char *path)
{
	char *line = NULL, *p, *mount;
	size_t n = 0;
	FILE *f;
	bool ret = true;

	f = fopen("/proc/self/mountinfo", "r");
	if (!f)
		die("Error getting mount information");

	while (getline(&line, &n, f) != -1) {
		p = line;

		strsep(&p, " "); /* mount id */
		strsep(&p, " "); /* parent id */
		strsep(&p, " "); /* dev */
		strsep(&p, " "); /* root */
		mount = strsep(&p, " ");
		strsep(&p, " ");

		if (mount && !strcmp(path, mount))
			goto found;
	}

	ret = false;
found:
	fclose(f);
	free(line);
	return ret;
}

static void mark_unreserved_space(struct cache_set *c, ranges extents)
{
	struct cache *ca = c->cache[0];
	struct hole_iter iter;
	struct range i;

	for_each_hole(iter, extents, bucket_to_sector(ca, ca->mi.nbuckets) << 9, i) {
		struct bucket_mark new;
		u64 b;

		if (i.start == i.end)
			return;

		b = sector_to_bucket(ca, i.start >> 9);
		do {
			bucket_cmpxchg(&ca->buckets[b], new, new.nouse = 1);
			b++;
		} while (bucket_to_sector(ca, b) << 9 < i.end);
	}
}

static void update_inode(struct cache_set *c,
			 struct bch_inode_unpacked *inode)
{
	struct bkey_inode_buf packed;
	int ret;

	bch_inode_pack(&packed, inode);
	ret = bch_btree_update(c, BTREE_ID_INODES, &packed.inode.k_i, NULL);
	if (ret)
		die("error creating file: %s", strerror(-ret));
}

static void create_dirent(struct cache_set *c,
			  struct bch_inode_unpacked *parent,
			  const char *name, u64 inum, mode_t mode)
{
	struct bch_hash_info parent_hash_info = bch_hash_info_init(parent);
	struct qstr qname = { { { .len = strlen(name), } }, .name = name };

	int ret = bch_dirent_create(c, parent->inum, &parent_hash_info,
				    mode_to_type(mode), &qname,
				    inum, NULL, BCH_HASH_SET_MUST_CREATE);
	if (ret)
		die("error creating file: %s", strerror(-ret));

	if (S_ISDIR(mode))
		parent->i_nlink++;
}

static void create_link(struct cache_set *c,
			struct bch_inode_unpacked *parent,
			const char *name, u64 inum, mode_t mode)
{
	struct bch_inode_unpacked inode;
	int ret = bch_inode_find_by_inum(c, inum, &inode);
	if (ret)
		die("error looking up hardlink: %s", strerror(-ret));

	inode.i_nlink++;
	update_inode(c, &inode);

	create_dirent(c, parent, name, inum, mode);
}

static struct bch_inode_unpacked create_file(struct cache_set *c,
					     struct bch_inode_unpacked *parent,
					     const char *name,
					     uid_t uid, gid_t gid,
					     mode_t mode, dev_t rdev)
{
	struct bch_inode_unpacked new_inode;
	struct bkey_inode_buf packed;
	int ret;

	bch_inode_init(c, &new_inode, uid, gid, mode, rdev);
	bch_inode_pack(&packed, &new_inode);

	ret = bch_inode_create(c, &packed.inode.k_i, BLOCKDEV_INODE_MAX, 0,
			       &c->unused_inode_hint);
	if (ret)
		die("error creating file: %s", strerror(-ret));

	new_inode.inum = packed.inode.k.p.inode;
	create_dirent(c, parent, name, new_inode.inum, mode);

	return new_inode;
}

#define for_each_xattr_handler(handlers, handler)		\
	if (handlers)						\
		for ((handler) = *(handlers)++;			\
			(handler) != NULL;			\
			(handler) = *(handlers)++)

static const struct xattr_handler *xattr_resolve_name(const char **name)
{
	const struct xattr_handler **handlers = bch_xattr_handlers;
	const struct xattr_handler *handler;

	for_each_xattr_handler(handlers, handler) {
		const char *n;

		n = strcmp_prefix(*name, xattr_prefix(handler));
		if (n) {
			if (!handler->prefix ^ !*n) {
				if (*n)
					continue;
				return ERR_PTR(-EINVAL);
			}
			*name = n;
			return handler;
		}
	}
	return ERR_PTR(-EOPNOTSUPP);
}

static void copy_times(struct cache_set *c, struct bch_inode_unpacked *dst,
		       struct stat *src)
{
	dst->i_atime = timespec_to_bch_time(c, src->st_atim);
	dst->i_mtime = timespec_to_bch_time(c, src->st_mtim);
	dst->i_ctime = timespec_to_bch_time(c, src->st_ctim);
}

static void copy_xattrs(struct cache_set *c, struct bch_inode_unpacked *dst,
			char *src)
{
	struct bch_hash_info hash_info = bch_hash_info_init(dst);
	ssize_t size = llistxattr(src, NULL, 0);
	if (size < 0)
		die("listxattr error: %s", strerror(errno));

	if (!size)
		return;

	char *buf = malloc(size);
	size = llistxattr(src, buf, size);
	if (size < 0)
		die("listxattr error: %s", strerror(errno));

	for (const char *next, *attr = buf;
	     attr <= buf + size;
	     attr = next) {
		next = attr + strlen(attr) + 1;

		/* max possible xattr val: */
		static char val[64 << 10];
		ssize_t val_size = lgetxattr(src, attr, val, sizeof(val));

		if (val_size < 0)
			die("error getting xattr val: %s", strerror(errno));

		const struct xattr_handler *h = xattr_resolve_name(&attr);

		int ret = __bch_xattr_set(c, dst->inum, &hash_info, attr,
					  val, val_size, 0, h->flags, NULL);
		if (ret < 0)
			die("error creating xattr: %s", strerror(-ret));
	}

	free(buf);
}

static void write_data(struct cache_set *c,
		       struct bch_inode_unpacked *dst_inode,
		       u64 dst_offset, void *buf, size_t len)
{
	struct disk_reservation res;
	struct bch_write_op op;
	struct bch_write_bio bio;
	struct bio_vec bv;
	struct closure cl;

	BUG_ON(dst_offset	& (block_bytes(c) - 1));
	BUG_ON(len		& (block_bytes(c) - 1));

	closure_init_stack(&cl);

	bio_init(&bio.bio);
	bio.bio.bi_max_vecs	= 1;
	bio.bio.bi_io_vec	= &bv;
	bio.bio.bi_iter.bi_size	= len;
	bch_bio_map(&bio.bio, buf);

	int ret = bch_disk_reservation_get(c, &res, len >> 9, 0);
	if (ret)
		die("error reserving space in new filesystem: %s", strerror(-ret));

	bch_write_op_init(&op, c, &bio, res, c->write_points,
			  POS(dst_inode->inum, dst_offset >> 9), NULL, 0);
	closure_call(&op.cl, bch_write, NULL, &cl);
	closure_sync(&cl);

	dst_inode->i_sectors += len >> 9;
}

static char buf[1 << 20] __aligned(PAGE_SIZE);

static void copy_data(struct cache_set *c,
		      struct bch_inode_unpacked *dst_inode,
		      int src_fd, u64 start, u64 end)
{
	while (start < end) {
		unsigned len = min_t(u64, end - start, sizeof(buf));

		xpread(src_fd, buf, len, start);
		write_data(c, dst_inode, start, buf, len);
		start += len;
	}
}

static void link_data(struct cache_set *c, struct bch_inode_unpacked *dst,
		      u64 logical, u64 physical, u64 length)
{
	struct cache *ca = c->cache[0];

	BUG_ON(logical	& (block_bytes(c) - 1));
	BUG_ON(physical & (block_bytes(c) - 1));
	BUG_ON(length	& (block_bytes(c) - 1));

	logical		>>= 9;
	physical	>>= 9;
	length		>>= 9;

	BUG_ON(physical + length > bucket_to_sector(ca, ca->mi.nbuckets));

	while (length) {
		struct bkey_i_extent *e;
		BKEY_PADDED(k) k;
		u64 b = sector_to_bucket(ca, physical >> 9);
		struct disk_reservation res;
		unsigned sectors;
		int ret;

		sectors = min(ca->mi.bucket_size -
			      (physical & (ca->mi.bucket_size - 1)),
			      length);

		e = bkey_extent_init(&k.k);
		e->k.p.inode	= dst->inum;
		e->k.p.offset	= logical + sectors;
		e->k.size	= sectors;
		extent_ptr_append(e, (struct bch_extent_ptr) {
					.offset = physical,
					.dev = 0,
					.gen = ca->buckets[b].mark.gen,
				  });

		ret = bch_disk_reservation_get(c, &res, sectors,
					       BCH_DISK_RESERVATION_NOFAIL);
		if (ret)
			die("error reserving space in new filesystem: %s",
			    strerror(-ret));

		ret = bch_btree_insert(c, BTREE_ID_EXTENTS, &e->k_i,
				       &res, NULL, NULL, 0);
		if (ret)
			die("btree insert error %s", strerror(-ret));

		bch_disk_reservation_put(c, &res);

		dst->i_sectors	+= sectors;
		logical		+= sectors;
		physical	+= sectors;
		length		-= sectors;
	}
}

static void copy_link(struct cache_set *c, struct bch_inode_unpacked *dst,
		      char *src)
{
	ssize_t ret = readlink(src, buf, sizeof(buf));
	if (ret < 0)
		die("readlink error: %s", strerror(errno));

	write_data(c, dst, 0, buf, round_up(ret, block_bytes(c)));
}

static void copy_file(struct cache_set *c, struct bch_inode_unpacked *dst,
		      int src, char *src_path, ranges *extents)
{
	struct fiemap_iter iter;
	struct fiemap_extent e;

	fiemap_for_each(src, iter, e)
		if (e.fe_flags & FIEMAP_EXTENT_UNKNOWN) {
			fsync(src);
			break;
		}

	fiemap_for_each(src, iter, e) {
		if ((e.fe_logical	& (block_bytes(c) - 1)) ||
		    (e.fe_length	& (block_bytes(c) - 1)))
			die("Unaligned extent in %s - can't handle", src_path);

		if (e.fe_flags & (FIEMAP_EXTENT_UNKNOWN|
				  FIEMAP_EXTENT_ENCODED|
				  FIEMAP_EXTENT_NOT_ALIGNED|
				  FIEMAP_EXTENT_DATA_INLINE)) {
			copy_data(c, dst,
				  src,
				  round_down(e.fe_logical, block_bytes(c)),
				  round_up(e.fe_logical + e.fe_length,
					   block_bytes(c)));
			continue;
		}

		if ((e.fe_physical	& (block_bytes(c) - 1)))
			die("Unaligned extent in %s - can't handle", src_path);

		range_add(extents, e.fe_physical, e.fe_length);
		link_data(c, dst, e.fe_logical, e.fe_physical, e.fe_length);
	}
}

struct copy_fs_state {
	u64			bcachefs_inum;
	dev_t			dev;

	GENRADIX(u64)		hardlinks;
	ranges			extents;
};

static void copy_dir(struct copy_fs_state *s,
		     struct cache_set *c,
		     struct bch_inode_unpacked *dst,
		     int src_fd, const char *src_path)
{
	DIR *dir = fdopendir(src_fd);
	struct dirent *d;

	while ((errno = 0), (d = readdir(dir))) {
		struct bch_inode_unpacked inode;
		int fd;

		if (fchdir(src_fd))
			die("chdir error: %s", strerror(errno));

		struct stat stat =
			xfstatat(src_fd, d->d_name, AT_SYMLINK_NOFOLLOW);

		if (!strcmp(d->d_name, ".") ||
		    !strcmp(d->d_name, "..") ||
		    stat.st_ino == s->bcachefs_inum)
			continue;

		char *child_path = mprintf("%s/%s", src_path, d->d_name);

		if (stat.st_dev != s->dev)
			die("%s does not have correct st_dev!", child_path);

		u64 *dst_inum = S_ISREG(stat.st_mode)
			? genradix_ptr_alloc(&s->hardlinks, stat.st_ino, GFP_KERNEL)
			: NULL;

		if (dst_inum && *dst_inum) {
			create_link(c, dst, d->d_name, *dst_inum, S_IFREG);
			goto next;
		}

		inode = create_file(c, dst, d->d_name,
				    stat.st_uid, stat.st_gid,
				    stat.st_mode, stat.st_rdev);

		if (dst_inum)
			*dst_inum = inode.inum;

		copy_times(c, &inode, &stat);
		copy_xattrs(c, &inode, d->d_name);

		/* copy xattrs */

		switch (mode_to_type(stat.st_mode)) {
		case DT_DIR:
			fd = xopen(d->d_name, O_RDONLY|O_NOATIME);
			copy_dir(s, c, &inode, fd, child_path);
			close(fd);
			break;
		case DT_REG:
			inode.i_size = stat.st_size;

			fd = xopen(d->d_name, O_RDONLY|O_NOATIME);
			copy_file(c, &inode, fd, child_path, &s->extents);
			close(fd);
			break;
		case DT_LNK:
			inode.i_size = stat.st_size;

			copy_link(c, &inode, d->d_name);
			break;
		case DT_FIFO:
		case DT_CHR:
		case DT_BLK:
		case DT_SOCK:
		case DT_WHT:
			/* nothing else to copy for these: */
			break;
		default:
			BUG();
		}

		update_inode(c, &inode);
next:
		free(child_path);
	}

	if (errno)
		die("readdir error: %s", strerror(errno));
}

static ranges reserve_new_fs_space(const char *file_path, unsigned block_size,
				   u64 size, u64 *bcachefs_inum, dev_t dev)
{
	int fd = open(file_path, O_RDWR|O_CREAT|O_EXCL, 0600);
	if (fd < 0)
		die("Error creating %s for bcachefs metadata: %s",
		    file_path, strerror(errno));

	struct stat statbuf = xfstat(fd);

	if (statbuf.st_dev != dev)
		die("bcachefs file has incorrect device");

	*bcachefs_inum = statbuf.st_ino;

	if (fallocate(fd, 0, 0, size))
		die("Error reserving space for bcachefs metadata: %s",
		    strerror(errno));

	fsync(fd);

	struct fiemap_iter iter;
	struct fiemap_extent e;
	ranges extents = { NULL };

	fiemap_for_each(fd, iter, e) {
		if (e.fe_flags & (FIEMAP_EXTENT_UNKNOWN|
				  FIEMAP_EXTENT_ENCODED|
				  FIEMAP_EXTENT_NOT_ALIGNED|
				  FIEMAP_EXTENT_DATA_INLINE))
			die("Unable to continue: metadata file not fully mapped");

		if ((e.fe_physical	& (block_size - 1)) ||
		    (e.fe_length	& (block_size - 1)))
			die("Unable to continue: unaligned extents in metadata file");

		range_add(&extents, e.fe_physical, e.fe_length);
	}
	close(fd);

	ranges_sort_merge(&extents);
	return extents;
}

static void reserve_old_fs_space(struct cache_set *c,
				 struct bch_inode_unpacked *root_inode,
				 ranges *extents)
{
	struct cache *ca = c->cache[0];
	struct bch_inode_unpacked dst;
	struct hole_iter iter;
	struct range i;

	dst = create_file(c, root_inode, "old_migrated_filesystem",
			  0, 0, S_IFREG|0400, 0);
	dst.i_size = bucket_to_sector(ca, ca->mi.nbuckets) << 9;

	ranges_sort_merge(extents);

	for_each_hole(iter, *extents, bucket_to_sector(ca, ca->mi.nbuckets) << 9, i)
		link_data(c, &dst, i.start, i.start, i.end - i.start);

	update_inode(c, &dst);
}

static void copy_fs(struct cache_set *c, int src_fd, const char *src_path,
		    u64 bcachefs_inum, ranges *extents)
{
	syncfs(src_fd);

	struct bch_inode_unpacked root_inode;
	int ret = bch_inode_find_by_inum(c, BCACHE_ROOT_INO, &root_inode);
	if (ret)
		die("error looking up root directory: %s", strerror(-ret));

	if (fchdir(src_fd))
		die("chdir error: %s", strerror(errno));

	struct stat stat = xfstat(src_fd);
	copy_times(c, &root_inode, &stat);
	copy_xattrs(c, &root_inode, ".");

	struct copy_fs_state s = {
		.bcachefs_inum	= bcachefs_inum,
		.dev		= stat.st_dev,
		.extents	= *extents,
	};

	/* now, copy: */
	copy_dir(&s, c, &root_inode, src_fd, src_path);

	reserve_old_fs_space(c, &root_inode, &s.extents);

	update_inode(c, &root_inode);

	darray_free(s.extents);
	genradix_free(&s.hardlinks);
}

static void find_superblock_space(ranges extents, struct dev_opts *dev)
{
	struct range *i;
	darray_foreach(i, extents) {
		u64 offset = max(256ULL << 10, i->start);

		if (offset + (128 << 10) <= i->end) {
			dev->sb_offset	= offset >> 9;
			dev->sb_end	= dev->sb_offset + 256;
			return;
		}
	}

	die("Couldn't find a valid location for superblock");
}

static void migrate_usage(void)
{
	puts("bcache migrate - migrate an existing filesystem to bcachefs\n"
	     "Usage: bcache migrate [OPTION]...\n"
	     "\n"
	     "Options:\n"
	     "  -f fs                  Root of filesystem to migrate(s)\n"
	     "      --encrypted        Enable whole filesystem encryption (chacha20/poly1305)\n"
	     "      --no_passphrase    Don't encrypt master encryption key\n"
	     "  -h                     Display this help and exit\n"
	     "Report bugs to <linux-bcache@vger.kernel.org>");
}

static const struct option migrate_opts[] = {
	{ "encrypted",		no_argument, NULL, 'e' },
	{ "no_passphrase",	no_argument, NULL, 'p' },
	{ NULL }
};

int cmd_migrate(int argc, char *argv[])
{
	struct format_opts format_opts = format_opts_default();
	char *fs_path = NULL;
	unsigned block_size;
	bool no_passphrase = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "f:h",
				  migrate_opts, NULL)) != -1)
		switch (opt) {
			case 'f':
				fs_path = optarg;
				break;
			case 'e':
				format_opts.encrypted = true;
				break;
			case 'p':
				no_passphrase = true;
				break;
			case 'h':
				migrate_usage();
				exit(EXIT_SUCCESS);
		}

	if (!fs_path)
		die("Please specify a filesytem to migrate");

	if (!path_is_fs_root(fs_path))
		die("%s is not a filysestem root", fs_path);

	int fs_fd = xopen(fs_path, O_RDONLY|O_NOATIME);
	struct stat stat = xfstat(fs_fd);

	if (!S_ISDIR(stat.st_mode))
		die("%s is not a directory", fs_path);

	struct dev_opts dev = { 0 };

	dev.path = dev_t_to_path(stat.st_dev);
	dev.fd = xopen(dev.path, O_RDWR);

	block_size = min_t(unsigned, stat.st_blksize,
			   get_blocksize(dev.path, dev.fd) << 9);

	BUG_ON(!is_power_of_2(block_size) || block_size < 512);
	format_opts.block_size = block_size >> 9;

	u64 bcachefs_inum;
	char *file_path = mprintf("%s/bcachefs", fs_path);

	ranges extents = reserve_new_fs_space(file_path,
				block_size, get_size(dev.path, dev.fd) / 5,
				&bcachefs_inum, stat.st_dev);

	find_superblock_space(extents, &dev);

	if (format_opts.encrypted && !no_passphrase) {
		format_opts.passphrase = read_passphrase("Enter passphrase: ");

		if (isatty(STDIN_FILENO)) {
			char *pass2 =
				read_passphrase("Enter same passphrase again: ");

			if (strcmp(format_opts.passphrase, pass2)) {
				memzero_explicit(format_opts.passphrase,
						 strlen(format_opts.passphrase));
				memzero_explicit(pass2, strlen(pass2));
				die("Passphrases do not match");
			}

			memzero_explicit(pass2, strlen(pass2));
			free(pass2);
		}
	}

	struct bch_sb *sb = bcache_format(format_opts, &dev, 1);
	u64 sb_offset = le64_to_cpu(sb->layout.sb_offset[0]);

	if (format_opts.passphrase)
		add_bcache_key(sb, format_opts.passphrase);

	free(sb);

	printf("Creating new filesystem on %s in space reserved at %s\n"
	       "To mount, run\n"
	       "  mount -t bcache -o sb=%llu %s dir\n"
	       "\n"
	       "After verifying that the new filesystem is correct, to create a\n"
	       "superblock at the default offset and finish the migration run\n"
	       "  bcache migrate_superblock -d %s -o %llu\n"
	       "\n"
	       "The new filesystem will have a file at /old_migrated_filestem\n"
	       "referencing all disk space that might be used by the existing\n"
	       "filesystem. That file can be deleted once the old filesystem is\n"
	       "no longer needed (and should be deleted prior to running\n"
	       "bcache migrate_superblock)\n",
	       dev.path, file_path, sb_offset, dev.path,
	       dev.path, sb_offset);

	struct bch_opts opts = bch_opts_empty();
	struct cache_set *c = NULL;
	char *path[1] = { dev.path };
	const char *err;

	opts.sb		= sb_offset;
	opts.nostart	= true;
	opts.noexcl	= true;

	err = bch_fs_open(path, 1, opts, &c);
	if (err)
		die("Error opening new filesystem: %s", err);

	mark_unreserved_space(c, extents);

	err = bch_fs_start(c);
	if (err)
		die("Error starting new filesystem: %s", err);

	copy_fs(c, fs_fd, fs_path, bcachefs_inum, &extents);

	bch_fs_stop(c);

	printf("Migrate complete, running fsck:\n");
	opts.nostart	= false;
	opts.nochanges	= true;
	fsck_err_opt	= FSCK_ERR_NO;

	err = bch_fs_open(path, 1, opts, &c);
	if (err)
		die("Error opening new filesystem: %s", err);

	bch_fs_stop(c);
	printf("fsck complete\n");
	return 0;
}

static void migrate_superblock_usage(void)
{
	puts("bcache migrate_superblock - create default superblock after migrating\n"
	     "Usage: bcache migrate_superblock [OPTION]...\n"
	     "\n"
	     "Options:\n"
	     "  -d device     Device to create superblock for\n"
	     "  -o offset     Offset of existing superblock\n"
	     "  -h            Display this help and exit\n"
	     "Report bugs to <linux-bcache@vger.kernel.org>");
}

int cmd_migrate_superblock(int argc, char *argv[])
{
	char *dev = NULL;
	u64 offset = 0;
	int opt, ret;

	while ((opt = getopt(argc, argv, "d:o:h")) != -1)
		switch (opt) {
			case 'd':
				dev = optarg;
				break;
			case 'o':
				ret = kstrtou64(optarg, 10, &offset);
				if (ret)
					die("Invalid offset");
				break;
			case 'h':
				migrate_superblock_usage();
				exit(EXIT_SUCCESS);
		}

	if (!dev)
		die("Please specify a device");

	if (!offset)
		die("Please specify offset of existing superblock");

	int fd = xopen(dev, O_RDWR);
	struct bch_sb *sb = __bcache_super_read(fd, offset);

	if (sb->layout.nr_superblocks >= ARRAY_SIZE(sb->layout.sb_offset))
		die("Can't add superblock: no space left in superblock layout");

	for (unsigned i = 0; i < sb->layout.nr_superblocks; i++)
		if (le64_to_cpu(sb->layout.sb_offset[i]) == BCH_SB_SECTOR)
			die("Superblock layout already has default superblock");

	memmove(&sb->layout.sb_offset[1],
		&sb->layout.sb_offset[0],
		sb->layout.nr_superblocks * sizeof(u64));
	sb->layout.nr_superblocks++;

	sb->layout.sb_offset[0] = cpu_to_le64(BCH_SB_SECTOR);

	bcache_super_write(fd, sb);
	close(fd);

	return 0;
}
