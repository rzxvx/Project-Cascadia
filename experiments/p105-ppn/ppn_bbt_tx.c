// SPDX-License-Identifier: GPL-2.0-only
#include "ppn_bbt_tx.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif
static u32 ppn_bbt_crc_byte(u32 crc, u8 byte)
{
	u32 i;

	crc ^= byte;
	for (i = 0; i < 8; i++)
		crc = (crc >> 1) ^ (0x82f63b78 & (0u - (crc & 1)));
	return crc;
}

static u32 ppn_bbt_crc_word(u32 crc, u64 value, u32 bytes)
{
	u32 i;

	for (i = 0; i < bytes; i++) {
		crc = ppn_bbt_crc_byte(crc, value & 255);
		value >>= 8;
	}
	return crc;
}

u32 ppn_bbt_snapshot_crc(const struct ppn_bbt_snapshot *s)
{
	u32 crc = 0xffffffff, i;

	/* Domain/version separate the planned Linux record from every Apple record. */
	crc = ppn_bbt_crc_word(crc, 0x314254424e505050ULL, 8);
	crc = ppn_bbt_crc_word(crc, s->media_id, 8);
	crc = ppn_bbt_crc_word(crc, s->owner_id, 8);
	crc = ppn_bbt_crc_word(crc, s->record_epoch, 8);
	crc = ppn_bbt_crc_word(crc, s->generation, 8);
	crc = ppn_bbt_crc_word(crc, s->physical_slot, 8);
	crc = ppn_bbt_crc_word(crc, s->alias_domain, 8);
	crc = ppn_bbt_crc_word(crc, s->blocks, 4);
	crc = ppn_bbt_crc_word(crc, s->operation, 4);
	crc = ppn_bbt_crc_word(crc, s->pending_block, 4);
	for (i = 0; i < PPN_BBT_BITMAP_BYTES; i++)
		crc = ppn_bbt_crc_byte(crc, s->bad[i]);
	return ~crc;
}

u32 ppn_bbt_manifest_crc(const struct ppn_bbt_snapshot *s, u32 body_crc)
{
	u32 crc = 0xffffffff;

	crc = ppn_bbt_crc_word(crc, 0x3154494d4d4f4350ULL, 8);
	crc = ppn_bbt_crc_word(crc, s->media_id, 8);
	crc = ppn_bbt_crc_word(crc, s->owner_id, 8);
	crc = ppn_bbt_crc_word(crc, s->record_epoch, 8);
	crc = ppn_bbt_crc_word(crc, s->generation, 8);
	crc = ppn_bbt_crc_word(crc, s->physical_slot, 8);
	crc = ppn_bbt_crc_word(crc, s->alias_domain, 8);
	crc = ppn_bbt_crc_word(crc, body_crc, 4);
	return ~crc;
}

/* Linux pointer/unsigned-long widths match on the supported host/ARM targets.
 * Ordered subtraction avoids wrap in half-open range overlap checks.
 */
static bool ppn_bbt_overlap(const void *a, unsigned long an,
			    const void *b, unsigned long bn)
{
	unsigned long av = (unsigned long)a, bv = (unsigned long)b;

	return av <= bv ? bv - av < an : av - bv < bn;
}

static int ppn_bbt_fresh(const struct ppn_bbt_tx *t)
{
	const u8 *bytes = (const u8 *)t;
	unsigned long i;

	for (i = 0; i < sizeof(*t); i++)
		if (bytes[i])
			return -EALREADY;
	return 0;
}

static int ppn_bbt_config_valid(const struct ppn_bbt_config *c)
{
	if (!c || !c->media_id || !c->owner_id || !c->lease_epoch ||
	    !c->blocks || c->blocks > PPN_BBT_MAX_BLOCKS)
		return -EINVAL;
	if (!c->ownership_proven)
		return -EACCES;
	if (c->slots[0].physical_id == c->slots[1].physical_id ||
	    c->slots[0].alias_domain == c->slots[1].alias_domain)
		return -EXDEV;
	return 0;
}

