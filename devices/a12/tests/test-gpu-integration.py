#!/usr/bin/env python3
"""Small offline guards for the production A12 GPU build/boot integration."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

A12 = Path(__file__).resolve().parents[1]
PROFILES = json.loads((A12/'profiles.json').read_text())
PATCH = (A12/'linux/patches/linux-0020-agx.patch').read_text()

def source(name):
    block = next(b for b in PATCH.split('diff --git ')[1:]
                 if b.splitlines()[0].endswith('b/drivers/gpu/drm/agx/'+name))
    return '\n'.join(x[1:] for x in block.splitlines()
                     if x.startswith('+') and not x.startswith('+++'))

spec = importlib.util.spec_from_file_location('a12_mesa_runtime', A12/'mesa/runtime.py')
runtime = importlib.util.module_from_spec(spec); spec.loader.exec_module(runtime)

class Integration(unittest.TestCase):
    def test_board_firmware_isolation(self):
        self.assertNotIn('gfx_ipsw', PROFILES['xr'])
        self.assertEqual(PROFILES['ipad8']['gfx_build'], '21B91')
        self.assertEqual(PROFILES['xsmax']['gfx_build'], '20G81')
        self.assertNotEqual(PROFILES['ipad8']['gfx_im4p_sha256'], PROFILES['xsmax']['gfx_im4p_sha256'])
    def test_builtin_not_manual_module(self):
        self.assertIn('--enable CONFIG_DRM_AGX', (A12/'linux/build-kernel.sh').read_text())
        self.assertNotIn('insmod', (A12/'boot/boot-device.py').read_text())
        drv = source('agx_drv.c')
        self.assertIn('cascadia,j171a-agx-21b91', drv)
        self.assertIn('apple,t8020-agx', drv)
    def test_no_embedded_bringup_tests(self):
        t = source('agx_21b91_transport.c')
        for x in ['module_param_named(j171_compute_test', 'module_param_named(j171_transport_test',
                  'DRM_IOCTL_AGX21_', 'agx21_run_minimal_compute', 'literal_shader.h']:
            self.assertNotIn(x, t)
        self.assertIn('static bool agx21_render_diag;', t)
        self.assertNotIn('static bool agx21_render_diag = true;', t)
        self.assertNotIn('agx_21b91_payload.o', source('Makefile'))
    def test_xs_features_not_downgraded(self):
        vm, fw = source('agx_vm.c'), source('agx_fw.c')
        self.assertIn('bool single', vm)
        self.assertIn('static int cut(', vm)
        self.assertIn('RN_BUDGET_W\t16384', fw)
        self.assertIn('RN_MAX_BLOCKS\t128', fw)
        self.assertIn('command_timestamp_frequency_hz = 1000000000', source('agx_drv.c'))
    def test_ipad_handoff_is_scoped(self):
        dts = (A12/'dts/t8020-j171a.dts').read_text()
        self.assertIn('gfxasc = &gfx_asc;', dts)
        self.assertIn('cascadia,g11-firmware-handoff;', dts)
        text = (A12/'m1n1/ipad8/0011-j171-gpu-handoff.patch').read_text()
        for field in ['memory-region', 'fdt_add_mem_rsv', 'apple,g11-core-perf-states',
                      'apple,g11-sram-perf-states', 'J171aAP', '21B91']:
            # memory-region is emitted by m1n1's existing reservation helper.
            if field == 'memory-region': field = 'dt_reserve_asc_firmware'
            self.assertIn(field, text)
    def test_ipad_identity_mapping_bound(self):
        text = (A12/'m1n1/ipad8/0012-j171-bounded-identity-map.patch').read_text()
        self.assertIn('chip_id == 0x8020 ? ram_size : mem_size_actual', text)
        self.assertIn('mmu_add_mapping(ram_base, ram_base, identity_size', text)

    def test_shared_mesa_pin(self):
        s = (A12/'mesa/build-mesa.sh').read_text()
        digest = hashlib.sha256((A12/'mesa/mesa-0001-asahi-g11.patch').read_bytes()).hexdigest()
        self.assertIn(digest, s)
        self.assertNotIn('/tmp/omarchy', s)
    def test_installer_refuses_host(self):
        with self.assertRaises(ValueError): runtime.install(Path('/missing'), Path('/'))
    def test_bad_target_never_activates_runtime(self):
        with tempfile.TemporaryDirectory() as d:
            stage, root = Path(d)/'stage', Path(d)/'root'
            (stage/'usr/lib/gbm').mkdir(parents=True)
            for n in ['libEGL_mesa.so.0', 'libgbm.so.1', 'libgallium-test.so', 'gbm/dri_gbm.so']:
                (stage/'usr/lib'/n).write_bytes(b'fixture')
            runtime.manifest(stage, 'test', 'test')
            (root/'etc').mkdir(parents=True); (root/'etc/os-release').write_text('ID=test\n')
            failure = subprocess.CompletedProcess([], 1, 'libLLVM: not found')
            with patch.object(runtime.subprocess, 'run', return_value=failure):
                with self.assertRaises(ValueError): runtime.install(stage, root)
            self.assertFalse((root/'usr/local/bin/cascadia-gpu').exists())

    def test_runtime_upgrade_failure_preserves_existing(self):
        with tempfile.TemporaryDirectory() as d:
            stage, root = Path(d)/'stage', Path(d)/'root'
            (stage/'usr/lib/gbm').mkdir(parents=True)
            for n in ['libEGL_mesa.so.0', 'libgbm.so.1', 'libgallium-test.so', 'gbm/dri_gbm.so']:
                (stage/'usr/lib'/n).write_bytes(b'known-working')
            runtime.manifest(stage, 'test', 'test')
            runtime.manifest(stage, 'test', 'test')  # Repeat must not hash itself.
            (root/'etc').mkdir(parents=True); (root/'etc/os-release').write_text('ID=test\n')
            success = subprocess.CompletedProcess([], 0, 'dependencies OK')
            with patch.object(runtime.subprocess, 'run', return_value=success):
                runtime.install(stage, root)
            installed = root/'opt/cascadia/mesa/lib/libgallium-test.so'
            wrapper = root/'usr/local/bin/cascadia-gpu'
            old_wrapper = wrapper.read_bytes()
            (stage/'usr/lib/libgallium-test.so').write_bytes(b'incompatible-new-build')
            runtime.manifest(stage, 'test', 'test')
            failure = subprocess.CompletedProcess([], 1, 'libLLVM: not found')
            with patch.object(runtime.subprocess, 'run', return_value=failure):
                with self.assertRaises(ValueError): runtime.install(stage, root)
            self.assertEqual(installed.read_bytes(), b'known-working')
            self.assertEqual(wrapper.read_bytes(), old_wrapper)
            with patch.object(runtime.subprocess, 'run', return_value=success), \
                 patch.object(runtime.os, 'replace', side_effect=OSError('fixture activation failure')):
                with self.assertRaises(OSError): runtime.install(stage, root)
            self.assertEqual(installed.read_bytes(), b'known-working')
            self.assertEqual(wrapper.read_bytes(), old_wrapper)
            self.assertFalse(list((root/'opt/cascadia').glob('.mesa-*')))

if __name__ == '__main__': unittest.main()
