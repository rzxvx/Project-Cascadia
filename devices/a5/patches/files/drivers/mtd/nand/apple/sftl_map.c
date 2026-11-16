// SPDX-License-Identifier: GPL-2.0-only
/* LBA map: a directory of lazily allocated leaves. */
#include "sftl.h"
#include "sftl_port.h"

#define LEAF_SHIFT	10
#define LEAF_ENTRIES	(1u << LEAF_SHIFT)
#define LEAF_MASK	(LEAF_ENTRIES - 1)

int sftl_map_init(struct sftl_map *m, u32 lbas)
{
	m->nleaf = (lbas + LEAF_MASK) >> LEAF_SHIFT;
	m->leaf = sftl_alloc(m->nleaf * sizeof(*m->leaf));
	return m->leaf ? 0 : -ENOMEM;
}

void sftl_map_free(struct sftl_map *m)
{
	u32 i;

	if (!m->leaf)
		return;
	for (i = 0; i < m->nleaf; i++)
		sftl_free(m->leaf[i]);
	sftl_free(m->leaf);
	m->leaf = NULL;
}

u32 sftl_map_get(const struct sftl_map *m, u32 lba)
{
	const u32 *leaf = m->leaf[lba >> LEAF_SHIFT];

	return leaf ? leaf[lba & LEAF_MASK] : SFTL_UNMAPPED;
}

int sftl_map_set(struct sftl_map *m, u32 lba, u32 vba)
{
	u32 **slot = &m->leaf[lba >> LEAF_SHIFT];

	if (!*slot) {
		u32 i;

		if (vba == SFTL_UNMAPPED)
			return 0;
		*slot = sftl_alloc(LEAF_ENTRIES * sizeof(u32));
		if (!*slot)
			return -ENOMEM;
		for (i = 0; i < LEAF_ENTRIES; i++)
			(*slot)[i] = SFTL_UNMAPPED;
	}
	(*slot)[lba & LEAF_MASK] = vba;
	return 0;
}

u32 sftl_map_count_mapped(const struct sftl_map *m, u32 lbas)
{
	u32 lba, n = 0;

	for (lba = 0; lba < lbas; lba++)
		if (sftl_map_get(m, lba) != SFTL_UNMAPPED)
			n++;
	return n;
}