static int ppn_bbt_snapshot_valid(const struct ppn_bbt_config *c,
				const struct ppn_bbt_snapshot *s, u32 slot)
{
	u32 i;

	if (s->media_id != c->media_id || s->owner_id != c->owner_id ||
	    s->blocks != c->blocks || !s->record_epoch || !s->generation ||
	    s->physical_slot != c->slots[slot].physical_id ||
	    s->alias_domain != c->slots[slot].alias_domain)
		return -ESTALE;
	if (s->operation > PPN_BBT_ERASE ||
	    (s->operation == PPN_BBT_NONE && s->pending_block != PPN_BBT_NO_BLOCK) ||
	    (s->operation != PPN_BBT_NONE && s->pending_block >= s->blocks))
		return -EINVAL;
	for (i = c->blocks; i < PPN_BBT_MAX_BLOCKS; i++)
		if (s->bad[i >> 3] & (1u << (i & 7)))
			return -EINVAL;
	return 0;
}

static int ppn_bbt_poison(struct ppn_bbt_tx *t, int error)
{
	if (t)
		t->phase = PPN_BBT_READONLY;
	return error;
}

int ppn_bbt_create_empty(struct ppn_bbt_tx *t, const struct ppn_bbt_config *c,
			 const struct ppn_bbt_receipt erased[2])
{
	u32 i;
	int ret;

	if (!t || !c)
		return -EINVAL;
	ret = ppn_bbt_fresh(t);
	if (ret)
		return ret;
	if (ppn_bbt_overlap(t, sizeof(*t), c, sizeof(*c)) ||
	    (erased && (ppn_bbt_overlap(t, sizeof(*t), erased, 2 * sizeof(*erased)) ||
			ppn_bbt_overlap(c, sizeof(*c), erased, 2 * sizeof(*erased)))))
		return -EINVAL;
	ret = ppn_bbt_config_valid(c);
	if (ret)
		return ppn_bbt_poison(t, ret);
	if (!erased)
		return ppn_bbt_poison(t, -EINVAL);
	for (i = 0; i < 2; i++) {
		const struct ppn_bbt_receipt *r = &erased[i];

		if (r->media_id != c->media_id || r->owner_id != c->owner_id ||
		    r->lease_epoch != c->lease_epoch || r->generation ||
		    r->physical_slot != c->slots[i].physical_id ||
		    r->alias_domain != c->slots[i].alias_domain || r->body_crc ||
		    r->manifest_crc || r->stage != PPN_BBT_SLOT_ERASED ||
		    !r->durable || !r->quiesced)
			return ppn_bbt_poison(t, -ESTALE);
	}
	memset(t, 0, sizeof(*t));
	t->config = *c;
	t->committed.media_id = c->media_id;
	t->committed.owner_id = c->owner_id;
	t->committed.record_epoch = c->lease_epoch;
	t->committed.blocks = c->blocks;
	t->committed.pending_block = PPN_BBT_NO_BLOCK;
	t->committed_crc = ppn_bbt_snapshot_crc(&t->committed);
	t->active_slot = 1;
	t->phase = PPN_BBT_GENESIS;
	return 0;
}

