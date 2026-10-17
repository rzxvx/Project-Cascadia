// SPDX-License-Identifier: GPL-2.0-only
#include "ppn_bbt_codec.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif
#define PPN_BBT_BODY_DOMAIN 0x314254424e505050ULL
#define PPN_BBT_MANIFEST_DOMAIN 0x3154494d4d4f4350ULL
#define PPN_BBT_PAGE_CRC_OFFSET 100u
static const u8 ppn_bbt_body_magic[8] = { 'P', 'P', 'N', 'L', 'B', 'O', 'D', '1' };
static const u8 ppn_bbt_manifest_magic[8] = { 'P', 'P', 'N', 'L', 'C', 'M', 'T', '1' };

static u64 ppn_bbt_get_le(const u8 *bytes, u32 count)
{
	u64 value = 0;
	u32 i;

	for (i = 0; i < count; i++)
		value |= (u64)bytes[i] << (8 * i);
	return value;
}

static void ppn_bbt_put_le(u8 *bytes, u64 value, u32 count)
{
	u32 i;

	for (i = 0; i < count; i++) {
		bytes[i] = value & 255;
		value >>= 8;
	}
}

static u32 ppn_bbt_codec_crc_byte(u32 crc, u8 byte)
{
	u32 i;

	crc ^= byte;
	for (i = 0; i < 8; i++)
		crc = (crc >> 1) ^ (0x82f63b78 & (0u - (crc & 1)));
	return crc;
}

static u32 ppn_bbt_codec_crc_span(u32 crc, const u8 *p, u32 count)
{
	u32 i;

	for (i = 0; i < count; i++)
		crc = ppn_bbt_codec_crc_byte(crc, p[i]);
	return crc;
}

static u32 ppn_bbt_page_crc(const u8 *page)
{
	u32 crc = 0xffffffff, i;

	for (i = 0; i < PPN_BBT_CODEC_PAGE; i++)
		crc = ppn_bbt_codec_crc_byte(crc,
			 i >= PPN_BBT_PAGE_CRC_OFFSET && i < PPN_BBT_PAGE_CRC_OFFSET + 4 ? 0 : page[i]);
	return ~crc;
}

static bool ppn_bbt_codec_overlap(const void *a, unsigned long an,
				  const void *b, unsigned long bn)
{
	unsigned long av = (unsigned long)a, bv = (unsigned long)b;

	return av <= bv ? bv - av < an : av - bv < bn;
}

static int ppn_bbt_codec_config(const struct ppn_bbt_config *c, u32 slot)
{
	if (!c || slot > 1 || !c->media_id || !c->owner_id || !c->lease_epoch ||
	    !c->blocks || c->blocks > PPN_BBT_MAX_BLOCKS)
		return -EINVAL;
	if (c->slots[0].physical_id == c->slots[1].physical_id ||
	    c->slots[0].alias_domain == c->slots[1].alias_domain)
		return -EXDEV;
	return 0;
}

static int ppn_bbt_codec_bitmap(const u8 *bitmap, u32 blocks)
{
	u32 bytes = (blocks + 7) / 8, i;

	if (blocks & 7)
		if (bitmap[bytes - 1] & (0xffu << (blocks & 7)))
			return -EINVAL;
	for (i = bytes; i < PPN_BBT_BITMAP_BYTES; i++)
		if (bitmap[i])
			return -EINVAL;
	return 0;
}

static int ppn_bbt_codec_snapshot(const struct ppn_bbt_config *c, u32 slot,
				   const struct ppn_bbt_snapshot *s)
{
	int ret = ppn_bbt_codec_config(c, slot);

	if (ret)
		return ret;
	if (s->media_id != c->media_id || s->owner_id != c->owner_id ||
	    s->record_epoch != c->lease_epoch || !s->generation ||
	    s->physical_slot != c->slots[slot].physical_id ||
	    s->alias_domain != c->slots[slot].alias_domain || s->blocks != c->blocks)
		return -ESTALE;
	if (s->operation > PPN_BBT_ERASE ||
	    (s->operation == PPN_BBT_NONE && s->pending_block != PPN_BBT_NO_BLOCK) ||
	    (s->operation != PPN_BBT_NONE && s->pending_block >= s->blocks))
		return -EINVAL;
	return ppn_bbt_codec_bitmap(s->bad, s->blocks);
}

