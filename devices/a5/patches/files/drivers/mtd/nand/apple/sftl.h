/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Swiss FTL for the iPad mini 1 (P105) PPN NAND: interface of the portable
 * core.  The core is plain C: it builds both inside the kernel (apple_ftl) and
 * in the offline test harness (devices/a5/tools/nand-offline).
 *
 * The on-media format and required behaviour are described in
 * devices/a5/docs/research/p105-nand-spec.md.  That the format on this package
 * is the same as on the iPhone 5, and the write path that first proved it
 * writable, are the work of crystal-lized.
 */
#ifndef SFTL_H
#define SFTL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define SFTL_BANKS		4
#define SFTL_BLOCKS		1064
#define SFTL_MLC_PAGES		256
#define SFTL_SLC_PAGES		128
#define SFTL_LBA_SIZE		4096u
#define SFTL_LBAS_PER_PAGE	4u
#define SFTL_PAGE_SIZE		16384u
#define SFTL_META_LBA		16u
#define SFTL_META_PAGE		(SFTL_META_LBA * SFTL_LBAS_PER_PAGE)
#define SFTL_LBAS_PER_SB	4096u
#define SFTL_NUM_SB		1055u
#define SFTL_TOTAL_LBAS		3906266u	/* reported + 16 hidden */
#define SFTL_CTX_LBAS		56u	/* the fixed part: sections 1, 2, 3, 5, 6, 4 */
#define SFTL_CTX_PAGES		14u
#define SFTL_TREE_LBAS		32u	/* tree LBAs inside the fixed part */
#define SFTL_CTX_TREE_VBA	24u	/* VBA of the first tree LBA */
/*
 * The tree may run on past the fixed part, in VBAs that follow it (iOS does
 * this when the map is fragmented); this is the most a Linux context writes.
 */
#define SFTL_TREE_MAX_LBAS	512u
#define SFTL_CTX_MAX_PAGES	((SFTL_CTX_TREE_VBA + SFTL_TREE_MAX_LBAS) / SFTL_LBAS_PER_PAGE)

/* In-memory and on-media "no mapping" VBA. */
#define SFTL_UNMAPPED		0x7fffffu

/* LBA tokens in BTOC entries and metadata. */
#define SFTL_TOKEN_PAD		0xffff0000u
#define SFTL_TOKEN_DELETE	0xffff0001u
#define SFTL_TOKEN_MIN		0xffff0000u

/* Metadata type byte. */
#define SFTL_MT_USER		0x01
#define SFTL_MT_INTERNAL	0x02
#define SFTL_MT_BTOC		0x1c
#define SFTL_MT_CONTEXT		0x1f
#define SFTL_MT_VFL		0x20
#define SFTL_MT_ERASED		0xff

/* Superblock array "type". */
#define SFTL_SB_AWAIT_ERASE	1
#define SFTL_SB_CLOSED		3
#define SFTL_SB_OPEN		4
#define SFTL_SB_OPEN_ALT	5
#define SFTL_SB_CONTEXT		7
#define SFTL_SB_NONE		8
#define SFTL_SB_ERASED		9

/* Package read/program status. */
#define SFTL_ST_OK		0x40
#define SFTL_ST_REFRESH		0x42
#define SFTL_ST_RETIRED		0x45
#define SFTL_ST_ERASED		0x49

#define SFTL_BATCH_PAGES	32u
#define SFTL_SPARE_FLOOR	6u
#define SFTL_SPARE_RESERVE	3u
#define SFTL_MAX_VICTIMS	4u
#define SFTL_VERIFY_DEFAULT	16u
#define SFTL_MAX_CTX		8u
#define SFTL_REPLAY_LOG		64u
#define SFTL_PAD_RUN_MAX	16u

struct sftl_prog {
	u8 bank;
	bool slc;		/* SLC row; the FTL always programs MLC */
	u16 block;
	u16 page;
	const u8 *data;
	const u8 *meta;
};

/*
 * What the core needs from the device below.  read_page() returns a negative
 * errno only when no status could be had at all; status bytes are handed back
 * through *status and judged by the core.  program() is a whole batch; a
 * non-zero return means at least one page of it did not make it.
 */
struct sftl_ops {
	int (*read_page)(void *hw, unsigned int bank, unsigned int block,
			 unsigned int page, bool slc, u8 *data, u8 *meta,
			 u8 *status);
	int (*erase_block)(void *hw, unsigned int bank, unsigned int block);
	int (*program)(void *hw, const struct sftl_prog *pages, unsigned int n);
};

/* One entry of the on-media superblock array. */
struct sftl_sb_rec {
	u8 type;
	u8 btoc_flags;
	u16 valid;
	u16 reserved;
	u16 erases;
};

/* Per-superblock user sequence summary (also on media). */
struct sftl_useq {
	u32 min, max, avg, count;
};

/* The physical shape of a superblock: the banks that carry it, in order. */
struct sftl_sb_shape {
	u8 width;
	u8 bank[SFTL_BANKS];
};

/* First-VBA facts for one superblock, from the mount-time scan. */
struct sftl_sb_probe {
	u8 first_type;
	u8 flags;
	u64 first_weave;
};