int ppn_bbt_recover(struct ppn_bbt_tx *t, const struct ppn_bbt_config *c,
		    const struct ppn_bbt_copy copies[2])
{
	u32 i, chosen = 0, valid = 0;
	int ret;

	if (!t || !c || !copies)
		return -EINVAL;
	ret = ppn_bbt_fresh(t);
	if (ret)
		return ret;
	if (ppn_bbt_overlap(t, sizeof(*t), c, sizeof(*c)) ||
	    ppn_bbt_overlap(t, sizeof(*t), copies, 2 * sizeof(*copies)) ||
	    ppn_bbt_overlap(c, sizeof(*c), copies, 2 * sizeof(*copies)))
		return -EINVAL;
	ret = ppn_bbt_config_valid(c);
	if (ret)
		return ppn_bbt_poison(t, ret);
	memset(t, 0, sizeof(*t));
	t->config = *c;
	t->phase = PPN_BBT_READONLY;
	for (i = 0; i < 2; i++) {
		const struct ppn_bbt_copy *copy = &copies[i];

		if (!copy->complete_read)
			return -EIO;
		if (!copy->body_present && !copy->manifest_present) {
			if (!copy->empty_slot_erased)
				return -EIO;
			continue;
		}
		if (!copy->body_present || copy->empty_slot_erased ||
		    (copy->manifest_present && copy->absent_manifest_erased))
			return -EINVAL;
		if (!copy->manifest_present) {
			if (!copy->absent_manifest_erased)
				return -EIO;
			/* Proven erased marker: this slot never reached its commit.
			 * A torn body here cannot authorize a later operation.
			 */
			if (copy->body_crc == ppn_bbt_snapshot_crc(&copy->snapshot)) {
				ret = ppn_bbt_snapshot_valid(c, &copy->snapshot, i);
				if (ret)
					return ret;
			}
			continue;
		}
		/* A once-committed intent may later suffer corruption. Silently
		 * choosing an older NONE record would forget that operation.
		 */
		if (copy->body_crc != ppn_bbt_snapshot_crc(&copy->snapshot) ||
		    copy->manifest_crc != ppn_bbt_manifest_crc(&copy->snapshot, copy->body_crc))
			return -EIO;
		ret = ppn_bbt_snapshot_valid(c, &copy->snapshot, i);
		if (ret)
			return ret;
		if (valid && copy->snapshot.generation == copies[chosen].snapshot.generation)
			return -EUCLEAN;
		if (!valid || copy->snapshot.generation > copies[chosen].snapshot.generation)
			chosen = i;
		valid++;
	}
	if (!valid)
		return -ENOENT;
	t->committed = copies[chosen].snapshot;
	t->committed_crc = copies[chosen].body_crc;
	t->active_slot = chosen;
	if (t->committed.operation == PPN_BBT_NONE &&
	    t->committed.generation != ~(u64)0)
		t->phase = PPN_BBT_READY;
	return 0;
}

static int ppn_bbt_erased_matches(const struct ppn_bbt_tx *t,
				const struct ppn_bbt_receipt *r)
{
	u32 slot = t->active_slot ^ 1;

	if (!r || r->media_id != t->config.media_id || r->owner_id != t->config.owner_id ||
	    r->lease_epoch != t->config.lease_epoch || r->generation != t->committed.generation + 1 ||
	    r->physical_slot != t->config.slots[slot].physical_id ||
	    r->alias_domain != t->config.slots[slot].alias_domain || r->body_crc ||
	    r->manifest_crc || r->stage != PPN_BBT_SLOT_ERASED || !r->durable || !r->quiesced)
		return -ESTALE;
	return 0;
}

static int ppn_bbt_prepare(struct ppn_bbt_tx *t, const struct ppn_bbt_receipt *r,
			   u32 operation, u32 block, bool retire, u32 after)
{
	int ret;

	if (ppn_bbt_snapshot_crc(&t->committed) != t->committed_crc)
		return ppn_bbt_poison(t, -EPROTO);
	if (t->committed.generation == ~(u64)0)
		return ppn_bbt_poison(t, -EOVERFLOW);
	ret = ppn_bbt_erased_matches(t, r);
	if (ret)
		return ppn_bbt_poison(t, ret);
	t->target_slot = t->active_slot ^ 1;
	t->prepared = t->committed;
	t->prepared.generation++;
	t->prepared.record_epoch = t->config.lease_epoch;
	t->prepared.physical_slot = t->config.slots[t->target_slot].physical_id;
	t->prepared.alias_domain = t->config.slots[t->target_slot].alias_domain;
	t->prepared.operation = operation;
	t->prepared.pending_block = operation == PPN_BBT_NONE ? PPN_BBT_NO_BLOCK : block;
	if (retire)
		t->prepared.bad[block >> 3] |= 1u << (block & 7);
	t->prepared_crc = ppn_bbt_snapshot_crc(&t->prepared);
	t->after_commit = after;
	t->phase = PPN_BBT_WAIT_BODY;
	return 0;
}