static void ppn_bbt_codec_header(u8 *page, const struct ppn_bbt_snapshot *s,
				u32 kind, u32 body_crc, u32 manifest_crc)
{
	memset(page, 0xff, PPN_BBT_CODEC_PAGE);
	memset(page, 0, PPN_BBT_CODEC_HEADER);
	memcpy(page, kind == 1 ? ppn_bbt_body_magic : ppn_bbt_manifest_magic, 8);
	ppn_bbt_put_le(page + 8, 1, 2);
	ppn_bbt_put_le(page + 10, kind, 2);
	ppn_bbt_put_le(page + 12, PPN_BBT_CODEC_HEADER, 4);
	ppn_bbt_put_le(page + 16, PPN_BBT_CODEC_PAGE, 4);
	ppn_bbt_put_le(page + 20, kind == 1 ? PPN_BBT_BITMAP_BYTES : 0, 4);
	ppn_bbt_put_le(page + 24, kind == 1 ? PPN_BBT_CODEC_BODY_USED : PPN_BBT_CODEC_HEADER, 4);
	ppn_bbt_put_le(page + 32, s->media_id, 8);
	ppn_bbt_put_le(page + 40, s->owner_id, 8);
	ppn_bbt_put_le(page + 48, s->record_epoch, 8);
	ppn_bbt_put_le(page + 56, s->generation, 8);
	ppn_bbt_put_le(page + 64, s->physical_slot, 8);
	ppn_bbt_put_le(page + 72, s->alias_domain, 8);
	ppn_bbt_put_le(page + 80, s->blocks, 4);
	ppn_bbt_put_le(page + 84, s->operation, 4);
	ppn_bbt_put_le(page + 88, s->pending_block, 4);
	ppn_bbt_put_le(page + 92, body_crc, 4);
	ppn_bbt_put_le(page + 96, manifest_crc, 4);
	ppn_bbt_put_le(page + 104, kind == 1 ? PPN_BBT_BODY_DOMAIN : PPN_BBT_MANIFEST_DOMAIN, 8);
	ppn_bbt_put_le(page + 112, PPN_BBT_BITMAP_BYTES, 4);
}

int ppn_bbt_codec_encode_body(const struct ppn_bbt_config *c, u32 slot,
			      const struct ppn_bbt_snapshot *s, u8 *page, size_t length)
{
	int ret;

	if (!c || !s || !page || length != PPN_BBT_CODEC_PAGE)
		return -EINVAL;
	if (ppn_bbt_codec_overlap(c, sizeof(*c), s, sizeof(*s)) ||
	    ppn_bbt_codec_overlap(c, sizeof(*c), page, length) ||
	    ppn_bbt_codec_overlap(s, sizeof(*s), page, length))
		return -EINVAL;
	ret = ppn_bbt_codec_snapshot(c, slot, s);
	if (ret)
		return ret;
	ppn_bbt_codec_header(page, s, 1, ppn_bbt_snapshot_crc(s), 0);
	memcpy(page + PPN_BBT_CODEC_HEADER, s->bad, PPN_BBT_BITMAP_BYTES);
	ppn_bbt_put_le(page + PPN_BBT_PAGE_CRC_OFFSET, ppn_bbt_page_crc(page), 4);
	return 0;
}

