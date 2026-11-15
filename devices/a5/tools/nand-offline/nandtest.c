// SPDX-License-Identifier: GPL-2.0-only
/*
 * Offline acceptance for the Swiss FTL core (spec section 8.1), run on a raw
 * dump: bus{N}-cau{C}.b{S}.bin + .st, S in {0, 500, 1000}, one nand page =
 * 16448 bytes as read off the bus, one status byte per page.
 *
 * --write runs the writer on an in-memory overlay of the dump: nothing is ever
 * written to the dump itself.  --rung3, --rung4 and --rung5 run the device-ladder tests
 * on the same overlay, as the kernel test modules run them on the device.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "sftl.h"
#include "sftl_port.h"
#include "rung3.h"
#include "rung4.h"
#include "rung5.h"
#include "rung6.h"
#include "rung7.h"

#define RAW_PAGE 16448
#define OVERLAY_PAGES (1u << 20)

static int checks_failed;
static int checks_passed;

#define CHECK(cond, fmt, ...) do {					\
		if (cond) {						\
			checks_passed++;				\
			printf("  ok   " fmt "\n", ##__VA_ARGS__);	\
		} else {						\
			checks_failed++;				\
			printf("  FAIL " fmt "\n", ##__VA_ARGS__);	\
		}							\
	} while (0)

/* ---- dump + overlay device ------------------------------------------- */

struct block_overlay {
	u8 *pages[SFTL_MLC_PAGES];	/* 16448-byte images, NULL if erased */
	u8 bad_status[SFTL_MLC_PAGES];	/* non-zero: a failed program, read with this status */
	int last;			/* highest programmed page, -1 if none */
	unsigned int programmed;
	unsigned int gaps;
};

struct device {
	int fd[2][2][3];
	int st[2][2][3];
	struct block_overlay *ov[SFTL_BANKS][SFTL_BLOCKS];
	unsigned long reads, progs, erases, pages_gapped;
	bool writes_enabled;
	const u16 *usage[SFTL_BANKS];	/* for the write gate */

	/* Fault injection (--faults): -1 = off. */
	long fail_prog_nth;		/* the Nth page program from now fails */
	int fail_prog_type;		/* or the next page whose slot 0 has this type */
	int fail_mode;			/* 1 nothing written, 2 written, 3 garbage + status 0x51 */
	unsigned int fail_count;	/* how many failures to inject in a row */
	long fail_erase_nth;		/* the Nth block erase from now fails */
};

static int seg_index(unsigned int block)
{
	return block < 500 ? 0 : block < 1000 ? 1 : 2;
}

static unsigned int seg_base(unsigned int block)
{
	return block < 500 ? 0 : block < 1000 ? 500 : 1000;
}

/* SLC page k as it appears in an MLC-row dump. */
static unsigned int slc_to_mlc(unsigned int k)
{
	return k < 4 ? k : (k % 2 == 0 ? 2 * k - 2 : 2 * k - 3);
}

static void raw_to_data_meta(const u8 *raw, u8 *data, u8 *meta)
{
	int u;

	for (u = 0; u < 4; u++) {
		const u8 *r = raw + u * 4112;

		if (data) {
			memcpy(data + u * 4096, r, 1024);
			memcpy(data + u * 4096 + 1024, r + 1040, 3072);
		}
		if (meta)
			memcpy(meta + u * 16, r + 1024, 16);
	}
}

static void data_meta_to_raw(const u8 *data, const u8 *meta, u8 *raw)
{
	int u;

	for (u = 0; u < 4; u++) {
		u8 *r = raw + u * 4112;

		memcpy(r, data + u * 4096, 1024);
		memcpy(r + 1024, meta + u * 16, 16);
		memcpy(r + 1040, data + u * 4096 + 1024, 3072);
	}
}

static int dev_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		    bool slc, u8 *data, u8 *meta, u8 *status)
{
	struct device *d = hw;
	unsigned int bus = bank & 1, cau = bank >> 1;
	struct block_overlay *ov = d->ov[bank][block];
	u8 raw[RAW_PAGE], st;

	d->reads++;
	if (ov && !slc) {
		u8 *p = ov->pages[page];

		if (p) {
			raw_to_data_meta(p, data, meta);
			*status = ov->bad_status[page] ? ov->bad_status[page] : SFTL_ST_OK;
		} else {
			if (data)
				memset(data, 0xff, SFTL_PAGE_SIZE);
			if (meta)
				memset(meta, 0xff, SFTL_META_PAGE);
			*status = SFTL_ST_ERASED;
		}
		return 0;
	}
	{
		unsigned int mp = slc ? slc_to_mlc(page) : page;
		off_t off = (off_t)((block - seg_base(block)) * 256 + mp);
		int s = seg_index(block);

		if (pread(d->fd[bus][cau][s], raw, RAW_PAGE, off * RAW_PAGE) != RAW_PAGE)
			return -EIO;
		if (pread(d->st[bus][cau][s], &st, 1, off) != 1)
			return -EIO;
	}
	raw_to_data_meta(raw, data, meta);
	if (slc && st == 0x61)
		st = SFTL_ST_OK;
	*status = st;
	return 0;
}

static struct block_overlay *overlay_of(struct device *d, unsigned int bank,
					unsigned int block)
{
	if (!d->ov[bank][block]) {
		d->ov[bank][block] = calloc(1, sizeof(struct block_overlay));
		d->ov[bank][block]->last = -1;
	}
	return d->ov[bank][block];
}

static int dev_erase(void *hw, unsigned int bank, unsigned int block)
{
	struct device *d = hw;
	struct block_overlay *ov;
	int i;

	d->erases++;
	if (!d->writes_enabled || !d->usage[bank] || d->usage[bank][block] != 0) {
		printf("  FAIL erase of a non-FTL block: bank %u block %u\n", bank, block);
		abort();
	}
	if (d->fail_erase_nth >= 0 && d->fail_erase_nth-- == 0) {
		printf("  [inject] erase of bank %u block %u fails\n", bank, block);
		return -EIO;
	}
	ov = overlay_of(d, bank, block);
	for (i = 0; i < SFTL_MLC_PAGES; i++) {
		free(ov->pages[i]);
		ov->pages[i] = NULL;
		ov->bad_status[i] = 0;
	}
	ov->last = -1;
	ov->programmed = 0;
	return 0;
}

