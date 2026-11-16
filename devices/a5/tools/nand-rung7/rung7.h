/* SPDX-License-Identifier: GPL-2.0-only */
/* Ladder rung 7 (real reclaim + commit), shared by harness and module. */
#ifndef RUNG7_H
#define RUNG7_H

#include "sftl.h"

struct rung7_args {
	const struct sftl_ops *ops;
	void *hw;
};

int rung7_run(const struct rung7_args *a);

#endif
