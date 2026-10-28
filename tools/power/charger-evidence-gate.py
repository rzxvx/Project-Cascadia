#!/usr/bin/env python3
"""Offline evidence contract for a future charger; all actuator outputs blocked.

This is a synthetic validation/state-generation prototype, not Apple's policy,
a Linux driver, or a hardware authorization mechanism. Firmware units are not
physical calibration. Missing CV/termination/fault initialization proof cannot
be overridden by an input file or by marking all synthetic inputs valid.
"""
import argparse
import json
from pathlib import Path

UNPROVEN = ('hardware CV target configuration', 'termination configuration',
            'complete fault shutdown and initialization')
BOOL_FIELDS = ('source_valid', 'permission', 'battery_present', 'temperature_valid',
               'fault_snapshot_valid', 'backend_acknowledged')
KEYS = {'epoch', 'source_kind', 'requested_input_ma', 'board_cap_ma', 'floor_ma',
        'voltage', 'voltage_limit'} | set(BOOL_FIELDS)


def integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'{name} must be integer in {low}..{high}')
    return value


class EvidenceGate:
    def __init__(self): self.epoch = 0

    def invalidate(self, reason):
        if reason not in ('attach', 'detach', 'role_change', 'reset', 'fault', 'communication_error'):
            raise ValueError('unknown invalidation reason')
        self.epoch += 1
        return self.evaluate({})

    def evaluate(self, snapshot):
        if not isinstance(snapshot, dict) or set(snapshot) - KEYS:
            raise ValueError('invalid snapshot or unknown keys')
        missing = sorted(KEYS - set(snapshot))
        rejected = []
        for key in BOOL_FIELDS:
            if key in snapshot:
                if type(snapshot[key]) is not bool: raise ValueError(f'{key} must be boolean')
                if not snapshot[key]: rejected.append(key)
        if 'epoch' in snapshot:
            integer(snapshot['epoch'], 0, 0xffffffff, 'epoch')
            if snapshot['epoch'] != self.epoch: rejected.append('stale epoch')
        if 'source_kind' in snapshot:
            if snapshot['source_kind'] not in ('usb', 'adapter', 'unknown'): raise ValueError('unknown source_kind')
            if snapshot['source_kind'] == 'unknown': rejected.append('unknown source classification')
        for name in ('requested_input_ma', 'board_cap_ma', 'floor_ma'):
            if name in snapshot: integer(snapshot[name], 0, 3262, name)
        if snapshot.get('source_kind') == 'usb' and snapshot.get('requested_input_ma', 0) > 500:
            rejected.append('USB2 request exceeds 500 mA; adapter classification required')
        if snapshot.get('board_cap_ma', 2100) > 2100 or snapshot.get('board_cap_ma') == 0:
            rejected.append('board cap lacks permitted p105 bound')
        # Existing Apple floor can raise an inhibited candidate. A Linux
        # prototype must not silently grant current because this field exists.
        if snapshot.get('floor_ma', 0): rejected.append('nonzero force-usb-power floor requires separate policy review')
        samples = {}
        for key in ('voltage', 'voltage_limit'):
            if key not in snapshot: continue
            value = snapshot[key]
            if not isinstance(value, dict) or set(value) != {'value', 'unit', 'valid', 'fresh'}:
                raise ValueError(f'{key} requires exactly value/unit/valid/fresh')
            integer(value['value'], 0, 0xffffffff, key)
            if type(value['valid']) is not bool or type(value['fresh']) is not bool:
                raise ValueError('sample valid/fresh must be boolean')
            if value['unit'] not in ('12H321-policy-units', 'unknown'):
                raise ValueError('physical unit conversion has not been verified')
            if not value['valid'] or not value['fresh'] or value['unit'] == 'unknown':
                rejected.append(f'{key} invalid, stale or untyped')
            else: samples[key] = value['value']
        cutoff = None
        if len(samples) == 2:
            if samples['voltage_limit'] == 0:
                rejected.append('software voltage limit absent/disabled')
            else: cutoff = samples['voltage'] >= samples['voltage_limit']
        complete = not missing and not rejected
        candidate = min(snapshot['requested_input_ma'], snapshot['board_cap_ma']) if complete and not cutoff else 0
        return {'epoch': self.epoch, 'missing_inputs': missing, 'rejected_inputs': rejected,
                'software_evidence_complete': complete, 'software_voltage_cutoff': cutoff,
                'candidate_input_ma_for_review_only': candidate,
                'blocked_hardware_proofs': list(UNPROVEN), 'input_current_ma': 0,
                'charge_current_ma': 0, 'register_operations': [],
                'hardware_access': False,
                'note': 'synthetic input validation only; no calibration, physical protection or writes'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    args = parser.parse_args()
    try:
        result = EvidenceGate().evaluate(json.loads(args.snapshot.read_text()))
        print(json.dumps({'format': 'p105-charger-evidence-gate-v1', 'result': result}, indent=2))
    except (OSError, ValueError) as error: parser.error(str(error))


if __name__ == '__main__': main()
