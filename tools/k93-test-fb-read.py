#!/usr/bin/env python3
"""Compile actual framebuffer mode checks/pixel conversion with host fixtures."""
from pathlib import Path
import subprocess
import tempfile

source=Path(__file__).with_name('k93-fb-read.c').read_text()
functions=source[source.index('static int valid_mode('):source.index('int main(')]
program=r'''
#include <assert.h>
#include <linux/fb.h>
#include <string.h>
'''+functions+r'''
int main(void) {
 struct fb_fix_screeninfo f={.type=FB_TYPE_PACKED_PIXELS,.visual=FB_VISUAL_TRUECOLOR,.line_length=4096,.smem_len=4096*768};
 struct fb_var_screeninfo v={.xres=1024,.yres=768,.bits_per_pixel=32,
 .red={.offset=16,.length=8},.green={.offset=8,.length=8},.blue={.offset=0,.length=8}};
 assert(valid_mode(&f,&v));
 v.xres--;assert(!valid_mode(&f,&v));v.xres++;
 v.yoffset=1;assert(!valid_mode(&f,&v));v.yoffset=0;
 v.bits_per_pixel=24;assert(!valid_mode(&f,&v));v.bits_per_pixel=32;
 v.red.offset=0;assert(!valid_mode(&f,&v));v.red.offset=16;
 v.blue.msb_right=1;assert(!valid_mode(&f,&v));v.blue.msb_right=0;
 f.line_length--;assert(!valid_mode(&f,&v));f.line_length++;
 f.smem_len--;assert(!valid_mode(&f,&v));f.smem_len++;
 f.visual=FB_VISUAL_PSEUDOCOLOR;assert(!valid_mode(&f,&v));f.visual=FB_VISUAL_TRUECOLOR;
 assert(valid_mode(&f,&v));
 unsigned char raw[4096],rgb[3072];
 for(unsigned x=0;x<1024;x++) {raw[x*4]=(unsigned char)(255-x);raw[x*4+1]=(unsigned char)x;raw[x*4+2]=(unsigned char)(x/4);raw[x*4+3]=0;}
 rgb_row(raw,rgb);
 for(unsigned x=0;x<1024;x++) {assert(rgb[x*3]==(unsigned char)(x/4));assert(rgb[x*3+1]==(unsigned char)x);assert(rgb[x*3+2]==(unsigned char)(255-x));}
}
'''
with tempfile.TemporaryDirectory(prefix='k93-fb-test-') as directory:
    binary=Path(directory)/'test'
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-x','c','-','-o',str(binary)],input=program,text=True,check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: actual fb mode rejection and all1024 RGB pixel conversions; no device access')