static int dev_program(void *hw, const struct sftl_prog *p, unsigned int n)
{
	struct device *d = hw;
	unsigned int i;

	for (i = 0; i < n; i++) {
		struct block_overlay *ov;

		d->progs++;
		if (!d->writes_enabled || d->usage[p[i].bank][p[i].block] != 0) {
			printf("  FAIL program of a non-FTL block: bank %u block %u\n",
			       p[i].bank, p[i].block);
			abort();
		}
		ov = d->ov[p[i].bank][p[i].block];
		if (!ov) {
			printf("  FAIL program without erase: bank %u block %u page %u\n",
			       p[i].bank, p[i].block, p[i].page);
			abort();
		}
		if ((int)p[i].page <= ov->last) {
			printf("  FAIL program out of order: bank %u block %u page %u after %d\n",
			       p[i].bank, p[i].block, p[i].page, ov->last);
			abort();
		}
		if ((int)p[i].page != ov->last + 1) {
			ov->gaps += p[i].page - (ov->last + 1);
			d->pages_gapped += p[i].page - (ov->last + 1);
		}
		{
			bool fail = d->fail_count &&
				    ((d->fail_prog_nth >= 0 && d->fail_prog_nth-- == 0) ||
				     (d->fail_prog_type >= 0 && p[i].meta[0] == d->fail_prog_type));

			if (fail) {
				d->fail_count--;
				if (d->fail_count && d->fail_prog_nth < 0 && d->fail_prog_type < 0)
					d->fail_prog_nth = 0;	/* the next one fails too */
				if (!d->fail_count)
					d->fail_prog_type = -1;
				printf("  [inject] program bank %u block %u page %u fails (mode %d)\n",
				       p[i].bank, p[i].block, p[i].page, d->fail_mode);
				if (d->fail_mode >= 2) {
					ov->pages[p[i].page] = malloc(RAW_PAGE);
					data_meta_to_raw(p[i].data, p[i].meta, ov->pages[p[i].page]);
					if (d->fail_mode == 3) {
						memset(ov->pages[p[i].page] + 100, 0x5a, 3000);
						ov->bad_status[p[i].page] = 0x51;
					}
				}
				ov->last = p[i].page;
				return -EIO;
			}
		}
		ov->pages[p[i].page] = malloc(RAW_PAGE);
		data_meta_to_raw(p[i].data, p[i].meta, ov->pages[p[i].page]);
		ov->last = p[i].page;
		ov->programmed++;
	}
	return 0;
}

static const struct sftl_ops dev_ops = {
	.read_page = dev_read,
	.erase_block = dev_erase,
	.program = dev_program,
};

static int dev_open(struct device *d, const char *dir)
{
	static const int bases[3] = { 0, 500, 1000 };
	int b, c, s;
	char path[1024];

	memset(d, 0, sizeof(*d));
	d->fail_prog_nth = -1;
	d->fail_prog_type = -1;
	d->fail_erase_nth = -1;
	for (b = 0; b < 2; b++)
		for (c = 0; c < 2; c++)
			for (s = 0; s < 3; s++) {
				snprintf(path, sizeof(path), "%s/bus%d-cau%d.b%d.bin",
					 dir, b, c, bases[s]);
				d->fd[b][c][s] = open(path, O_RDONLY);
				if (d->fd[b][c][s] < 0) {
					perror(path);
					return -1;
				}
				snprintf(path, sizeof(path), "%s/bus%d-cau%d.b%d.bin.st",
					 dir, b, c, bases[s]);
				d->st[b][c][s] = open(path, O_RDONLY);
				if (d->st[b][c][s] < 0) {
					perror(path);
					return -1;
				}
			}
	return 0;
}

/* ---- section 8.1 checks ---------------------------------------------- */

static u8 *page_of(u8 *pages, u32 i)
{
	return pages + i * SFTL_PAGE_SIZE;
}

/* Expand a tree image into a normalised (vba, span) list. */
static u32 tree_flatten(const u8 *img, u32 *vba, u32 *span)
{
	u32 n = 0, t, r;

	for (t = 0; t < SFTL_TREE_LBAS; t++) {
		const u8 *p = img + t * SFTL_LBA_SIZE;

		if (sftl_get32(p + 4) != 0xfffffff0u)
			break;
		for (r = 0; r < 511; r++) {
			u32 v = sftl_get32(p + 8 + 8 * r), s = sftl_get32(p + 12 + 8 * r);

			if (v == 0xffffffffu)
				break;
			if (n && ((v == SFTL_UNMAPPED && vba[n - 1] == SFTL_UNMAPPED) ||
				  (v != SFTL_UNMAPPED && vba[n - 1] != SFTL_UNMAPPED &&
				   vba[n - 1] + span[n - 1] == v))) {
				span[n - 1] += s;
				continue;
			}
			vba[n] = v;
			span[n] = s;
			n++;
		}
	}
	return n;
}

/* iOS's own tree splits some runs; trees compare after normalising. */
static int compare_trees(const u8 *a, const u8 *b)
{
	static u32 va[20000], sa[20000], vb[20000], sb[20000];
	u32 na = tree_flatten(a, va, sa);
	u32 nb = tree_flatten(b, vb, sb);
	u32 i;

	if (na != nb) {
		printf("  tree: %u normalised runs vs %u\n", na, nb);
		return 1;
	}
	for (i = 0; i < na; i++) {
		if (va[i] != vb[i] || sa[i] != sb[i]) {
			printf("  tree run %u: dump %#x+%u ours %#x+%u\n", i, va[i], sa[i], vb[i], sb[i]);
			return 1;
		}
	}
	printf("  tree normalised: %u runs on both sides\n", na);
	return 0;
}

