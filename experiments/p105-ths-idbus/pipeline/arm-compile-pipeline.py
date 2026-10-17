#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile pinned existing C components against real ARM kernel headers; no activation."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('--source-root',type=Path,required=True);ap.add_argument('--linux-source',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True)
files={'experiments/p105-ths-idbus/cascadia-ths-idbus.c':'bd6da8ec07730a79c5d3bfc9b936085bd67016c02bfc70337a0b69978f5af61c','experiments/p105-authcp-aid/cascadia-authcp-aid.c':'382cc9dd37bd18d37e9466830fe109c0b29bbe1bdab6885bf65acdff2584b529'}
opts=['-nostdinc','-D__KERNEL__','-D__LINUX_ARM_ARCH__=7','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-sign-compare','-Wno-type-limits','-marm','-march=armv7-a','-msoft-float','-fno-pic','-fno-strict-aliasing']
opts+=['-I'+p for p in ('./arch/arm/include','./arch/arm/include/generated','./include','./arch/arm/include/uapi','./arch/arm/include/generated/uapi','./include/uapi','./include/generated/uapi')]
for p in('./include/linux/compiler-version.h','./include/linux/kconfig.h','./include/linux/compiler_types.h'):opts+=['-include',p]
objects=[]
for rel,sha in files.items():
 source=a.source_root/rel;assert hashlib.sha256(source.read_bytes()).hexdigest()==sha
 obj=a.output/(source.stem+'.o');subprocess.run(['arm-linux-gnueabihf-gcc',*opts,'-c',str(source),'-o',str(obj)],cwd=a.linux_source,check=True);objects.append(str(obj))
subprocess.run(['arm-linux-gnueabihf-ld','-r',*objects,'-o',str(a.output/'pipeline-components.o')],check=True)
print(json.dumps({'source_sha256':files,'ARM_compile':'PASS strictwarnings','relocatable_link':'PASS','scope':'existing source-only transport+parser; no IRQ backend or device activation'},indent=2))
