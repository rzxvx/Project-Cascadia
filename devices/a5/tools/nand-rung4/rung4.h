/* SPDX-License-Identifier: GPL-2.0-only */
/* Ladder rung 4 (context written unused), shared by harness and module. */
#ifndef RUNG4_H
#define RUNG4_H

#include "sftl.h"

struct rung4_args {
	const struct sftl_ops *ops;
	void *hw;
};

int rung4_run(const struct rung4_args *a);

#endif