/* Golden context: rebuild sb519's 14 pages from the loaded tables and compare. */
static void golden_context(struct sftl *fs)
{
	u32 sb = fs->ctx_sb;
	u8 *ours = malloc(SFTL_CTX_MAX_PAGES * (SFTL_PAGE_SIZE + SFTL_META_PAGE));
	u8 *ours_meta = ours + SFTL_CTX_MAX_PAGES * SFTL_PAGE_SIZE;
	u32 nlbas = 0;
	u8 data[SFTL_PAGE_SIZE], meta[SFTL_META_PAGE];
	u32 i, used_runs = 0, used_lbas = 0;
	static u8 dump_tree[8 * SFTL_PAGE_SIZE];
	int bad_meta = 0, bad_base = 0, bad_sbarr = 0, bad_useq = 0, bad_reads = 0, bad_tree = 0;

	assert(!sftl_ctx_build(fs, sb, fs->ctx_weave, ours, ours_meta, &nlbas));
	CHECK(nlbas == SFTL_CTX_LBAS, "context of a 26-LBA tree is %u LBAs (want 56)", nlbas);
	{
		static u8 tree_tmp[SFTL_TREE_MAX_LBAS * SFTL_LBA_SIZE];

		sftl_tree_serialize(fs, tree_tmp, SFTL_TREE_MAX_LBAS, &used_runs, &used_lbas);
	}
	for (i = 0; i < SFTL_CTX_PAGES; i++) {
		u8 st;
		u32 bank = fs->shape[sb].bank[i % 4];

		assert(!fs->ops->read_page(fs->hw, bank, sb, i / 4, false, data, meta, &st));
		if (st != SFTL_ST_OK)
			printf("  page %u status %02x\n", i, st);
		if (memcmp(meta, ours_meta + i * SFTL_META_PAGE, SFTL_META_PAGE)) {
			printf("  metadata differs on context page %u\n", i);
			bad_meta++;
		}
		if (i == 0 && memcmp(data, page_of(ours, i), SFTL_PAGE_SIZE)) {
			u32 w;

			for (w = 0; w < SFTL_PAGE_SIZE / 4; w++)
				if (sftl_get32(data + w * 4) != sftl_get32(page_of(ours, i) + w * 4)) {
					printf("  base differs at word %u: dump %#x ours %#x\n", w,
					       sftl_get32(data + w * 4),
					       sftl_get32(page_of(ours, i) + w * 4));
					bad_base++;
				}
		}
		if (i == 1 && memcmp(data, page_of(ours, i), SFTL_PAGE_SIZE))
			printf("  stats page differs\n");
		if (i == 2 && memcmp(data, page_of(ours, i), 8 * SFTL_NUM_SB))
			bad_sbarr++;
		if ((i == 3 || i == 4) &&
		    memcmp(data, page_of(ours, i), i == 3 ? SFTL_PAGE_SIZE :
			   16 * SFTL_NUM_SB - SFTL_PAGE_SIZE))
			bad_useq++;
		if (i == 5 && memcmp(data, page_of(ours, i), 4 * SFTL_NUM_SB))
			bad_reads++;
		if (i >= 6)
			memcpy(dump_tree + (i - 6) * SFTL_PAGE_SIZE, data, SFTL_PAGE_SIZE);
	}
	bad_tree = compare_trees(dump_tree, ours + 6 * SFTL_PAGE_SIZE);
	CHECK(!bad_meta, "golden context metadata identical (14 pages)");
	CHECK(!bad_base, "golden context base section identical");
	CHECK(!bad_sbarr, "golden SB array identical");
	CHECK(!bad_useq, "golden user sequences identical");
	CHECK(!bad_reads, "golden read counts identical");
	CHECK(!bad_tree, "golden tree identical (%u runs serialized, %u LBAs)", used_runs, used_lbas);
	free(ours);
}

static const u32 expected_tree_runs = 13026;

