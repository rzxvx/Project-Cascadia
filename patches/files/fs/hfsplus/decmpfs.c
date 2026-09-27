// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/hfsplus/decmpfs.c
 *
 * HFS+ transparent compression (decmpfs), read-only.
 *
 * A compressed file has UF_COMPRESSED (0x20) in its BSD flags, an empty data
 * fork and a com.apple.decmpfs attribute: a 16-byte header -- "fpmc", the
 * compression type, the uncompressed size, little-endian -- and for type 3
 * the zlib data right after it.  Type 4 keeps the zlib data in the resource
 * fork instead: a resource-fork header (big-endian, data at its first word),
 * a 4-byte resource length, then a table of 64 KB blocks, little-endian: the
 * count and an (offset, size) pair per block, offsets from the table's start.
 * A block (or a type-3 payload) whose first byte has its low nibble all ones
 * is stored, not compressed, after that byte.
 *
 * Types 3 and 4 are all an iOS 8 System partition uses: 47346 and 5027 of
 * its files on the iPad mini this was written for, 91% of the partition's
 * files.  Without this they read as empty.
 */
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/zlib.h>

#include "hfsplus_fs.h"
#include "xattr.h"

#define DECMPFS_XATTR		"com.apple.decmpfs"
#define DECMPFS_MAGIC		0x636d7066	/* "fpmc" */
#define DECMPFS_ZLIB_XATTR	3
#define DECMPFS_ZLIB_RSRC	4
#define DECMPFS_BLOCK		65536

struct decmpfs_header {
	__le32 magic;
	__le32 type;
	__le64 size;
} __packed;

struct hfsplus_decmpfs {
	u32 type;
	u64 size;			/* uncompressed */
	struct mutex lock;		/* everything below */
	u8 *xattr;			/* type 3: the attribute, header included */
	u32 xattr_len;
	struct inode *rsrc;		/* type 4: the resource fork */
	u64 table_base;
	u32 nblocks;
	__le32 *table;			/* offset, size per block */
	u8 *in;				/* one compressed block */
	u32 in_len;
	u8 *cache;			/* the last block inflated (type 3: the file) */
	s64 cache_idx;
	void *workspace;
};

static int decmpfs_inflate(struct hfsplus_decmpfs *d, const u8 *src, u32 srclen,
			   u8 *dst, u32 dstlen)
{
	struct z_stream_s s = {};
	int ret;

	if (!srclen)
		return -EIO;
	if ((src[0] & 0x0f) == 0x0f) {
		if (srclen - 1 < dstlen)
			return -EIO;
		memcpy(dst, src + 1, dstlen);
		return 0;
	}
	s.workspace = d->workspace;
	if (zlib_inflateInit(&s) != Z_OK)
		return -EIO;
	s.next_in = src;
	s.avail_in = srclen;
	s.next_out = dst;
	s.avail_out = dstlen;
	ret = zlib_inflate(&s, Z_FINISH);
	zlib_inflateEnd(&s);
	if (ret != Z_STREAM_END || s.total_out != dstlen)
		return -EIO;
	return 0;
}

static int rsrc_read(struct inode *rsrc, u64 pos, void *buf, u32 len)
{
	u8 *to = buf;

	if (pos + len > i_size_read(rsrc))
		return -EIO;
	while (len) {
		struct folio *folio = read_mapping_folio(rsrc->i_mapping,
							 pos >> PAGE_SHIFT, NULL);
		size_t off, n;

		if (IS_ERR(folio))
			return PTR_ERR(folio);
		off = offset_in_folio(folio, pos);
		n = min_t(size_t, len, folio_size(folio) - off);
		memcpy_from_folio(to, folio, off, n);
		folio_put(folio);
		to += n;
		pos += n;
		len -= n;
	}
	return 0;
}

/* The resource fork as an inode of its own, private to the decmpfs state:
 * the way the resource fork inodes of old ("file/rsrc") were made. */
