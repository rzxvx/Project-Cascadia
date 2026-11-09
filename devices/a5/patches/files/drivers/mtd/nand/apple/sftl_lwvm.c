// SPDX-License-Identifier: GPL-2.0-only
/* LwVM header parsing and partition address translation. */
#include "sftl.h"
#include "sftl_port.h"

#define LWVM_MAGIC_0	0xcf88906au
#define CHUNK_LBAS	4096u
#define CHUNK_BYTES	(CHUNK_LBAS * 4096ull)
#define ENTRY_OFF	0x200
#define MAP_OFF		0x800
#define MAP_HEADER	0xf000
#define MAP_FREE	0xf111
#define MAP_PAST	0xf3ff

static u32 crc32_update(u32 crc, u8 byte)
{
	unsigned int b;

	crc ^= byte;
	for (b = 0; b < 8; b++)
		crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
	return crc;
}

u32 sftl_crc32(const u8 *p, u32 len)
{
	u32 crc = 0xffffffffu;
	u32 i;

	for (i = 0; i < len; i++)
		crc = crc32_update(crc, p[i]);
	return ~crc;
}

/* The header's CRC covers all 4096 bytes with its own field read as zero. */
static u32 header_crc(const u8 *hdr)
{
	u32 crc = 0xffffffffu;
	u32 i;

	for (i = 0; i < SFTL_LBA_SIZE; i++)
		crc = crc32_update(crc, (i >= 0x2c && i < 0x30) ? 0 : hdr[i]);
	return ~crc;
}

static void utf16_to_ascii(const u8 *in, char *out, unsigned int n)
{
	unsigned int i;

	for (i = 0; i + 1 < n; i++) {
		u16 c = sftl_get16(in + 2 * i);

		if (!c)
			break;
		out[i] = c < 0x80 ? (char)c : '?';
	}
	out[i] = 0;
}

int sftl_lwvm_parse(const u8 *hdr, const u8 *backup, struct sftl_lwvm *out)
{
	u32 i, j;

	if (sftl_get32(hdr) != LWVM_MAGIC_0)
		return -EINVAL;
	if (header_crc(hdr) != sftl_get32(hdr + 0x2c))
		return -EINVAL;
	if (backup && memcmp(hdr, backup, SFTL_LBA_SIZE))
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	out->media_size = sftl_get64(hdr + 0x20);
	out->nparts = sftl_get32(hdr + 0x28);
	if (out->nparts > SFTL_MAX_PARTS)
		return -EINVAL;
	for (i = 0; i < out->nparts; i++) {
		const u8 *e = hdr + ENTRY_OFF + 128 * i;
		struct sftl_part *p = &out->part[i];

		p->start = sftl_get64(e + 32);
		p->end = sftl_get64(e + 40);
		p->attrs = sftl_get64(e + 48);
		utf16_to_ascii(e + 56, p->name, sizeof(p->name));
		if (p->end <= p->start || p->end > out->media_size)
			return -EINVAL;
		p->nlbas = (u32)((p->end - p->start) / SFTL_LBA_SIZE);
		for (j = 0; j < SFTL_LWVM_CHUNKS; j++)
			p->chunk[j] = 0xffff;
	}
	for (j = 0; j < SFTL_LWVM_CHUNKS; j++) {
		u16 v = sftl_get16(hdr + MAP_OFF + 2 * j);
		u32 pn, idx;

		if (v == MAP_HEADER || v == MAP_FREE || v == MAP_PAST)
			continue;
		pn = v >> 12;
		idx = v & 0xfff;
		if (pn >= out->nparts || idx >= SFTL_LWVM_CHUNKS)
			return -EINVAL;
		out->part[pn].chunk[idx] = (u16)j;
		if (idx >= out->part[pn].nchunks)
			out->part[pn].nchunks = idx + 1;
	}
	for (i = 0; i < out->nparts; i++) {
		struct sftl_part *p = &out->part[i];
		u64 chunks = (p->end - p->start + CHUNK_BYTES - 1);

#ifdef __KERNEL__
		do_div(chunks, CHUNK_BYTES);
#else
		chunks /= CHUNK_BYTES;
#endif
		if (chunks != p->nchunks)
			return -EINVAL;
		for (j = 0; j < p->nchunks; j++)
			if (p->chunk[j] == 0xffff)
				return -EINVAL;
	}
	return 0;
}

int sftl_lwvm_load(struct sftl *fs, struct sftl_lwvm *out)
{
	u8 *a, *b;
	int ret;

	a = sftl_alloc(SFTL_LBA_SIZE * 2);
	if (!a)
		return -ENOMEM;
	b = a + SFTL_LBA_SIZE;
	ret = sftl_read_lba(fs, 0, a, NULL);
	if (!ret)
		ret = sftl_read_lba(fs, 4095, b, NULL);
	if (!ret)
		ret = sftl_lwvm_parse(a, b, out);
	sftl_free(a);
	return ret;
}

/* Partition-relative LBA to FTL LBA, through the chunk map. */
int sftl_part_lba(const struct sftl_part *p, u32 rel_lba, u32 *lba)
{
	u32 chunk = rel_lba / CHUNK_LBAS;
	u32 pc;

	if (rel_lba >= p->nlbas || chunk >= p->nchunks)
		return -EINVAL;
	pc = p->chunk[chunk];
	if (pc == 0xffff)
		return -EIO;
	*lba = pc * CHUNK_LBAS + rel_lba % CHUNK_LBAS;
	return 0;
}
