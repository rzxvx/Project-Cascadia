/* SPDX-License-Identifier: GPL-2.0 */
#include <stdint.h>
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define ERR_PTR(v) ((void *)(intptr_t)(v))