static struct inode *decmpfs_rsrc_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct hfsplus_inode_info *hip;
	struct hfs_find_data fd;
	struct inode *rsrc;
	int err;

	rsrc = new_inode(sb);
	if (!rsrc)
		return ERR_PTR(-ENOMEM);
	hip = HFSPLUS_I(rsrc);
	rsrc->i_ino = inode->i_ino;
	INIT_LIST_HEAD(&hip->open_dir_list);
	spin_lock_init(&hip->open_dir_lock);
	mutex_init(&hip->extents_lock);
	hip->extent_state = 0;
	hip->flags = 0;
	hip->rsrc_inode = NULL;
	hip->decmpfs = NULL;
	atomic_set(&hip->opencnt, 0);
	set_bit(HFSPLUS_I_RSRC, &hip->flags);

	err = hfs_find_init(HFSPLUS_SB(sb)->cat_tree, &fd);
	if (!err) {
		err = hfsplus_find_cat(sb, inode->i_ino, &fd);
		if (!err)
			err = hfsplus_cat_read_inode(rsrc, &fd);
		hfs_find_exit(&fd);
	}
	if (err) {
		clear_bit(HFSPLUS_I_RSRC, &hip->flags);
		iput(rsrc);
		return ERR_PTR(err);
	}
	return rsrc;
}

static int decmpfs_load_table(struct hfsplus_decmpfs *d)
{
	__be32 data_off;
	__le32 n;
	u32 i, max = 0;
	int err;

	err = rsrc_read(d->rsrc, 0, &data_off, 4);
	if (err)
		return err;
	/* the resource data starts with its own length, then the table */
	d->table_base = (u64)be32_to_cpu(data_off) + 4;
	err = rsrc_read(d->rsrc, d->table_base, &n, 4);
	if (err)
		return err;
	d->nblocks = le32_to_cpu(n);
	if (d->nblocks != DIV_ROUND_UP(d->size, DECMPFS_BLOCK) || d->nblocks > (1u << 24))
		return -EIO;
	d->table = kvmalloc_array(d->nblocks, 8, GFP_KERNEL);
	if (!d->table)
		return -ENOMEM;
	err = rsrc_read(d->rsrc, d->table_base + 4, d->table, d->nblocks * 8);
	if (err)
		return err;
	for (i = 0; i < d->nblocks; i++)
		max = max(max, le32_to_cpu(d->table[2 * i + 1]));
	if (max > 2 * DECMPFS_BLOCK)
		return -EIO;
	d->in = kvmalloc(max ? max : 1, GFP_KERNEL);
	d->in_len = max;
	d->cache = kvmalloc(DECMPFS_BLOCK, GFP_KERNEL);
	return d->in && d->cache ? 0 : -ENOMEM;
}

/* Inflate block idx (type 3: the whole file) into d->cache. */
static int decmpfs_load(struct hfsplus_decmpfs *d, s64 idx)
{
	int err;

	if (d->cache_idx == idx)
		return 0;
	d->cache_idx = -1;
	if (d->type == DECMPFS_ZLIB_XATTR) {
		if (!d->cache) {
			d->cache = kvmalloc(max_t(u64, d->size, 1), GFP_KERNEL);
			if (!d->cache)
				return -ENOMEM;
		}
		err = decmpfs_inflate(d, d->xattr + sizeof(struct decmpfs_header),
				      d->xattr_len - sizeof(struct decmpfs_header),
				      d->cache, d->size);
	} else {
		u32 off = le32_to_cpu(d->table[2 * idx]);
		u32 len = le32_to_cpu(d->table[2 * idx + 1]);
		u32 out = min_t(u64, DECMPFS_BLOCK, d->size - idx * DECMPFS_BLOCK);

		err = rsrc_read(d->rsrc, d->table_base + off, d->in, len);
		if (!err)
			err = decmpfs_inflate(d, d->in, len, d->cache, out);
	}
	if (!err)
		d->cache_idx = idx;
	return err;
}

static int hfsplus_decmpfs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	struct hfsplus_decmpfs *d = HFSPLUS_I(inode)->decmpfs;
	u64 pos = folio_pos(folio);
	size_t len = folio_size(folio), done = 0;
	int err = 0;

	mutex_lock(&d->lock);
	while (done < len) {
		u64 p = pos + done, start = 0, end = d->size;
		s64 idx = 0;
		size_t n;

		if (p >= d->size) {
			folio_zero_range(folio, done, len - done);
			break;
		}
		if (d->type == DECMPFS_ZLIB_RSRC) {
			idx = div_u64(p, DECMPFS_BLOCK);
			start = idx * DECMPFS_BLOCK;
			end = min_t(u64, start + DECMPFS_BLOCK, d->size);
		}
		err = decmpfs_load(d, idx);
		if (err)
			break;
		n = min_t(u64, len - done, end - p);
		memcpy_to_folio(folio, done, d->cache + (p - start), n);
		done += n;
	}
	mutex_unlock(&d->lock);
	if (err)
		pr_warn_ratelimited("hfsplus: inode %lu: compressed data unreadable (%d)\n",
				    inode->i_ino, err);
	else
		folio_mark_uptodate(folio);
	folio_unlock(folio);
	return err;
}

