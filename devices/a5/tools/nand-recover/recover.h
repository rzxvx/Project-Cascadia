/* SPDX-License-Identifier: GPL-2.0-only */
/* Recovery after a writer that stopped without a commit; harness and module. */
#ifndef RECOVER_H
#define RECOVER_H

#include "sftl.h"

#define RECOVER_MAX_ERASE	16

struct recover_args {
	const struct sftl_ops *ops;
	void *hw;
	u32 erase[RECOVER_MAX_ERASE];	/* spares to erase again (an erase was cut off) */
	u32 nerase;
	bool dry_run;			/* report only */
};

int recover_run(const struct recover_args *a);

#endif
