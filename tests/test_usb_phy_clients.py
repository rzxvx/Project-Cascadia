"""Independent expected transitions for actual pinned PHY arbitration code."""
import importlib.util
import os
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
try:
    import unicorn
except ModuleNotFoundError:
    unicorn = None
if unicorn:
    spec = importlib.util.spec_from_file_location('usb_phy_clients', ROOT / 'scripts/emulate-usb-phy-clients.py')
    emulator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emulator)


@unittest.skipUnless(unicorn is not None and CACHE.exists(), 'requires local pinned firmware and Unicorn')
class SharedClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = CACHE.read_bytes()

    def callbacks(self, requests):
        result = emulator.experiment(self.data, requests)
        self.assertFalse(result['hardware_access'])
        self.assertTrue(result['power_and_mode_callbacks_stubbed'])
        self.assertFalse(result['wlan_continuity_or_electrical_power_verified'])
        self.assertTrue(all(state['return_code'] == '0x0' for state in result['states']))
        return [[(call['callback'], call['argument']) for call in state['callbacks']]
                for state in result['states']], result['states']

    def test_two_host_clients_keep_host_until_last_release(self):
        calls, states = self.callbacks([(1, 4), (2, 4), (1, 0), (2, 0)])
        self.assertEqual(calls, [[('power', 2), ('host', 1)], [('power', 2)],
                                [('power', 2)], [('host', 0), ('power', 0)]])
        self.assertEqual([state['collections'][2] for state in states], [[1], [1, 2], [2], []])

    def test_migration_removes_previous_request_before_new_role_callback(self):
        calls, states = self.callbacks([(9, 4), (9, 8), (9, 0)])
        self.assertEqual(calls, [[('power', 2), ('host', 1)],
                                [('power', 3), ('host', 0), ('device', 1)],
                                [('device', 0), ('power', 0)]])
        self.assertEqual(states[1]['collections'], [[], [], [], [9], []])

    def test_level_four_request_prevents_global_powerdown_after_host_release(self):
        calls, _ = self.callbacks([(1, 16), (2, 4), (2, 0), (1, 0)])
        self.assertEqual(calls, [[('power', 4)], [('power', 4), ('host', 1)],
                                [('power', 4), ('host', 0)], [('power', 0)]])

    def test_repeated_same_client_does_not_duplicate_membership_or_host_callback(self):
        calls, states = self.callbacks([(3, 4), (3, 4), (3, 0)])
        self.assertEqual(calls, [[('power', 2), ('host', 1)], [('power', 2)],
                                [('host', 0), ('power', 0)]])
        self.assertEqual(states[1]['collections'][2], [3])

    def test_no_client_and_level_zero_both_request_power_zero(self):
        calls, states = self.callbacks([(1, 0), (1, 1), (1, 0)])
        self.assertEqual(calls, [[('power', 0)], [('power', 0)], [('power', 0)]])
        self.assertEqual(states[1]['collections'], [[1], [], [], [], []])

    def test_invalid_requests_and_unpinned_firmware_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            emulator.experiment(b'not firmware', [(1, 4)])
        for requests in ([], [(1, 4)] * 65, [(1,)], [(0, 4)], [(-1, 4)],
                         [(65536, 4)], [(True, 4)], [(1, True)], [(1, -1)], [(1, 32)]):
            with self.subTest(requests=requests[:2]):
                with self.assertRaises(ValueError):
                    emulator.experiment(self.data, requests)


if __name__ == '__main__':
    unittest.main()