static const struct address_space_operations hfsplus_decmpfs_aops = {
	.read_folio	= hfsplus_decmpfs_read_folio,
};

static int hfsplus_decmpfs_open(struct inode *inode, struct file *file)
{
	if (file->f_mode & FMODE_WRITE)
		return -EOPNOTSUPP;
	return generic_file_open(inode, file);
}

static const struct file_operations hfsplus_decmpfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.mmap		= generic_file_readonly_mmap,
	.splice_read	= filemap_splice_read,
	.open		= hfsplus_decmpfs_open,
};

static void decmpfs_free(struct hfsplus_decmpfs *d)
{
	if (d->rsrc) {
		/* not a real resource fork inode: no main inode to let go of */
		clear_bit(HFSPLUS_I_RSRC, &HFSPLUS_I(d->rsrc)->flags);
		iput(d->rsrc);
	}
	kfree(d->xattr);
	kvfree(d->table);
	kvfree(d->in);
	kvfree(d->cache);
	kvfree(d->workspace);
	kfree(d);
}

/*
 * Called for every inode hfsplus_iget reads.  A compressed regular file gets
 * its real size and read-only operations that inflate; anything this cannot
 * read (another compression type, a damaged header) is left as it was, with
 * one warning.
 */
void hfsplus_decmpfs_init(struct inode *inode)
{
	struct hfsplus_decmpfs *d;
	struct decmpfs_header h;
	ssize_t len;
	int err = -ENOMEM;

	if (!S_ISREG(inode->i_mode) ||
	    !(HFSPLUS_I(inode)->userflags & HFSPLUS_UF_COMPRESSED))
		return;
	len = __hfsplus_getxattr(inode, DECMPFS_XATTR, NULL, 0);
	if (len < (ssize_t)sizeof(h))
		return;
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		goto fail;
	mutex_init(&d->lock);
	d->cache_idx = -1;
	d->xattr = kmalloc(len, GFP_KERNEL);
	d->workspace = kvmalloc(zlib_inflate_workspacesize(), GFP_KERNEL);
	if (!d->xattr || !d->workspace)
		goto fail;
	err = -EIO;
	if (__hfsplus_getxattr(inode, DECMPFS_XATTR, d->xattr, len) != len)
		goto fail;
	d->xattr_len = len;
	memcpy(&h, d->xattr, sizeof(h));
	d->type = le32_to_cpu(h.type);
	d->size = le64_to_cpu(h.size);
	if (le32_to_cpu(h.magic) != DECMPFS_MAGIC)
		goto fail;
	if (d->type == DECMPFS_ZLIB_RSRC) {
		d->rsrc = decmpfs_rsrc_inode(inode);
		if (IS_ERR(d->rsrc)) {
			err = PTR_ERR(d->rsrc);
			d->rsrc = NULL;
			goto fail;
		}
		err = decmpfs_load_table(d);
		if (err)
			goto fail;
		kfree(d->xattr);
		d->xattr = NULL;
	} else if (d->type != DECMPFS_ZLIB_XATTR) {
		err = -EOPNOTSUPP;
		goto fail;
	}
	HFSPLUS_I(inode)->decmpfs = d;
	i_size_write(inode, d->size);
	inode->i_mapping->a_ops = &hfsplus_decmpfs_aops;
	inode->i_fop = &hfsplus_decmpfs_file_operations;
	return;
fail:
	pr_warn_ratelimited("hfsplus: inode %lu: compressed (type %u), left unreadable (%d)\n",
			    inode->i_ino, d ? d->type : 0, err);
	if (d)
		decmpfs_free(d);
}

void hfsplus_decmpfs_evict(struct inode *inode)
{
	struct hfsplus_decmpfs *d = HFSPLUS_I(inode)->decmpfs;

	if (d) {
		HFSPLUS_I(inode)->decmpfs = NULL;
		decmpfs_free(d);
	}
}
