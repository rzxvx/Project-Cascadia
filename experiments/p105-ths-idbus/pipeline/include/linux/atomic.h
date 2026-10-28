/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FAKE_ATOMIC_H
#define FAKE_ATOMIC_H
#include <stdatomic.h>
#include <linux/types.h>
typedef _Atomic s64 atomic64_t;
#define ATOMIC64_INIT(v) (v)
static inline s64 atomic64_read(atomic64_t *p) { return atomic_load(p); }
static inline s64 atomic64_cmpxchg(atomic64_t *p, s64 old, s64 newv)
{ atomic_compare_exchange_strong(p, &old, newv); return old; }
static inline void atomic64_set(atomic64_t *p, s64 v) { atomic_store(p, v); }

typedef _Atomic int atomic_t;
static inline int atomic_read(atomic_t *p) { return atomic_load(p); }
static inline void atomic_set(atomic_t *p, int v) { atomic_store(p, v); }
#endif
