#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gate', ROOT / 'tools/power/charger-evidence-gate.py')
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)


def ready():
    sample = {'value': 3999, 'unit': '12H321-policy-units', 'valid': True, 'fresh': True}
    return {'epoch': 0, 'source_kind': 'usb', 'requested_input_ma': 500, 'board_cap_ma': 2100, 'floor_ma': 0,
            'voltage': sample, 'voltage_limit': sample | {'value': 4000},
            **{key: True for key in g.BOOL_FIELDS}}


class EvidenceContract(unittest.TestCase):
    def assert_blocked(self, result):
        self.assertEqual(result['input_current_ma'], 0)
        self.assertEqual(result['charge_current_ma'], 0)
        self.assertEqual(result['register_operations'], [])
        self.assertEqual(len(result['blocked_hardware_proofs']), 3)
        self.assertFalse(result['hardware_access'])

    def test_ready_software_inputs_never_enable_outputs(self):
        out = g.EvidenceGate().evaluate(ready())
        self.assertTrue(out['software_evidence_complete'])
        self.assertEqual(out['candidate_input_ma_for_review_only'], 500)
        self.assert_blocked(out)

    def test_every_missing_input_is_fail_closed(self):
        for key in g.KEYS:
            values = ready(); del values[key]
            out = g.EvidenceGate().evaluate(values)
            self.assertIn(key, out['missing_inputs'])
            self.assertEqual(out['candidate_input_ma_for_review_only'], 0)
            self.assert_blocked(out)

    def test_each_negative_status_invalidates_candidate(self):
        for key in g.BOOL_FIELDS:
            out = g.EvidenceGate().evaluate(ready() | {key: False})
            self.assertIn(key, out['rejected_inputs'])
            self.assertEqual(out['candidate_input_ma_for_review_only'], 0)
            self.assert_blocked(out)

    def test_epoch_invalidation_blocks_old_callbacks(self):
        for event in ('attach', 'detach', 'role_change', 'reset', 'fault', 'communication_error'):
            gate = g.EvidenceGate(); self.assert_blocked(gate.invalidate(event))
            self.assertIn('stale epoch', gate.evaluate(ready())['rejected_inputs'])
            out = gate.evaluate(ready() | {'epoch': 1})
            self.assertTrue(out['software_evidence_complete'])
            self.assert_blocked(out)

    def test_sample_validation_and_units(self):
        for key in ('voltage', 'voltage_limit'):
            for update in ({'valid': False}, {'fresh': False}, {'unit': 'unknown'}):
                values = ready(); values[key] = values[key] | update
                out = g.EvidenceGate().evaluate(values)
                self.assertEqual(out['candidate_input_ma_for_review_only'], 0)
                self.assert_blocked(out)
            values = ready(); values[key] = values[key] | {'unit': 'mV'}
            with self.assertRaises(ValueError): g.EvidenceGate().evaluate(values)

    def test_cutoff_equal_above_and_missing(self):
        for voltage in (4000, 4001):
            values = ready(); values['voltage'] = values['voltage'] | {'value': voltage}
            out = g.EvidenceGate().evaluate(values)
            self.assertTrue(out['software_voltage_cutoff'])
            self.assertEqual(out['candidate_input_ma_for_review_only'], 0)
            self.assert_blocked(out)
        values = ready(); values['voltage_limit'] = values['voltage_limit'] | {'value': 0}
        self.assertFalse(g.EvidenceGate().evaluate(values)['software_evidence_complete'])

    def test_floor_does_not_override_inhibit(self):
        out = g.EvidenceGate().evaluate(ready() | {'floor_ma': 100, 'permission': False})
        self.assertEqual(out['candidate_input_ma_for_review_only'], 0)
        self.assert_blocked(out)

    def test_unknown_source_and_cap_boundaries(self):
        for update in ({'source_kind': 'unknown'}, {'board_cap_ma': 0}, {'board_cap_ma': 2101}, {'requested_input_ma': 501}):
            out = g.EvidenceGate().evaluate(ready() | update)
            self.assertFalse(out['software_evidence_complete'])
            self.assert_blocked(out)
        out = g.EvidenceGate().evaluate(ready() | {'requested_input_ma': 3262, 'source_kind': 'adapter'})
        self.assertEqual(out['candidate_input_ma_for_review_only'], 2100)
        self.assert_blocked(out)

    def test_malformed_inputs_raise_before_any_actuation(self):
        for update in ({'epoch': True}, {'permission': 1}, {'source_kind': 'host'}, {'requested_input_ma': -1}, {'floor_ma': True}, {'enable_charging': True}):
            with self.assertRaises(ValueError): g.EvidenceGate().evaluate(ready() | update)
        with self.assertRaises(ValueError): g.EvidenceGate().invalidate('grant')
        with self.assertRaises(ValueError): g.EvidenceGate().evaluate([])
        values = ready(); values['voltage'] = {'value': 1}
        with self.assertRaises(ValueError): g.EvidenceGate().evaluate(values)

    def test_empty_and_returned_proof_list_cannot_enable_outputs(self):
        gate = g.EvidenceGate(); out = gate.evaluate({}); out['blocked_hardware_proofs'].clear()
        fresh = gate.evaluate(ready())
        self.assert_blocked(fresh)


if __name__ == '__main__': unittest.main()