/* Every closed SB: compare its BTOC with one rebuilt from page metadata. */
static void golden_btoc_and_stats(struct sftl *fs, int *closed_out)
{
	u32 sb;
	int closed = 0, flag8 = 0, flag16 = 0, flag9 = 0, flag_other = 0;
	int btoc_ok = 0, btoc_bad = 0, useq_bad = 0, short_sb = 0, valid_bad = 0;
	int user_diff_sbs = 0, user_diff_ents = 0, nb_choice_diff = 0, multi_pad = 0;
	long deletes = 0, pads = 0;
	static u8 img[2 * SFTL_PAGE_SIZE], rebuilt[2 * SFTL_PAGE_SIZE];
	static struct sftl_btoc_entry ent[2048];
	u8 data[SFTL_PAGE_SIZE], meta[SFTL_META_PAGE];

	for (sb = 0; sb < SFTL_NUM_SB; sb++) {
		u8 last[SFTL_META_LBA];
		u32 nb, cap = sftl_capacity(fs, sb), n = 0, i, o, total = 0, nb2;

		if (!cap || fs->rec[sb].type != SFTL_SB_CLOSED ||
		    !sftl_sb_closed(fs, sb, last))
			continue;
		closed++;
		if (last[1] == 8) flag8++;
		else if (last[1] == 16) flag16++;
		else if (last[1] == 9) flag9++;
		else flag_other++;
		assert(!sftl_read_btoc(fs, sb, &nb, img));

		{
			u64 first = sftl_get48(last + 2);

			for (o = 0; o < cap - nb; o += 4) {
				u32 bank, page, slot, s;
				u8 st;

				sftl_locate(fs, sftl_vba(sb, o), &bank, &page, &slot);
				assert(!fs->ops->read_page(fs->hw, bank, sb, page, false, data, meta, &st));
				if (st != SFTL_ST_OK && st != SFTL_ST_REFRESH)
					break;
				for (s = 0; s < 4; s++) {
					const u8 *m = meta + 16 * s;
					u32 lba = sftl_get32(m + 8);

					sftl_btoc_append(ent, &n, first, lba, sftl_get48(m + 2),
							 sftl_get32(m + 12));
				}
			}
			sftl_btoc_build(ent, n, rebuilt, &nb2, 0);
		}
		if (nb2 != nb) {
			/* iOS picks 8 for some lists that fit in 4 (1014..1022 entries). */
			nb_choice_diff++;
			if (nb2 < nb)
				memset(rebuilt + nb2 * SFTL_LBA_SIZE, 0,
				       (nb - nb2) * SFTL_LBA_SIZE);
		}
		if (!memcmp(img, rebuilt, nb * SFTL_LBA_SIZE)) {
			btoc_ok++;
		} else if (nb2 <= nb) {
			/* Same bytes apart from an entry's user field? */
			u32 k, user_only = 0, other = 0;

			for (k = 0; k < nb * SFTL_LBA_SIZE / 4; k++) {
				if (sftl_get32(img + 4 * k) == sftl_get32(rebuilt + 4 * k))
					continue;
				if (k % 4 == 1)
					user_only++;
				else
					other++;
			}
			if (other) {
				printf("  btoc sb %u: %u non-user words differ, %u rebuilt entries\n", sb, other, n);
				for (k = 0; k < nb * SFTL_LBA_SIZE / 4; k++)
					if (sftl_get32(img + 4 * k) != sftl_get32(rebuilt + 4 * k)) {
						printf("    first diff: entry %u word %u dump %#x ours %#x\n", k / 4, k % 4,
						       sftl_get32(img + 4 * k), sftl_get32(rebuilt + 4 * k));
						printf("    dump entry %u: %#x %u %#x %u\n", k / 4, sftl_get32(img + (k / 4) * 16),
						       sftl_get32(img + (k / 4) * 16 + 4), sftl_get32(img + (k / 4) * 16 + 8),
						       sftl_get32(img + (k / 4) * 16 + 12));
						printf("    ours entry %u: %#x %u %#x %u\n", k / 4, sftl_get32(rebuilt + (k / 4) * 16),
						       sftl_get32(rebuilt + (k / 4) * 16 + 4), sftl_get32(rebuilt + (k / 4) * 16 + 8),
						       sftl_get32(rebuilt + (k / 4) * 16 + 12));
						break;
					}
				btoc_bad++;
			} else {
				if (user_diff_sbs < 16)
					printf("  btoc sb %u: only %u entry user field(s) differ from page metadata\n",
					       sb, user_only);
				user_diff_sbs++;
				user_diff_ents += user_only;
			}
		} else {
			printf("  btoc sb %u: %u entries do not fit iOS's nb %u\n", sb, n, nb);
			btoc_bad++;
		}
		/* Count from the dump's own BTOC. */
		for (i = 0, total = 0; i < nb * SFTL_LBA_SIZE / 16; i++) {
			u32 l = sftl_get32(img + 16 * i + 8), c = sftl_get32(img + 16 * i + 12);

			if (!c)
				break;
			total += c;
			if (l == SFTL_TOKEN_DELETE) deletes++;
			if (l == SFTL_TOKEN_PAD) {
				pads++;
				if (c > 1)
					multi_pad++;
			}
		}
		if (total != cap - nb)
			short_sb++;
		{
			struct sftl_useq u;

			sftl_summarize_useq(img, i, &u);
			if (u.min != fs->useq[sb].min || u.max != fs->useq[sb].max ||
			    u.avg != fs->useq[sb].avg || u.count != fs->useq[sb].count) {
				if (useq_bad < 5)
					printf("  useq sb %u differs: ours %u/%u/%u/%u dump %u/%u/%u/%u\n",
					       sb, u.min, u.max, u.avg, u.count,
					       fs->useq[sb].min, fs->useq[sb].max, fs->useq[sb].avg,
					       fs->useq[sb].count);
				useq_bad++;
			}
		}
		if (fs->rec[sb].btoc_flags != last[1])
			valid_bad++;
	}
	*closed_out = closed;
	CHECK(closed == 1033, "closed SBs = %d (want 1033)", closed);
	CHECK(flag8 == 487 && flag16 == 390 && flag9 == 156 && !flag_other,
	      "BTOC flags 8:%d 16:%d 9:%d other:%d (want 487/390/156/0)", flag8, flag16,
	      flag9, flag_other);
	CHECK(!btoc_bad, "golden BTOC: %d identical, %d differ only in user fields (%d entries), %d differ otherwise",
	      btoc_ok, user_diff_sbs, user_diff_ents, btoc_bad);
	CHECK(!useq_bad, "user sequence summaries match on all closed SBs (%d differ)", useq_bad);
	CHECK(deletes == 339 && pads == 32, "tokens DELETE=%ld PAD=%ld (want 339/32)", deletes, pads);
	CHECK(!valid_bad, "btoc_flags in SB array match BTOCs (%d differ)", valid_bad);
	printf("  note closed SBs with entry total != capacity - nb: %d (iOS always fills)\n", short_sb);
	printf("  note PAD entries with count > 1: %d\n", multi_pad);
	printf("  note SBs where iOS chose nb 8 while the entries fit in 4: %d\n", nb_choice_diff);
}

static void check_valid_counts(struct sftl *fs)
{
	u32 *counts = calloc(SFTL_NUM_SB, sizeof(u32));
	u32 lba, sb, bad = 0;

	for (lba = 0; lba < fs->total_lbas; lba++) {
		u32 v = sftl_map_get(&fs->map, lba);

		if (v != SFTL_UNMAPPED)
			counts[sftl_sb_of(v)]++;
	}
	for (sb = 0; sb < SFTL_NUM_SB; sb++)
		if (counts[sb] != fs->rec[sb].valid && fs->shape[sb].width)
			bad++;
	CHECK(!bad, "valid counts match the tree on all SBs (%u differ)", bad);
	free(counts);
}

static void map_check(struct sftl *fs, unsigned int samples)
{
	unsigned int done = 0, bad = 0, tries = 0;
	u32 lba;
	u8 buf[SFTL_LBA_SIZE], meta[SFTL_META_LBA];
	u32 x = 123456789;

	while (done < samples && tries < samples * 200) {
		tries++;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		lba = x % fs->total_lbas;
		if (sftl_map_get(&fs->map, lba) == SFTL_UNMAPPED)
			continue;
		done++;
		if (sftl_read_lba(fs, lba, buf, meta) ||
		    sftl_get32(meta + 8) != lba)
			bad++;
	}
	CHECK(done == samples && !bad, "%u random mapped LBAs metadata OK (%u mismatches)", done, bad);
}

