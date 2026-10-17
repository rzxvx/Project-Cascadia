#ifndef FAKE_MUTEX_H
#define FAKE_MUTEX_H
#include <pthread.h>
#include <assert.h>
struct mutex { pthread_mutex_t value; bool locked; };
#define MUTEX_INITIALIZER { PTHREAD_MUTEX_INITIALIZER, false }
static inline void mutex_lock(struct mutex *p)
{ assert(!pthread_mutex_lock(&p->value)); assert(!p->locked); p->locked=true; }
static inline void mutex_unlock(struct mutex *p)
{ assert(p->locked); p->locked=false; assert(!pthread_mutex_unlock(&p->value)); }
#endif