struct sftl_replay_item {
	u32 sb;
	bool closed;
	u32 applied;
};

/*
 * The map is a two-level table: a directory of 1024-entry leaves indexed by
 * LBA >> 10.  A NULL leaf means "all unmapped", so memory stays proportional
 * to the space actually in use.  Mapped entries are the VBA.
 */
struct sftl_map {
	u32 **leaf;
	u32 nleaf;
};

struct sftl_btoc_entry {
	u32 weave_offset;
	u32 user;
	u32 lba;
	u32 count;
};

struct sftl_frontier {
	u32 sb;			/* SFTL_UNMAPPED when none open */
	u32 capacity;		/* VBAs in the superblock */
	u32 next;		/* next data VBA offset */
	u64 first_weave;
	u64 last_weave;
	bool first_set;
	u32 nent;
};

struct sftl_writer {
	u8 *pool;			/* SFTL_BATCH_PAGES of data, then meta */
	u8 *meta_pool;
	struct sftl_prog prog[SFTL_BATCH_PAGES];
	u32 *pend_lba;			/* (lba, vba) pairs of the open batch */
	u32 *pend_vba;
	unsigned int pend_n;
	unsigned int npages;

	struct sftl_btoc_entry *entries;
	struct sftl_frontier fr;

	u8 *check_data;
	u8 check_meta[SFTL_META_PAGE];
	u8 *btoc_img;
	u8 *ctx_pages;			/* SFTL_CTX_MAX_PAGES of data, then meta */
	struct sftl_prog *ctx_prog;

	u32 *gc_list;
	u8 *victim_map;
	u32 pending_erase[SFTL_MAX_VICTIMS * 2 + SFTL_MAX_CTX];
	unsigned int n_pending;

	unsigned int verify_every;
	u32 prog_counter;
	bool wedged;
	bool dirty;

	/* Test fence: one superblock only, never a reclaim; commit if allowed. */
	bool fenced;
	bool fence_commit;
	u32 fence_sb;
};

struct sftl {
	const struct sftl_ops *ops;
	void *hw;

	/* VFL */
	u16 *usage[SFTL_BANKS];
	u32 reported_lbas;
	u32 vfl_block;
	u32 vfl_log_pages;
	struct sftl_sb_shape shape[SFTL_NUM_SB];
	u32 sig_bank, sig_block;

	/* Superblock facts */
	struct sftl_sb_probe probe[SFTL_NUM_SB];
	struct sftl_sb_rec *rec;
	struct sftl_useq *useq;
	u32 *reads;
	u8 *stats;

	/* Context */
	u32 ctx_sb;
	u64 ctx_weave;
	u32 ctx_lbas;			/* LBAs of the loaded context, all sections */
	u32 ctx_list[SFTL_MAX_CTX];	/* context superblocks seen on media */
	unsigned int n_ctx;

	/* Map */
	struct sftl_map map;
	u32 total_lbas;			/* mapped space including hidden LBAs */
	u32 tree_runs;
	u32 tree_used;
	u32 mapped_before;

	/* Replay */
	struct sftl_replay_item replay_log[SFTL_REPLAY_LOG];
	unsigned int n_replay;
	u32 replay_applied;
	bool replayed;
	u64 next_weave;

	/* I/O */
	u8 *io_data;
	u8 *io_meta;
	u8 *aux_data;
	u8 aux_meta[SFTL_META_PAGE];
	u8 *cache_data;
	u8 cache_meta[SFTL_META_PAGE];
	bool cache_valid;
	u8 cache_bank;
	u16 cache_block;
	u16 cache_page;

	struct sftl_writer *wr;
};

/* LwVM view. */
#define SFTL_MAX_PARTS 8
#define SFTL_LWVM_CHUNKS 1024

struct sftl_part {
	char name[36];
	u64 start, end, attrs;
	u32 nlbas;
	u16 chunk[SFTL_LWVM_CHUNKS];	/* logical chunk -> physical chunk */
	u32 nchunks;
};

struct sftl_lwvm {
	u32 nparts;
	u64 media_size;
	struct sftl_part part[SFTL_MAX_PARTS];
};

/* Little-endian accessors, used for all media structures. */
static inline u16 sftl_get16(const u8 *p)
{
	return (u16)p[0] | (u16)p[1] << 8;
}

static inline u32 sftl_get32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline u64 sftl_get48(const u8 *p)
{
	return (u64)sftl_get32(p) | (u64)sftl_get16(p + 4) << 32;
}

static inline u64 sftl_get64(const u8 *p)
{
	return (u64)sftl_get32(p) | (u64)sftl_get32(p + 4) << 32;
}