static void lwvm_check(struct sftl *fs)
{
	struct sftl_lwvm lv;
	int ret = sftl_lwvm_load(fs, &lv);
	u32 i;

	CHECK(!ret, "LwVM header parses, CRC valid, primary = backup");
	if (ret)
		return;
	CHECK(lv.nparts == 2 && lv.part[0].start == 24576 && lv.part[0].end == 2224152576ull,
	      "System %llu..%llu (%u parts)", (unsigned long long)lv.part[0].start,
	      (unsigned long long)lv.part[0].end, lv.nparts);
	CHECK(lv.part[1].end == 15854493696ull, "Data ends %llu",
	      (unsigned long long)lv.part[1].end);
	for (i = 0; i < lv.nparts; i++) {
		u32 lba;
		u8 buf[SFTL_LBA_SIZE];
		u32 off, found = 0;

		printf("  part %u: name %s nlbas %u chunks %u attrs %#llx\n", i, lv.part[i].name,
		       lv.part[i].nlbas, lv.part[i].nchunks, (unsigned long long)lv.part[i].attrs);
		/* An HFS volume header at LBA 0 + 1024: signature 'H+' or 'HX'. */
		assert(!sftl_part_lba(&lv.part[i], 0, &lba));
		assert(!sftl_read_lba(fs, lba, buf, NULL));
		off = 1024;
		found = (buf[off] == 'H' && (buf[off + 1] == '+' || buf[off + 1] == 'X'));
		printf("  part %u rel LBA0 -> FTL %u: HFS sig %c%c %s\n", i, lba, buf[off], buf[off + 1],
		       found ? "(found)" : "");
		if (i == 0)
			CHECK(found, "System HFS signature at relative LBA 0");
	}
}

/* A saved 3-volume header (the I1/L0/L5r sets) must parse. */
static void header_check(const char *prefix)
{
	static u8 a[SFTL_LBA_SIZE], b[SFTL_LBA_SIZE];
	struct sftl_lwvm lv;
	char path[1024];
	int fd, ret;
	u32 i;

	snprintf(path, sizeof(path), "%s.header-0.bin", prefix);
	fd = open(path, O_RDONLY);
	if (fd < 0 || read(fd, a, SFTL_LBA_SIZE) != SFTL_LBA_SIZE)
		return;
	close(fd);
	snprintf(path, sizeof(path), "%s.header-4095.bin", prefix);
	fd = open(path, O_RDONLY);
	if (fd < 0 || read(fd, b, SFTL_LBA_SIZE) != SFTL_LBA_SIZE)
		return;
	close(fd);
	ret = sftl_lwvm_parse(a, b, &lv);
	CHECK(!ret, "saved header %s parses (%d)", prefix, ret);
	if (ret)
		return;
	for (i = 0; i < lv.nparts; i++)
		printf("  part %u: %s %llu..%llu chunks %u\n", i, lv.part[i].name,
		       (unsigned long long)lv.part[i].start, (unsigned long long)lv.part[i].end,
		       lv.part[i].nchunks);
	CHECK(lv.nparts == 3 && !strcmp(lv.part[2].name, "Linux"), "third volume is Linux");
}

/* ---- writer tests (overlay only) ------------------------------------- */

static u32 xr = 0x4a3b2c1d;

static u32 rnd(void)
{
	xr ^= xr << 13;
	xr ^= xr >> 17;
	xr ^= xr << 5;
	return xr;
}

static void pattern(u8 *b, u32 lba, u32 seed)
{
	u32 i;

	for (i = 0; i < SFTL_LBA_SIZE / 4; i++)
		sftl_put32(b + i * 4, lba * 2654435761u + seed + i);
}

/* ---- program and erase failures (spec 4.9) -------------------------- */

struct fault_case {
	const char *name;
	long prog_nth;
	int prog_type;
	int mode;
	unsigned int count;
	long erase_nth;
	bool crash;		/* no commit after the failure: unmount and replay */
};

static struct sftl *fault_mount(struct device *d)
{
	struct sftl *fs = calloc(1, sizeof(*fs));

	if (sftl_init(fs, &dev_ops, d) || sftl_mount(fs)) {
		printf("  mount failed\n");
		return NULL;
	}
	return fs;
}

static void fault_case_run(const char *dir, const struct fault_case *c, unsigned int nr)
{
	struct device *d = malloc(sizeof(*d));
	struct sftl *fs;
	u8 buf[SFTL_LBA_SIZE], want[SFTL_LBA_SIZE], *src;
	u32 base = 300000 + nr * 20000, n = 3000, lba, done = 0, bad_lbas = 0, nbad = 0, sb;
	u8 bad[(SFTL_NUM_SB + 7) / 8];
	unsigned int failures, b;
	int ret = 0, rbad = 0;

	printf("-- %s\n", c->name);
	if (dev_open(d, dir))
		return;
	fs = fault_mount(d);
	if (!fs)
		return;
	for (b = 0; b < SFTL_BANKS; b++)
		d->usage[b] = fs->usage[b];
	d->writes_enabled = true;
	assert(!sftl_writer_start(fs, 1));
	fs->wr->hold_commit = c->crash;
	d->fail_prog_nth = c->prog_nth;
	d->fail_prog_type = c->prog_type;
	d->fail_mode = c->mode;
	d->fail_count = c->count;
	d->fail_erase_nth = c->erase_nth;

	src = malloc(37 * SFTL_LBA_SIZE);
	while (!ret && done < n) {
		u32 k = n - done < 37 ? n - done : 37, j;

		for (j = 0; j < k; j++)
			pattern(src + j * SFTL_LBA_SIZE, base + done + j, nr);
		ret = sftl_write(fs, base + done, k, src);
		done += k;
	}
	free(src);
	CHECK(!ret, "%u LBAs written through the failure (%d)", n, ret);
	if (!c->crash) {
		ret = sftl_checkpoint(fs);
		CHECK(!ret, "checkpoint after it (%d)", ret);
	}
	failures = fs->wr->failures;
	memcpy(bad, fs->wr->bad_map, sizeof(bad));
	for (sb = 0; sb < SFTL_NUM_SB; sb++)
		if ((bad[sb / 8] >> (sb % 8)) & 1)
			nbad++;
	CHECK(failures >= 1 && nbad >= 1 && !fs->wr->wedged,
	      "%u failure(s) handled, %u SB(s) given up, writer running", failures, nbad);
	for (lba = base; lba < base + n; lba++) {
		pattern(want, lba, nr);
		if (sftl_read_lba(fs, lba, buf, NULL) || memcmp(buf, want, sizeof(buf)))
			rbad++;
	}
	CHECK(!rbad, "all LBAs read back in the session (%d bad)", rbad);
	sftl_writer_stop(fs);
	sftl_exit(fs);
	free(fs);

	fs = fault_mount(d);
	if (!fs)
		return;
	rbad = 0;
	for (lba = base; lba < base + n; lba++) {
		pattern(want, lba, nr);
		if (sftl_read_lba(fs, lba, buf, NULL) || memcmp(buf, want, sizeof(buf)))
			rbad++;
	}
	for (lba = 0; lba < fs->total_lbas; lba++) {
		u32 v = sftl_map_get(&fs->map, lba);

		if (v != SFTL_UNMAPPED && ((bad[sftl_sb_of(v) / 8] >> (sftl_sb_of(v) % 8)) & 1))
			bad_lbas++;
	}
	CHECK(!rbad, "remount (context sb %u, %u replayed): all LBAs read back (%d bad)",
	      fs->ctx_sb, fs->replay_applied, rbad);
	if (c->crash) {
		CHECK(fs->replay_applied > 0, "no commit happened: the data came back by replay");
	} else {
		CHECK(!bad_lbas && !fs->replay_applied,
		      "committed: no LBA maps into a given-up SB (%u), nothing to replay", bad_lbas);
		for (sb = 0; sb < SFTL_NUM_SB; sb++)
			if (((bad[sb / 8] >> (sb % 8)) & 1) &&
			    (fs->rec[sb].type != SFTL_SB_AWAIT_ERASE || fs->rec[sb].valid))
				CHECK(0, "given-up sb %u: type %u valid %u in the context (want 1, 0)",
				      sb, fs->rec[sb].type, fs->rec[sb].valid);
	}
	sftl_exit(fs);
	free(fs);
	free(d);
}

