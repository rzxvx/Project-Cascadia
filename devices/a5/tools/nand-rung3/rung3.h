/* SPDX-License-Identifier: GPL-2.0-only */
/* Ladder rung 3 (FTL writes that are undone), shared by harness and module. */
#ifndef RUNG3_H
#define RUNG3_H

#include "sftl.h"

struct rung3_args {
	const struct sftl_ops *ops;
	void *hw;
	u32 nlbas;		/* LBAs to rewrite, 8..3000 */
	u32 search_from;	/* first LBA of the search for a mapped run */
};

int rung3_run(const struct rung3_args *a);

#endif
