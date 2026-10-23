#ifndef TEST_MUTEX_H
#define TEST_MUTEX_H
#include <assert.h>
struct mutex { int held; };
static inline void mutex_init(struct mutex *m) { m->held=0; }
static inline void mutex_lock(struct mutex *m) { assert(!m->held);m->held=1; }
static inline void mutex_unlock(struct mutex *m) { assert(m->held);m->held=0; }
#endif
static inline int mutex_is_locked(struct mutex *m) { return m->held; }