static void fault_tests(const char *dir)
{
	static const struct fault_case cases[] = {
		{ "data program fails, nothing written", 40, -1, 1, 1, -1, false },
		{ "data program fails, page written", 40, -1, 2, 1, -1, false },
		{ "data program fails, garbage with status 0x51", 40, -1, 3, 1, -1, false },
		{ "two failures in a row (the second while moving)", 40, -1, 3, 2, -1, false },
		{ "BTOC program fails at close", -1, SFTL_MT_BTOC, 2, 1, -1, false },
		{ "context program fails at commit", -1, SFTL_MT_CONTEXT, 3, 1, -1, false },
		{ "erase of a spare fails", -1, -1, 1, 0, 0, false },
		{ "failure, then a crash before the commit", 40, -1, 3, 1, -1, true },
	};
	unsigned int i;

	printf("== program and erase failures (overlay)\n");
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		fault_case_run(dir, &cases[i], i);
}

static void writer_tests(struct device *d, const char *dir)
{
	struct sftl *fs = calloc(1, sizeof(*fs));
	struct sftl_lwvm lv;
	struct sftl_part *linuxp;
	static u8 buf[SFTL_LBA_SIZE * 256];
	u8 meta[SFTL_META_LBA];
	u32 i, j;
	int ret;
	u32 base_lba = 3500000; /* Far from System/Data use of LBAs */
	u32 seeds[256];

	(void)dir;
	(void)lv;
	(void)linuxp;
	(void)seeds;

	printf("== writer on overlay\n");
	assert(!sftl_init(fs, &dev_ops, d));
	assert(!sftl_mount(fs));
	for (i = 0; i < SFTL_BANKS; i++)
		d->usage[i] = fs->usage[i];
	d->writes_enabled = true;
	ret = sftl_writer_start(fs, 1);
	CHECK(!ret, "writer start (%d)", ret);
	if (ret)
		return;

	u32 mapped_before = sftl_map_count_mapped(&fs->map, fs->total_lbas);
	u32 weave_before = (u32)fs->next_weave;

	/* Aligned and unaligned writes, partial pages, siblings. */
	for (i = 0; i < 200; i++) {
		u32 n = rnd() % 200 + 1;
		u32 start = base_lba + rnd() % 8000;

		for (j = 0; j < n; j++)
			pattern(buf + j * SFTL_LBA_SIZE, start + j, 1);
		ret = sftl_write(fs, start, n, buf);
		if (ret) {
			CHECK(0, "write #%u (%d)", i, ret);
			return;
		}
	}
	/* Second pass overwrites part of it. */
	for (i = 0; i < 300; i++) {
		u32 n = rnd() % 7 + 1;
		u32 start = base_lba + rnd() % 8000;

		for (j = 0; j < n; j++)
			pattern(buf + j * SFTL_LBA_SIZE, start + j, 2);
		ret = sftl_write(fs, start, n, buf);
		if (ret) {
			CHECK(0, "overwrite #%u (%d)", i, ret);
			return;
		}
	}
	/* Read-back of a sample against both possible patterns. */
	{
		int bad = 0;
		u8 *a = malloc(SFTL_LBA_SIZE), *b = malloc(SFTL_LBA_SIZE), *out = malloc(SFTL_LBA_SIZE);

		for (i = 0; i < 1500; i++) {
			u32 lba = base_lba + i * 5;

			if (sftl_map_get(&fs->map, lba) == SFTL_UNMAPPED)
				continue;
			pattern(a, lba, 1);
			pattern(b, lba, 2);
			ret = sftl_read_lba(fs, lba, out, meta);
			if (ret || (memcmp(out, a, SFTL_LBA_SIZE) && memcmp(out, b, SFTL_LBA_SIZE))) {
				bad++;
				continue;
			}
		}
		CHECK(!bad, "read-back after writes: %d bad", bad);
		free(a); free(b); free(out);
	}

	/* Commit, and verify the just-written context by remounting. */
	ret = sftl_checkpoint(fs);
	CHECK(!ret, "checkpoint (%d)", ret);
	u32 ctx_sb = fs->ctx_sb;
	u64 ctx_w = fs->ctx_weave;
	u32 mapped_after = sftl_map_count_mapped(&fs->map, fs->total_lbas);
	printf("  mapped %u -> %u, weave %u -> %llu, ctx sb %u\n", mapped_before, mapped_after,
	       weave_before, (unsigned long long)fs->next_weave, ctx_sb);

	/* Write a few more, then "crash": no checkpoint. */
	for (j = 0; j < 40; j++)
		pattern(buf + j * SFTL_LBA_SIZE, base_lba + 9000 + j, 3);
	CHECK(!sftl_write(fs, base_lba + 9000, 40, buf), "post-commit write");

	/* Keep a digest of the map to compare after remount. */
	u32 *snapshot = malloc(fs->total_lbas * sizeof(u32));

	for (i = 0; i < fs->total_lbas; i++)
		snapshot[i] = sftl_map_get(&fs->map, i);
	u32 total = fs->total_lbas;

	{
		struct sftl *fs2 = calloc(1, sizeof(*fs2));
		unsigned int diffs = 0;

		assert(!sftl_init(fs2, &dev_ops, d));
		ret = sftl_mount(fs2);
		CHECK(!ret, "remount after commit + crash");
		if (!ret) {
			CHECK(fs2->ctx_sb == ctx_sb && fs2->ctx_weave == ctx_w,
			      "newest context is ours: sb %u weave %llu", fs2->ctx_sb,
			      (unsigned long long)fs2->ctx_weave);
			for (i = 0; i < total; i++)
				if (sftl_map_get(&fs2->map, i) != snapshot[i])
					diffs++;
			CHECK(!diffs, "map after replay equals live map (%u differ)", diffs);
			CHECK(fs2->next_weave == fs->next_weave, "next weave resumes: %llu vs %llu",
			      (unsigned long long)fs2->next_weave, (unsigned long long)fs->next_weave);
		}
		sftl_exit(fs2);
		free(fs2);
	}
	free(snapshot);

	/* Reclaim: consume spares until the floor is hit. */
	{
		unsigned int written = 0;
		u32 n, s, lba2 = base_lba;

		/* Enough to rotate several superblocks. */
		for (n = 0; n < 12000 && !ret; n++) {
			for (s = 0; s < 8; s++)
				pattern(buf + s * SFTL_LBA_SIZE, lba2 + s, 4);
			ret = sftl_write(fs, lba2, 8, buf);
			written += 8;
			lba2 += 8;
			if (lba2 > 3700000)
				lba2 = base_lba;
		}
		CHECK(!ret, "sustained writes through reclaim (%u LBAs, %d)", written, ret);
		ret = sftl_checkpoint(fs);
		CHECK(!ret, "checkpoint after reclaim (%d)", ret);
	}

	/* Fragment the map until the tree must compact to commit. */
	{
		unsigned int n;
		u32 lba3 = base_lba + 20000;

		for (n = 0; n < 2600 && !ret; n++) {
			pattern(buf, lba3, 5);
			ret = sftl_write(fs, lba3, 1, buf);
			lba3 += 32;
		}
		CHECK(!ret, "fragmenting writes (%d)", ret);
		if (!ret) {
			static u8 tree_img[SFTL_TREE_MAX_LBAS * SFTL_LBA_SIZE];
			u32 before_runs = 0;

			sftl_tree_serialize(fs, tree_img, SFTL_TREE_MAX_LBAS, &before_runs, NULL);
			printf("  fragmented tree: %u runs before commit\n", before_runs);
			ret = sftl_checkpoint(fs);
			CHECK(!ret, "commit with an over-long tree (%d)", ret);
		}
	}
	/* Hash check: all data still read back correctly. */
	{
		int bad = 0;
		u8 *a = malloc(SFTL_LBA_SIZE), *out = malloc(SFTL_LBA_SIZE);
		u32 l;

		for (l = 0; l < 3700000; l += 1009) {
			if (sftl_map_get(&fs->map, l) == SFTL_UNMAPPED)
				continue;
			ret = sftl_read_lba(fs, l, out, meta);
			if (ret) {
				bad++;
				continue;
			}
			if (l >= base_lba) {
				int ok = 0;

				for (j = 1; j <= 5; j++) {
					pattern(a, l, j);
					ok |= !memcmp(a, out, SFTL_LBA_SIZE);
				}
				if (!ok)
					bad++;
			}
		}
		CHECK(!bad, "all sampled LBAs readable after reclaim: %d bad", bad);
		free(a); free(out);
	}

	/* Remount again and confirm the whole state survives. */
	{
		struct sftl *fs3 = calloc(1, sizeof(*fs3));

		assert(!sftl_init(fs3, &dev_ops, d));
		ret = sftl_mount(fs3);
		CHECK(!ret, "final remount");
		CHECK(fs3->ctx_sb == fs->ctx_sb, "final context sb %u", fs3->ctx_sb);
		if (!ret) {
			unsigned int diffs = 0;

			for (i = 0; i < fs->total_lbas; i++)
				if (sftl_map_get(&fs3->map, i) != sftl_map_get(&fs->map, i))
					diffs++;
			CHECK(!diffs, "final map equals live map (%u differ)", diffs);
		}
		sftl_exit(fs3);
		free(fs3);
	}
	printf("  device: %lu reads, %lu programs, %lu erases, %lu pages skipped\n",
	       d->reads, d->progs, d->erases, d->pages_gapped);
	sftl_writer_stop(fs);
	sftl_exit(fs);
	free(fs);
	d->writes_enabled = false;
}

