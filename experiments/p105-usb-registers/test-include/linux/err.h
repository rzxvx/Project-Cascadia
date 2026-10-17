#ifndef FIXTURE_ERR_H
#define FIXTURE_ERR_H
#include <stdint.h>
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p) >= (uintptr_t)-4095)
#endif
