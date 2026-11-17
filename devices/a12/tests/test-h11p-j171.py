#!/usr/bin/env python3
"""Offline guards for board-specific J171 scanout; not hardware tests."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

A12=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(A12/'boot'))
from device_common import profile,write_manifest
spec=importlib.util.spec_from_file_location('h11p_boot',A12/'boot/boot-device.py')
boot=importlib.util.module_from_spec(spec);spec.loader.exec_module(boot)


def patched_driver():
    base=(A12/'linux/patches/linux-0007-apple-h11p-display.patch').read_text()
    block=next(s for s in base.split('diff --git ')[1:]
               if s.splitlines()[0].endswith('b/drivers/gpu/drm/tiny/apple-h11p.c'))
    source='\n'.join(s[1:] for s in block.splitlines()
                     if s.startswith('+') and not s.startswith('+++'))+'\n'
    return source



class H11PJ171(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.source=patched_driver()

    def test_board_specific_display_defaults(self):
        ipad=(A12/'dts/t8020-j171a.dts').read_text()
        self.assertIn('compatible = "cascadia,j171a-display-takeover";',ipad)
        self.assertIn('&dsi { status = "disabled"; };',ipad)
        self.assertIn('&disp0 { status = "disabled"; };',(A12/'dts/t8020-n841.dts').read_text())
        self.assertNotIn('cascadia,j171a-display-takeover',(A12/'dts/t8020-d331p.dts').read_text())
        self.assertIn('of_machine_is_compatible("apple,j171a")',self.source)
        self.assertIn('{ .compatible = "apple,t8020-display-pipe" }',self.source)

    def test_preserve_measured_format(self):
        self.assertIn('h11p->j171_takeover ? h11p->boot_format :',self.source)
        self.assertIn('h11p->boot_format != 0x055c8000',self.source)
        self.assertIn('h11p->j171_takeover ? 1 :',self.source)
        self.assertIn('fb->format->format != DRM_FORMAT_XRGB8888',self.source)
        self.assertIn('GP_FORMAT_XRGB8888\t0x057c8000',self.source) # XS unchanged.

    def test_guard_before_eviction(self):
        s=self.source
        hold=s.index('ret = devm_pm_domain_attach_list')
        pre=s.index('ret = apple_h11p_j171_preflight(h11p, &preflight_bo)')
        evict=s.index('ret = aperture_remove_all_conflicting_devices')
        self.assertLess(hold,pre);self.assertLess(pre,evict)
        self.assertIn('lower_32_bits(res.start) != readl(h11p->read + GP_SURFACE)',s)
        self.assertIn('drm_gem_dma_create(&h11p->drm, size)',s)
        self.assertIn('drm_gem_object_put(&preflight_bo->base)',s)

    def test_actual_c_extent_guard(self):
        helper=re.search(r'static bool apple_h11p_j171_extent\([^}]+\n}',self.source).group()
        program='''#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
typedef uint64_t u64; typedef uint64_t dma_addr_t;
struct apple_h11p { uint32_t surface_hi; };
#define GP_SURFACE_ALIGN 64
#define upper_32_bits(x) ((uint32_t)((uint64_t)(x)>>32))
#define IS_ALIGNED(x,a) (((x)&((a)-1))==0)
#define check_add_overflow(a,b,p) __builtin_add_overflow(a,b,p)
'''+helper+'''
int main(void) {
 struct apple_h11p h={8};
 assert(apple_h11p_j171_extent(&h,0x800000000ULL,0xd72800));
 assert(apple_h11p_j171_extent(&h,0x8ffffffc0ULL,64));
 assert(!apple_h11p_j171_extent(&h,0x8ffffffc0ULL,65));
 assert(!apple_h11p_j171_extent(&h,0x800000001ULL,4096));
 assert(!apple_h11p_j171_extent(&h,0x900000000ULL,4096));
 assert(!apple_h11p_j171_extent(&h,0x800000000ULL,0));
 h.surface_hi=0xffffffff;
 assert(!apple_h11p_j171_extent(&h,0xffffffffffffffc0ULL,65));
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'extent.c';src.write_text(program);exe=Path(tmp)/'extent'
            subprocess.run(['cc','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_alternate_bootset_keeps_profile_and_hash_guards(self):
        for bad in ('none','board','hash'):
            with tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);out=root/'test-boot';out.mkdir()
                p=profile.paths('ipad8');original_boot=p['boot'];p['nfsroot']=root/'nfsroot'
                p['nfsroot'].write_text('172.16.42.2:/srv/test\n')
                smart=out/'SmartIO.img4';smart.write_bytes(b'smart')
                gfx=out/'gfx.img4';gfx.write_bytes(b'gfx')
                linux=out/'linux.img4';linux.write_bytes(b'linux')
                write_manifest(out/'bootset.json',p,[smart,gfx],serial=False)
                write_manifest(out/'linux.json',p,[linux],nfsroot='172.16.42.2:/srv/test')
                if bad=='board':write_manifest(out/'linux.json',profile.paths('xr'),[linux],nfsroot='172.16.42.2:/srv/test')
                if bad=='hash':linux.write_bytes(b'changed')
                with patch.dict(os.environ,{},clear=True), \
                     patch.object(boot.profile,'paths',return_value=p), \
                     patch.object(sys,'argv',['boot-device.py','--device','ipad8','--boot-dir',str(out),'--verify-only']), \
                     contextlib.redirect_stdout(io.StringIO()):
                    if bad=='none':boot.main()
                    else:
                        with self.assertRaises(ValueError):boot.main()
                self.assertEqual(p['boot'],original_boot)


if __name__=='__main__':unittest.main()
