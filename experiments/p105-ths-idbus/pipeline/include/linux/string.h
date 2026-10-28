/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CASCADIA_PIPELINE_FAKE_STRING_H
#define CASCADIA_PIPELINE_FAKE_STRING_H
#include <string.h>
static inline const void *memchr_inv(const void *p,int v,size_t n)
{const unsigned char *b=p;for(size_t i=0;i<n;i++)if(b[i]!=(unsigned char)v)return b+i;return NULL;}
#endif
