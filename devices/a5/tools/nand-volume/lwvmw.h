/* SPDX-License-Identifier: GPL-2.0-only */
/* One-shot LwVM header write (spec 6.4 step 3), shared by harness and module. */
#ifndef LWVMW_H
#define LWVMW_H

#include "sftl.h"

struct lwvmw_args {
	const struct sftl_ops *ops;
	void *hw;
	const u8 *from;		/* the header the plan was made from (4096 bytes) */
	const u8 *to;		/* the planned header (4096 bytes) */
	bool dry_run;		/* checks only: nothing is written */
};

int lwvmw_run(const struct lwvmw_args *a);

#endif
