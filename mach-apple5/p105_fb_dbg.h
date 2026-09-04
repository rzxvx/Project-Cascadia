/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MACH_APPLE_P105_FB_DBG_H
#define _MACH_APPLE_P105_FB_DBG_H

#include <linux/types.h>

/*
 * Early framebuffer text for P105AP (iPad mini 1) Recovery scanout.
 * Panel is portrait 768x1024, stride 3072, BGRA @ 0x9F6FC000.
 */
void p105_fb_dbg_clear(void);
void p105_fb_dbg(const char *msg);
void p105_fb_dbg_hex(const char *tag, unsigned long val);
void p105_fb_dbg_use_virt(void);
/* Stop phys-FB writes before free_initmem / idle (identity map goes away). */
void p105_fb_dbg_shutdown(void);

/* Spawn init on alt stack with IRQs on; returns init pid or negative errno. */
pid_t __init apple_s5l_rest_spawn_init(int (*fn)(void *), void *arg,
				       unsigned long flags);

#endif
