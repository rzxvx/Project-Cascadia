/* SPDX-License-Identifier: GPL-2.0-only */
/* Ladder rung 6 (iOS on a Linux context): commit and check, harness and module. */
#ifndef RUNG6_H
#define RUNG6_H

#include "sftl.h"

#define RUNG6_WATCH 4

struct rung6_args {
	const struct sftl_ops *ops;
	void *hw;
	bool commit;			/* commit and leave; else check (read only) */
	u32 samples;			/* check: random mapped LBAs to read */
	u32 watch_sb[RUNG6_WATCH];	/* check: superblocks to report, 0 ends */
	u64 weave_lo, weave_hi;		/* check: also report SBs whose first weave is in [lo, hi) */
};

int rung6_run(const struct rung6_args *a);

#endif
