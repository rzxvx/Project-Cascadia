"""Actual Linux C callbacks versus real firmware, with no hardware backend.

The fixed runtime source is compiled without mutation. A temporary legacy-mask
control reproduces the old mismatch; this test does not modify the driver.
"""
import importlib.util
from pathlib import Path
import shutil
import unittest

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / 'build/firmware/kernelcache.12H321.macho'
try:
    import unicorn
except ModuleNotFoundError:
    unicorn = None
if unicorn:
    spec = importlib.util.spec_from_file_location('soc_gpio_comparison', ROOT / 'scripts/compare-soc-gpio.py')
    comparison = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(comparison)


@unittest.skipUnless(unicorn is not None and CACHE.exists() and shutil.which('cc'),
                     'requires local verified firmware, Unicorn and existing C compiler')
class SoCGPIOMasks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = CACHE.read_bytes()
        cls.callbacks = comparison.LinuxCallbacks()

    @classmethod
    def tearDownClass(cls):
        cls.callbacks.close()

    def test_direction_output_actual_fix_matches_oracle_legacy_masks_reproduce_bug(self):
        for initial in (0x290, 0x2B0, 0x2D0, 0x2F0, 0x29E, 0xFFFF):
            for operation, mode in ((0, 14), (1, 15)):
                with self.subTest(initial=hex(initial), operation=operation):
                    oracle = comparison.firmware_mode(self.data, mode, initial)
                    legacy = self.callbacks.run(operation, initial, legacy_control=True)
                    runtime = self.callbacks.run(operation, initial)
                    self.assertEqual(runtime['final'], oracle['final'])
                    self.assertEqual(legacy['final'] ^ oracle['final'], initial & 0x40)
                    self.assertFalse(runtime['hardware_access'])
                    self.assertEqual(len(runtime['writes']), 1)

    def test_set_on_output_uses_same_problematic_refresh_branch(self):
        for initial in (0x292, 0x293, 0x2D2, 0x2D3):
            for operation, mode in ((2, 14), (3, 15)):
                with self.subTest(initial=hex(initial), operation=operation):
                    oracle = comparison.firmware_mode(self.data, mode, initial)
                    legacy = self.callbacks.run(operation, initial, legacy_control=True)
                    runtime = self.callbacks.run(operation, initial)
                    self.assertEqual(runtime['final'], oracle['final'])
                    self.assertEqual(legacy['final'] ^ oracle['final'], initial & 0x40)

    def test_set_covers_each_refresh_branch_without_changing_software_policy(self):
        # Independent expected values, including the enabled-IRQ branch. These
        # cases exercise extracted C; they do not replace Linux policy with a
        # new Python implementation or assert full input/IRQ firmware parity.
        for initial, operation, initial_stat, final_stat, legacy_expected, runtime_expected in (
                (0x2D2, 3, 0x02, 0x03, 0x2D3, 0x293),  # output
                (0x2D4, 2, 0x80, 0x80, 0x2D4, 0x294),  # enabled IRQ low value
                (0x2D4, 3, 0x80, 0x81, 0x2D5, 0x295),  # enabled IRQ high value
                (0x2D0, 3, 0x00, 0x01, 0x2DF, 0x29F),  # input/IRQ off
                (0x2F0, 3, 0x20, 0x21, 0x2F1, 0x2B1),  # inherited peripheral
                (0x2F0, 6, 0x20, 0x00, 0x2DE, 0x29E)):  # explicit GPIO mux
            with self.subTest(initial=hex(initial), operation=operation):
                old = self.callbacks.run(operation, initial, legacy_control=True)
                new = self.callbacks.run(operation, initial)
                self.assertEqual(old['final'], legacy_expected)
                self.assertEqual(new['final'], runtime_expected)
                self.assertEqual(new['initial_stat'], initial_stat)
                self.assertEqual(new['final_stat'], final_stat)
                self.assertEqual(old['final_stat'], new['final_stat'])
                self.assertEqual(old['final'] ^ new['final'], 0x40)

    def test_explicit_peripheral_selection_clears_second_mux_bit_in_actual_source(self):
        for initial in (0x290, 0x2D0, 0x2F0, 0xFFFE):
            with self.subTest(initial=hex(initial)):
                oracle = comparison.firmware_mode(self.data, 2, initial)
                legacy = self.callbacks.run(5, initial, legacy_control=True)
                runtime = self.callbacks.run(5, initial)
                self.assertEqual(runtime['final'] & 0x60, 0x20)
                self.assertEqual(runtime['final'] & 0x60, oracle['final'] & 0x60)
                self.assertEqual(legacy['final'] & 0x60, 0x20 | (initial & 0x40))
                if initial != 0xFFFE:
                    self.assertEqual(runtime['final'], oracle['final'])
                else:
                    # Firmware mode2 preserves the old IRQ field; current
                    # Linux peripheral refresh clears it. Separate policy.
                    self.assertEqual(runtime['final'], 0xFFB0)
                    self.assertEqual(oracle['final'], 0xFFBE)

    def test_firmware_has_three_distinct_function_settings(self):
        for mode, expected in ((2, 0x2B0), (3, 0x2D0), (4, 0x2F0)):
            with self.subTest(mode=mode):
                result = comparison.firmware_mode(self.data, mode, 0x290)
                self.assertEqual(result['writes'], [expected])
                self.assertEqual(len(result['external_calls']), 4)

    def test_input_preserves_unrelated_irq_policy_difference(self):
        oracle = comparison.firmware_mode(self.data, 0, 0x2D0)
        legacy = self.callbacks.run(4, 0x2D0, legacy_control=True)
        runtime = self.callbacks.run(4, 0x2D0)
        self.assertEqual(oracle['final'], 0x290)
        self.assertEqual(legacy['final'], 0x2DE)
        self.assertEqual(runtime['final'], 0x29E)
        self.assertEqual(runtime['final'] & 0x60, oracle['final'] & 0x60)
        self.assertNotEqual(runtime['final'], oracle['final'])
        # The separate explicit-input fix clears cached PERIPH before refresh;
        # it keeps the existing Linux input IRQ policy rather than copying mode0.
        inherited = self.callbacks.run(4, 0x2B0)
        self.assertEqual(inherited['initial_stat'], 0x20)
        self.assertEqual(inherited['final_stat'], 0)
        self.assertEqual(inherited['final'], 0x29E)
        self.assertEqual(inherited['final'] & 0x60, 0)

    def test_initial_alternate_function_is_not_recognized_as_peripheral(self):
        result = self.callbacks.run(4, 0x2D0)
        self.assertEqual(result['initial_stat'], 0)
        # Bit20 inherited peripheral state remains recognized by initialization;
        # explicit direction_input now clears that cached flag after decoding.
        other = self.callbacks.run(4, 0x2B0)
        self.assertEqual(other['initial_stat'], 0x20)

    def test_cfg_done_and_upper_fields_are_separate_from_mux_mask(self):
        runtime = self.callbacks.run(1, 0xD0)
        oracle = comparison.firmware_mode(self.data, 15, 0xD0)
        self.assertEqual(runtime['writes'], [0x93, 0x293])
        self.assertEqual(oracle['writes'], [0x93])
        self.assertEqual(runtime['final'] ^ oracle['final'], 0x200)
        high = self.callbacks.run(1, 0xA50002D0)
        self.assertEqual(high['final'], 0xA5000293)
        # Current Linux peripheral refresh clears DATA; firmware mode2 does
        # not. This is also intentionally excluded from the mask-only fix.
        periph = self.callbacks.run(5, 0x291)
        periph_oracle = comparison.firmware_mode(self.data, 2, 0x291)
        self.assertEqual(periph['final'], 0x2B0)
        self.assertEqual(periph_oracle['final'], 0x2B1)

    def test_unverified_firmware_unaudited_inputs_and_changed_control_shape_refused(self):
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            comparison.firmware_mode(b'unverified', 14, 0x290)
        for mode in (1, 16, True, 14.0):
            with self.assertRaisesRegex(ValueError, 'audited'):
                comparison.firmware_mode(self.data, mode, 0x290)
        for shadow in (-1, 0x10000, True):
            with self.assertRaisesRegex(ValueError, '16-bit'):
                comparison.firmware_mode(self.data, 14, shadow)
        with self.assertRaisesRegex(ValueError, 'operation'):
            self.callbacks.run(7, 0x290)
        with self.assertRaisesRegex(ValueError, '32-bit'):
            self.callbacks.run(1, -1)
        with self.assertRaisesRegex(ValueError, 'boolean'):
            self.callbacks.run(1, 0x290, legacy_control=1)
        changed = self.callbacks.text.replace('REG_GPIOx_MUX_MASK,', 'DIFFERENT_MASK,')
        with self.assertRaisesRegex(ValueError, 'shape changed'):
            comparison.linux_harness(changed, True)


if __name__ == '__main__':
    unittest.main()
