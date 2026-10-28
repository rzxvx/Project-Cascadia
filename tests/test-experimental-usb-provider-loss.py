#!/usr/bin/env python3
"""Actual backend C: lost callbacks cannot silently resume retained ownership."""
import ctypes
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPECS = {
    'power': ('cascadia-phy-power.c', 'power-fixture.c', (11, 12), 14, 7, 5),
    'reset': ('cascadia-reset-client.c', 'reset-fixture.c', (9, 10), 12, 7, 5),
    'controller': ('cascadia-controller.c', 'controller-fixture.c', (12,), 15, 4, 6),
}


class LostProvider(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.controls = json.loads((ROOT/'tests/fixtures/usb-provider-loss.json').read_text())['blocks']
        cls.libs = {}
        for name, spec in SPECS.items():
            source, fixture, *_ = spec
            here = ROOT/'experiments'/('p105-usb-'+name)
            current = (here/source).read_text()
            block = cls.controls[name]
            if current.count(block['new']) != 1:
                raise AssertionError('negative control no longer matches '+name)
            for old in (False, True):
                stage = Path(cls.tmp.name)/(name+('-old' if old else '-fixed'))
                stage.mkdir()
                (stage/source).write_text(current.replace(block['new'], block['old'], 1) if old else current)
                so = stage/'backend.so'
                subprocess.run(['cc', '-shared', '-fPIC', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                                '-I'+str(here/'test-include'), '-I'+str(here),
                                str(stage/source), str(here/fixture), '-o', str(so)], check=True)
                lib = ctypes.CDLL(str(so))
                if name == 'power':
                    lib.start.argtypes = [ctypes.c_int, ctypes.c_uint, ctypes.c_ulong]
                    lib.transition.argtypes = [ctypes.c_uint64, ctypes.c_uint]
                elif name == 'reset':
                    lib.update.argtypes = [ctypes.c_uint64, ctypes.c_uint, ctypes.c_uint, ctypes.c_int]
                else:
                    lib.operation.argtypes = [ctypes.c_uint64, ctypes.c_int, ctypes.c_int]
                cls.libs[name, old] = lib

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def start(self, name, lib, active=False):
        if name == 'power':
            self.assertEqual(lib.start(1, 2 if active else 0, 24000000), 0)
        elif name == 'reset':
            self.assertEqual(lib.start(1, 0x3f), 0)
        else:
            self.assertEqual(lib.start(1, 0x1ff, 0xa50000e1), 0)

    def request(self, name, lib, initial=False):
        if name == 'power':
            return lib.transition(7, 2 if initial else 3)
        if name == 'reset':
            return lib.update(7, 1 if initial else 2, 0, 1)
        return lib.operation(7, 1, 0)

    def retained_case(self, name):
        _, _, missing, restore, io, fault = SPECS[name]
        for field in missing:
            for old in (False, True):
                lib = self.libs[name, old]
                self.start(name, lib)
                self.assertEqual(self.request(name, lib, initial=True), 0)
                before = lib.inspect(io, 0)
                if name == 'power': retained = lib.inspect(4, 0)
                elif name == 'reset': retained = lib.inspect(4, 1)
                else: retained = lib.inspect(8, 0)
                lib.inject(field, 0)
                self.assertEqual(self.request(name, lib), -19)
                self.assertEqual(lib.inspect(io, 0), before)
                self.assertEqual(lib.inspect(fault, 0), int(not old))
                lib.inject(restore, 0)
                self.assertEqual(self.request(name, lib), 0 if old else -19)
                if not old:
                    self.assertEqual(lib.inspect(io, 0), before)
                    if name == 'power': self.assertEqual(lib.inspect(4, 0), retained)
                    elif name == 'reset': self.assertEqual(lib.inspect(4, 1), retained)
                    else: self.assertEqual(lib.inspect(8, 0), retained)

    def test_power_retains_active_state(self):
        self.retained_case('power')

    def test_reset_retains_memberships(self):
        self.retained_case('reset')

    def test_controller_retains_completed_disposition(self):
        self.retained_case('controller')

    def test_cold_missing_provider_remains_retryable_without_io(self):
        for name, spec in SPECS.items():
            _, _, missing, restore, io, fault = spec
            lib = self.libs[name, False]
            for field in missing:
                self.start(name, lib)
                lib.inject(field, 0)
                self.assertEqual(self.request(name, lib), -19)
                self.assertEqual(lib.inspect(fault, 0), 0)
                self.assertEqual(lib.inspect(io, 0), 0)
                lib.inject(restore, 0)
                self.assertEqual(self.request(name, lib), 0)

    def test_adopted_active_power_faults_even_without_own_write(self):
        lib = self.libs['power', False]
        for field in (11, 12):
            self.start('power', lib, active=True)
            lib.inject(field, 0)
            self.assertEqual(self.request('power', lib), -19)
            self.assertEqual(lib.inspect(5, 0), 1)
            lib.inject(14, 0)
            self.assertEqual(self.request('power', lib), -19)
            self.assertEqual(lib.inspect(7, 0), 0)
            self.assertEqual(lib.inspect(4, 0), 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