int ppn_bbt_codec_encode_manifest(const struct ppn_bbt_config *c, u32 slot,
				  const struct ppn_bbt_snapshot *s, u32 body_crc,
				  u32 manifest_crc, u8 *page, size_t length)
{
	int ret;

	if (!c || !s || !page || length != PPN_BBT_CODEC_PAGE)
		return -EINVAL;
	if (ppn_bbt_codec_overlap(c, sizeof(*c), s, sizeof(*s)) ||
	    ppn_bbt_codec_overlap(c, sizeof(*c), page, length) ||
	    ppn_bbt_codec_overlap(s, sizeof(*s), page, length))
		return -EINVAL;
	ret = ppn_bbt_codec_snapshot(c, slot, s);
	if (ret)
		return ret;
	if (body_crc != ppn_bbt_snapshot_crc(s) ||
	    manifest_crc != ppn_bbt_manifest_crc(s, body_crc))
		return -EBADMSG;
	ppn_bbt_codec_header(page, s, 2, body_crc, manifest_crc);
	ppn_bbt_put_le(page + PPN_BBT_PAGE_CRC_OFFSET, ppn_bbt_page_crc(page), 4);
	return 0;
}

static int ppn_bbt_codec_validate(const struct ppn_bbt_config *c, u32 slot,
				 const u8 *page, u32 kind)
{
	u32 used = kind == 1 ? PPN_BBT_CODEC_BODY_USED : PPN_BBT_CODEC_HEADER;
	u32 op, block, crc, i;
	int ret = ppn_bbt_codec_config(c, slot);

	if (ret)
		return ret;
	if (memcmp(page, kind == 1 ? ppn_bbt_body_magic : ppn_bbt_manifest_magic, 8) ||
	    ppn_bbt_get_le(page + 8, 2) != 1 || ppn_bbt_get_le(page + 10, 2) != kind ||
	    ppn_bbt_get_le(page + 12, 4) != PPN_BBT_CODEC_HEADER ||
	    ppn_bbt_get_le(page + 16, 4) != PPN_BBT_CODEC_PAGE ||
	    ppn_bbt_get_le(page + 20, 4) != (kind == 1 ? PPN_BBT_BITMAP_BYTES : 0) ||
	    ppn_bbt_get_le(page + 24, 4) != used || ppn_bbt_get_le(page + 28, 4) ||
	    ppn_bbt_get_le(page + 104, 8) != (kind == 1 ? PPN_BBT_BODY_DOMAIN : PPN_BBT_MANIFEST_DOMAIN) ||
	    ppn_bbt_get_le(page + 112, 4) != PPN_BBT_BITMAP_BYTES)
		return -EINVAL;
	for (i = 116; i < PPN_BBT_CODEC_HEADER; i++)
		if (page[i])
			return -EINVAL;
	for (i = used; i < PPN_BBT_CODEC_PAGE; i++)
		if (page[i] != 0xff)
			return -EINVAL;
	if (ppn_bbt_get_le(page + 32, 8) != c->media_id ||
	    ppn_bbt_get_le(page + 40, 8) != c->owner_id || !ppn_bbt_get_le(page + 48, 8) ||
	    !ppn_bbt_get_le(page + 56, 8) ||
	    ppn_bbt_get_le(page + 64, 8) != c->slots[slot].physical_id ||
	    ppn_bbt_get_le(page + 72, 8) != c->slots[slot].alias_domain ||
	    ppn_bbt_get_le(page + 80, 4) != c->blocks)
		return -ESTALE;
	op = ppn_bbt_get_le(page + 84, 4);
	block = ppn_bbt_get_le(page + 88, 4);
	if (op > PPN_BBT_ERASE || (op == PPN_BBT_NONE && block != PPN_BBT_NO_BLOCK) ||
	    (op != PPN_BBT_NONE && block >= c->blocks))
		return -EINVAL;
	if (kind == 1) {
		if (ppn_bbt_get_le(page + 96, 4))
			return -EINVAL;
		ret = ppn_bbt_codec_bitmap(page + PPN_BBT_CODEC_HEADER, c->blocks);
		if (ret)
			return ret;
		crc = ppn_bbt_codec_crc_span(0xffffffff, page + 104, 8);
		crc = ppn_bbt_codec_crc_span(crc, page + 32, 60);
		crc = ~ppn_bbt_codec_crc_span(crc, page + PPN_BBT_CODEC_HEADER, PPN_BBT_BITMAP_BYTES);
		if (crc != ppn_bbt_get_le(page + 92, 4))
			return -EBADMSG;
	} else {
		crc = ppn_bbt_codec_crc_span(0xffffffff, page + 104, 8);
		crc = ppn_bbt_codec_crc_span(crc, page + 32, 48);
		crc = ~ppn_bbt_codec_crc_span(crc, page + 92, 4);
		if (crc != ppn_bbt_get_le(page + 96, 4))
			return -EBADMSG;
	}
	if (ppn_bbt_page_crc(page) != ppn_bbt_get_le(page + PPN_BBT_PAGE_CRC_OFFSET, 4))
		return -EBADMSG;
	return 0;
}