int main(int argc, char **argv)
{
	struct device *d;
	struct sftl *fs;
	int ret, closed = 0;
	u32 i;
	bool do_write = false;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <dump-dir> [--write | --rung3 | --rung4 | --rung5 | --rung6 | --rung7]\n", argv[0]);
		return 2;
	}
	if (argc > 2 && !strcmp(argv[2], "--write"))
		do_write = true;
	d = malloc(sizeof(*d));
	if (dev_open(d, argv[1]))
		return 1;
	if (argc > 2 && !strcmp(argv[2], "--faults")) {
		fault_tests(argv[1]);
		goto done;
	}
	if (argc > 2 && !strcmp(argv[2], "--rung7")) {
		/* Reclaim + commit on the overlay, then the rung-6 check of the result. */
		struct sftl *vfl = calloc(1, sizeof(*vfl));
		struct rung7_args a7 = { .ops = &dev_ops, .hw = d };
		struct rung6_args a6 = { .ops = &dev_ops, .hw = d, .samples = 20000 };

		assert(!sftl_init(vfl, &dev_ops, d) && !sftl_load_vfl(vfl));
		for (i = 0; i < SFTL_BANKS; i++)
			d->usage[i] = vfl->usage[i];
		d->writes_enabled = true;
		ret = rung7_run(&a7);
		CHECK(!ret, "rung 7 on the overlay (%d)", ret);
		ret = rung6_run(&a6);
		CHECK(!ret, "rung 6 check after the reclaim (%d)", ret);
		printf("  device: %lu reads, %lu programs, %lu erases, %lu pages skipped\n",
		       d->reads, d->progs, d->erases, d->pages_gapped);
		goto done;
	}
	if (argc > 2 && !strcmp(argv[2], "--rung6")) {
		/* Read-only check of a dump, then commit + check on the overlay. */
		struct sftl *vfl = calloc(1, sizeof(*vfl));
		struct rung6_args a6 = { .ops = &dev_ops, .hw = d, .samples = 20000,
					 .watch_sb = { 519, 1017 } };

		ret = rung6_run(&a6);
		CHECK(!ret, "rung 6 check of the dump (%d)", ret);
		assert(!sftl_init(vfl, &dev_ops, d) && !sftl_load_vfl(vfl));
		for (i = 0; i < SFTL_BANKS; i++)
			d->usage[i] = vfl->usage[i];
		d->writes_enabled = true;
		a6.commit = true;
		ret = rung6_run(&a6);
		CHECK(!ret, "rung 6 commit on the overlay (%d)", ret);
		a6.commit = false;
		ret = rung6_run(&a6);
		CHECK(!ret, "rung 6 check of the overlay after the commit (%d)", ret);
		goto done;
	}
	if (argc > 2 && (!strcmp(argv[2], "--rung3") || !strcmp(argv[2], "--rung4") ||
			 !strcmp(argv[2], "--rung5"))) {
		struct sftl *vfl = calloc(1, sizeof(*vfl));
		struct rung3_args a = { .ops = &dev_ops, .hw = d, .nlbas = 2000,
					.search_from = 100000 };
		struct rung4_args a4 = { .ops = &dev_ops, .hw = d };
		struct rung5_args a5 = { .ops = &dev_ops, .hw = d };

		assert(!sftl_init(vfl, &dev_ops, d) && !sftl_load_vfl(vfl));
		for (i = 0; i < SFTL_BANKS; i++)
			d->usage[i] = vfl->usage[i];
		d->writes_enabled = true;
		if (argv[2][6] == '3') {
			ret = rung3_run(&a);
			CHECK(!ret, "rung 3 on the overlay (%d)", ret);
		} else if (argv[2][6] == '4') {
			ret = rung4_run(&a4);
			CHECK(!ret, "rung 4 on the overlay (%d)", ret);
		} else {
			ret = rung5_run(&a5);
			CHECK(!ret, "rung 5 on the overlay (%d)", ret);
		}
		printf("  device: %lu reads, %lu programs, %lu erases, %lu pages skipped\n",
		       d->reads, d->progs, d->erases, d->pages_gapped);
		goto done;
	}
	fs = calloc(1, sizeof(*fs));
	assert(!sftl_init(fs, &dev_ops, d));

	printf("== VFL\n");
	ret = sftl_load_vfl(fs);
	CHECK(!ret, "VFL context loads (%d)", ret);
	CHECK(fs->vfl_block == 1063, "live VFL block %u (want 1063)", fs->vfl_block);
	CHECK(fs->vfl_log_pages == 59, "%u log pages (want 59)", fs->vfl_log_pages);
	CHECK(fs->reported_lbas == 3906250, "reported LBAs %u", fs->reported_lbas);
	{
		static const u32 want[SFTL_BANKS] = { 1053, 1053, 1051, 1054 };
		u32 b, n;

		for (b = 0; b < SFTL_BANKS; b++) {
			u32 c = 0;

			for (n = 0; n < SFTL_BLOCKS; n++)
				if (fs->usage[b][n] == 0)
					c++;
			CHECK(c == want[b], "bank %u usable %u (want %u)", b, c, want[b]);
		}
	}
	CHECK(fs->sig_bank == 2, "signature block on bank %u block %u", fs->sig_bank, fs->sig_block);

	printf("== scan + context\n");
	ret = sftl_scan(fs);
	CHECK(!ret, "scan (%d)", ret);
	ret = sftl_load_context(fs);
	CHECK(!ret, "context loads (%d)", ret);
	if (ret)
		goto done;
	CHECK(fs->ctx_sb == 519 && fs->ctx_weave == 372052037ull, "context sb %u weave %llu",
	      fs->ctx_sb, (unsigned long long)fs->ctx_weave);
	CHECK(fs->tree_runs == expected_tree_runs && fs->total_lbas == 3906266 &&
	      fs->tree_used == 26, "tree: %u runs over %u LBAs, %u used", fs->tree_runs,
	      fs->total_lbas, fs->tree_used);
	CHECK(fs->mapped_before == 747503, "mapped before replay: %u", fs->mapped_before);

	check_valid_counts(fs);

	printf("== golden context\n");
	golden_context(fs);

	if (sftl_map_get(&fs->map, 3906265) != 0x9157b)
		CHECK(0, "hidden LBA at %#x (want 0x9157b)", sftl_map_get(&fs->map, 3906265));
	else
		CHECK(1, "hidden LBA 3906265 at 0x9157b before replay");

	printf("== closed SBs and golden BTOC (reads every page of every closed SB)\n");
	golden_btoc_and_stats(fs, &closed);

	printf("== replay\n");
	ret = sftl_replay(fs);
	CHECK(!ret, "replay (%d)", ret);
	for (i = 0; i < fs->n_replay; i++)
		printf("  replay sb %u closed=%d applied=%u\n", fs->replay_log[i].sb,
		       fs->replay_log[i].closed, fs->replay_log[i].applied);
	{
		u32 m = sftl_map_count_mapped(&fs->map, fs->total_lbas);

		CHECK(m == 743683, "mapped after replay: %u (want 743683)", m);
		CHECK(sftl_map_get(&fs->map, 3906265) == 0xa6513, "hidden LBA at %#x after replay",
		      sftl_map_get(&fs->map, 3906265));
	}

	printf("== whole-map check\n");
	map_check(fs, 120000);

	printf("== LwVM\n");
	lwvm_check(fs);

	if (argc > 3)
		header_check(argv[3]);

	if (do_write)
		writer_tests(d, argv[1]);

done:
	printf("\n%d checks passed, %d failed\n", checks_passed, checks_failed);
	return checks_failed ? 1 : 0;
}
