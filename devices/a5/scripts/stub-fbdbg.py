#!/usr/bin/env python3
import sys, os

tree = sys.argv[1]
c_path = os.path.join(tree, 'arch/arm/mach-apple/p105_fb_dbg.c')

stub = '''// SPDX-License-Identifier: GPL-2.0-only
/* Stubbed out — no identity map for FB phys addr */
#include <linux/init.h>
#include <linux/types.h>
#include "p105_fb_dbg.h"

void p105_fb_dbg(const char *s) { (void)s; }
void p105_fb_dbg_hex(const char *tag, unsigned long val) { (void)tag; (void)val; }
void p105_fb_dbg_shutdown(void) {}
void p105_fb_dbg_use_virt(void) {}
'''

with open(c_path, 'w') as f:
    f.write(stub)
print('stubbed p105_fb_dbg.c')
