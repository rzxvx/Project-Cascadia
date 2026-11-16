/* SPDX-License-Identifier: GPL-2.0-only */
/* Ladder rung 5 (Linux context committed and withdrawn), harness and module. */
#ifndef RUNG5_H
#define RUNG5_H

#include "sftl.h"

struct rung5_args {
	const struct sftl_ops *ops;
	void *hw;
};

int rung5_run(const struct rung5_args *a);

#endif