static void ppn_bbt_codec_publish(const u8 *page, struct ppn_bbt_snapshot *s)
{
	memset(s, 0, sizeof(*s));
	s->media_id = ppn_bbt_get_le(page + 32, 8);
	s->owner_id = ppn_bbt_get_le(page + 40, 8);
	s->record_epoch = ppn_bbt_get_le(page + 48, 8);
	s->generation = ppn_bbt_get_le(page + 56, 8);
	s->physical_slot = ppn_bbt_get_le(page + 64, 8);
	s->alias_domain = ppn_bbt_get_le(page + 72, 8);
	s->blocks = ppn_bbt_get_le(page + 80, 4);
	s->operation = ppn_bbt_get_le(page + 84, 4);
	s->pending_block = ppn_bbt_get_le(page + 88, 4);
	memcpy(s->bad, page + PPN_BBT_CODEC_HEADER, PPN_BBT_BITMAP_BYTES);
}

int ppn_bbt_codec_decode_body(const struct ppn_bbt_config *c, u32 slot,
			      const u8 *page, size_t length, struct ppn_bbt_body_view *out)
{
	int ret;

	if (!c || !page || !out || length != PPN_BBT_CODEC_PAGE)
		return -EINVAL;
	if (ppn_bbt_codec_overlap(c, sizeof(*c), page, length) ||
	    ppn_bbt_codec_overlap(c, sizeof(*c), out, sizeof(*out)) ||
	    ppn_bbt_codec_overlap(page, length, out, sizeof(*out)))
		return -EINVAL;
	ret = ppn_bbt_codec_validate(c, slot, page, 1);
	if (ret)
		return ret;
	memset(out, 0, sizeof(*out));
	ppn_bbt_codec_publish(page, &out->snapshot);
	out->body_crc = ppn_bbt_get_le(page + 92, 4);
	return 0;
}

int ppn_bbt_codec_decode_pair(const struct ppn_bbt_config *c, u32 slot,
			      const u8 *body, size_t blen, const u8 *manifest, size_t mlen,
			      struct ppn_bbt_decoded *out)
{
	int ret;

	if (!c || !body || !manifest || !out ||
	    blen != PPN_BBT_CODEC_PAGE || mlen != PPN_BBT_CODEC_PAGE)
		return -EINVAL;
	if (ppn_bbt_codec_overlap(c, sizeof(*c), body, blen) ||
	    ppn_bbt_codec_overlap(c, sizeof(*c), manifest, mlen) ||
	    ppn_bbt_codec_overlap(c, sizeof(*c), out, sizeof(*out)) ||
	    ppn_bbt_codec_overlap(body, blen, manifest, mlen) ||
	    ppn_bbt_codec_overlap(body, blen, out, sizeof(*out)) ||
	    ppn_bbt_codec_overlap(manifest, mlen, out, sizeof(*out)))
		return -EINVAL;
	ret = ppn_bbt_codec_validate(c, slot, body, 1);
	if (ret)
		return ret;
	ret = ppn_bbt_codec_validate(c, slot, manifest, 2);
	if (ret)
		return ret;
	if (memcmp(body + 32, manifest + 32, 64))
		return -ESTALE;
	memset(out, 0, sizeof(*out));
	ppn_bbt_codec_publish(body, &out->snapshot);
	out->body_crc = ppn_bbt_get_le(body + 92, 4);
	out->manifest_crc = ppn_bbt_get_le(manifest + 96, 4);
	return 0;
}