int ppn_bbt_checkpoint(struct ppn_bbt_tx *t, const struct ppn_bbt_receipt *r)
{
	if (!t || (t->phase != PPN_BBT_GENESIS && t->phase != PPN_BBT_READY))
		return -EPERM;
	return ppn_bbt_prepare(t, r, PPN_BBT_NONE, PPN_BBT_NO_BLOCK, false, PPN_BBT_READY);
}

int ppn_bbt_begin(struct ppn_bbt_tx *t, enum ppn_bbt_operation operation,
		  u32 block, const struct ppn_bbt_receipt *r)
{
	if (!t || t->phase != PPN_BBT_READY)
		return -EPERM;
	if ((operation != PPN_BBT_PROGRAM && operation != PPN_BBT_ERASE) ||
	    block >= t->config.blocks || (t->committed.bad[block >> 3] & (1u << (block & 7))))
		return -EINVAL;
	return ppn_bbt_prepare(t, r, operation, block, false, PPN_BBT_ACTIVE);
}

int ppn_bbt_finish(struct ppn_bbt_tx *t, bool success, bool durable,
		   bool quiesced, bool retired, const struct ppn_bbt_receipt *r)
{
	if (!t || t->phase != PPN_BBT_ACTIVE)
		return -EPERM;
	if (!quiesced || (success && (!durable || retired)) || (!success && !retired))
		return ppn_bbt_poison(t, -EIO);
	return ppn_bbt_prepare(t, r, PPN_BBT_NONE, t->committed.pending_block,
			       retired, PPN_BBT_READY);
}

int ppn_bbt_get_manifest(struct ppn_bbt_tx *t, u32 *crc)
{
	if (!t || !crc || ppn_bbt_overlap(t, sizeof(*t), crc, sizeof(*crc)))
		return -EINVAL;
	if (t->phase != PPN_BBT_WAIT_MANIFEST)
		return -EPERM;
	if (ppn_bbt_snapshot_crc(&t->prepared) != t->prepared_crc)
		return ppn_bbt_poison(t, -EPROTO);
	*crc = ppn_bbt_manifest_crc(&t->prepared, t->prepared_crc);
	return 0;
}

int ppn_bbt_confirm(struct ppn_bbt_tx *t, const struct ppn_bbt_receipt *r)
{
	u32 expected, manifest;

	if (!t || (t->phase != PPN_BBT_WAIT_BODY && t->phase != PPN_BBT_WAIT_MANIFEST))
		return -EPERM;
	manifest = t->phase == PPN_BBT_WAIT_BODY ? 0 :
		ppn_bbt_manifest_crc(&t->prepared, t->prepared_crc);
	expected = t->phase == PPN_BBT_WAIT_BODY ? PPN_BBT_BODY_DURABLE : PPN_BBT_MANIFEST_DURABLE;
	if (!r || r->media_id != t->config.media_id || r->owner_id != t->config.owner_id ||
	    r->lease_epoch != t->config.lease_epoch || r->generation != t->prepared.generation ||
	    r->physical_slot != t->prepared.physical_slot || r->alias_domain != t->prepared.alias_domain ||
	    r->body_crc != t->prepared_crc || r->manifest_crc != manifest || r->stage != expected || !r->durable || !r->quiesced ||
	    ppn_bbt_snapshot_crc(&t->prepared) != t->prepared_crc)
		return ppn_bbt_poison(t, -ESTALE);
	if (t->phase == PPN_BBT_WAIT_BODY) {
		t->phase = PPN_BBT_WAIT_MANIFEST;
		return 0;
	}
	t->committed = t->prepared;
	t->committed_crc = t->prepared_crc;
	t->active_slot = t->target_slot;
	t->phase = t->after_commit;
	return 0;
}
