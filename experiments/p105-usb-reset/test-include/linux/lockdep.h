#ifndef FIXTURE_LOCKDEP_H
#define FIXTURE_LOCKDEP_H
#include <assert.h>
#define lockdep_assert_held(p) assert((p)->held)
#endif