static inline void sftl_put16(u8 *p, u16 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static inline void sftl_put32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static inline void sftl_put48(u8 *p, u64 v)
{
	sftl_put32(p, (u32)v);
	sftl_put16(p + 4, (u16)(v >> 32));
}

static inline u32 sftl_vba(u32 sb, u32 offset)
{
	return sb * SFTL_LBAS_PER_SB + offset;
}

static inline u32 sftl_sb_of(u32 vba)
{
	return vba / SFTL_LBAS_PER_SB;
}

/* The superblock's capacity in VBAs. */
static inline u32 sftl_capacity(const struct sftl *fs, u32 sb)
{
	return (u32)fs->shape[sb].width * (SFTL_LBAS_PER_SB / SFTL_BANKS);
}

/*
 * VBA to physical: VBAs stripe dense over the banks that exist, four LBAs per
 * page.  Returns false for a VBA outside the superblock's capacity.
 */
static inline bool sftl_locate(const struct sftl *fs, u32 vba, u32 *bank,
			       u32 *page, u32 *slot)
{
	u32 sb = sftl_sb_of(vba);
	u32 o = vba % SFTL_LBAS_PER_SB;
	const struct sftl_sb_shape *sh;
	u32 row;

	if (sb >= SFTL_NUM_SB)
		return false;
	sh = &fs->shape[sb];
	if (!sh->width || o >= (u32)sh->width * 1024)
		return false;
	row = o / SFTL_LBAS_PER_PAGE;
	*slot = o % SFTL_LBAS_PER_PAGE;
	*bank = sh->bank[row % sh->width];
	*page = row / sh->width;
	return true;
}

/* sftl_map.c */
int sftl_map_init(struct sftl_map *m, u32 lbas);
void sftl_map_free(struct sftl_map *m);
u32 sftl_map_get(const struct sftl_map *m, u32 lba);
int sftl_map_set(struct sftl_map *m, u32 lba, u32 vba);
u32 sftl_map_count_mapped(const struct sftl_map *m, u32 lbas);

/* sftl_mount.c */
int sftl_init(struct sftl *fs, const struct sftl_ops *ops, void *hw);
void sftl_exit(struct sftl *fs);
int sftl_load_vfl(struct sftl *fs);
int sftl_scan(struct sftl *fs);
int sftl_load_context(struct sftl *fs);
int sftl_replay(struct sftl *fs);
int sftl_mount(struct sftl *fs);
int sftl_read_lba(struct sftl *fs, u32 lba, u8 *out, u8 *meta16);
int sftl_read_vba(struct sftl *fs, u32 vba, u8 *data, u8 *meta16);
int sftl_read_page_cached(struct sftl *fs, u32 bank, u32 block, u32 page,
			  u8 *data, u8 *meta);
int sftl_recount_valid(struct sftl *fs);
bool sftl_sb_closed(struct sftl *fs, u32 sb, u8 *last_meta);
int sftl_read_btoc(struct sftl *fs, u32 sb, u32 *nb, u8 *img);
void sftl_summarize_useq(const u8 *btoc, u32 entries, struct sftl_useq *out);

/* sftl_format.c: producing the on-media images (shared by mount tests and writer). */
u32 sftl_ctx_tree_lbas(u32 used);
int sftl_ctx_build(struct sftl *fs, u32 sb, u64 weave, u8 *pages, u8 *metas,
		   u32 *nlbas_out);
int sftl_tree_serialize(struct sftl *fs, u8 *lbas, u32 max_lbas, u32 *runs_out,
			u32 *used_out);
void sftl_btoc_append(struct sftl_btoc_entry *ent, u32 *n, u64 first_weave,
		      u32 lba, u64 weave, u32 user);
u32 sftl_btoc_build(const struct sftl_btoc_entry *ent, u32 n, u8 *img, u32 *nb_out,
		    u32 force_nb);

/* sftl_write.c; absent from a read-only build */
#ifdef SFTL_NO_WRITE
static inline int sftl_writer_start(struct sftl *fs, unsigned int verify_every)
{
	return -EROFS;
}
static inline void sftl_writer_stop(struct sftl *fs) { }
static inline int sftl_write(struct sftl *fs, u32 lba, u32 count, const u8 *src)
{
	return -EROFS;
}
static inline int sftl_checkpoint(struct sftl *fs)
{
	return 0;
}
#else
int sftl_writer_start(struct sftl *fs, unsigned int verify_every);
void sftl_writer_stop(struct sftl *fs);
int sftl_write(struct sftl *fs, u32 lba, u32 count, const u8 *src);
int sftl_checkpoint(struct sftl *fs);
int sftl_checkpoint_due(struct sftl *fs);
int sftl_writer_fence(struct sftl *fs, u32 sb, bool allow_commit);
int sftl_context_write(struct sftl *fs, u32 sb);
int sftl_gc_plan(struct sftl *fs, u32 *victims, u32 *nv, u32 *lbas, u32 *count);
int sftl_gc(struct sftl *fs);
int sftl_writer_seal(struct sftl *fs);
#endif

/* sftl_lwvm.c */
u32 sftl_crc32(const u8 *p, u32 len);
int sftl_lwvm_parse(const u8 *hdr, const u8 *backup, struct sftl_lwvm *out);
int sftl_lwvm_load(struct sftl *fs, struct sftl_lwvm *out);
int sftl_part_lba(const struct sftl_part *p, u32 rel_lba, u32 *lba);

#endif
