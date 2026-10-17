#include <string.h>

static inline const void *memchr_inv(const void *ptr, int value, size_t n)
{ const unsigned char *p=ptr;for(size_t i=0;i<n;i++)if(p[i]!=(unsigned char)value)return p+i;return NULL; }
