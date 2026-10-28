#!/usr/bin/env python3
"""Offline fixtures for safe telemetry and strict binary dump comparison."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PowerTools(unittest.TestCase):
    def test_snapshot_allowlist_and_units(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            for key, value in {'status': 'Discharging', 'current_now': '-235000',
                               'voltage_now': '3987000', 'temp': '251',
                               'cycle_count': '12-45',
                               'serial_number': 'MUST-NOT-BE-COLLECTED'}.items():
                (path / key).write_text(value + '\n')
            result = subprocess.run(['sh', str(ROOT / 'tools/power/battery-snapshot.sh'),
                                     str(path)], capture_output=True, text=True, check=True)
            data = dict(line.split('=', 1) for line in result.stdout.splitlines())
            self.assertEqual(data['current_now'], '-235000')
            self.assertEqual(data['temp'], '251')
            self.assertEqual(data['capacity'], 'unavailable')
            self.assertEqual(data['cycle_count'], 'unavailable')
            self.assertNotIn('serial_number', data)
            self.assertNotIn('MUST-NOT-BE-COLLECTED', result.stdout)

    def test_snapshot_absent(self):
        with tempfile.TemporaryDirectory() as temp:
            result = subprocess.run(['sh', str(ROOT / 'tools/power/battery-snapshot.sh'),
                                     str(Path(temp) / 'absent')], capture_output=True)
            self.assertEqual(result.returncode, 1)

    def test_diff_preserves_exact_bit_changes(self):
        with tempfile.TemporaryDirectory() as temp:
            before = Path(temp) / 'before.bin'
            after = Path(temp) / 'after.bin'
            before.write_bytes(bytes(128))
            changed = bytearray(128)
            changed[0x3d] = 0x20
            changed[0x3f] = 0x40
            after.write_bytes(changed)
            result = subprocess.run(['python3', str(ROOT / 'tools/power/pmu-diff.py'),
                                     str(before), str(after)], capture_output=True,
                                    text=True, check=True)
            data = json.loads(result.stdout)
            self.assertEqual(data['changed'], [
                {'offset': '0x3d', 'before': '0x00', 'after': '0x20', 'changed_bits': '0x20'},
                {'offset': '0x3f', 'before': '0x00', 'after': '0x40', 'changed_bits': '0x40'},
            ])
            self.assertEqual(before.read_bytes(), bytes(128))
            self.assertEqual(after.read_bytes(), bytes(changed))

    def test_diff_rejects_truncated_or_oversized_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            before = Path(temp) / 'before.bin'
            after = Path(temp) / 'after.bin'
            before.write_bytes(bytes(128))
            for size in (0, 127, 129, 256):
                after.write_bytes(bytes(size))
                result = subprocess.run(['python3', str(ROOT / 'tools/power/pmu-diff.py'),
                                         str(before), str(after)], capture_output=True)
                self.assertEqual(result.returncode, 2)


if __name__ == '__main__':
    unittest.main()
