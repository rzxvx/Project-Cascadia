/* SPDX-License-Identifier: GPL-2.0-only */
/* Interface between the PPN controller driver and the Swiss FTL. */
#ifndef APPLE_PPN_H
#define APPLE_PPN_H

#include <linux/types.h>

struct apple_ppn;
struct sftl_prog;

/* The package's geometry, from its device parameters. */
struct apple_ppn_geo {
	unsigned int blocks;		/* per CAU */
	unsigned int mlc_pages;		/* per block */
	unsigned int slc_pages;
	unsigned int page_bits, block_bits, cau_bits;	/* row address fields */
	char firmware[17];		/* feature 0x9080, NUL-terminated */
	bool reference;			/* the tested 040472P_HYNIX_B_ 1064 x 256 package */
};

/* Grab the single bound controller, or NULL if none is bound. */
struct apple_ppn *apple_ppn_get(void);
void apple_ppn_put(struct apple_ppn *ppn);

/* True when the controller was loaded with allow_write=1. */
bool apple_ppn_write_allowed(struct apple_ppn *ppn);
void apple_ppn_geometry(struct apple_ppn *ppn, struct apple_ppn_geo *geo);

int apple_ppn_read_page(struct apple_ppn *ppn, unsigned int bank,
			unsigned int block, unsigned int page, bool slc,
			u8 *data, u8 *meta, u8 *status);
int apple_ppn_erase(struct apple_ppn *ppn, unsigned int bank,
		    unsigned int block);
int apple_ppn_program(struct apple_ppn *ppn, const struct sftl_prog *pages,
		      unsigned int n);

#endif
