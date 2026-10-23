#!/usr/bin/env python3
"""Offline design prototype for USB2 gadget charging-budget lifecycle.

Read a JSON event array from a local file and emit conservative input-current
plans. No kernel, USB, sysfs, SSH, device or register access is performed.
Permission/source-health inputs are synthetic; the missing Linux notification
and D1946 protection drivers are not implemented by this prototype.
"""
import argparse
import json
from pathlib import Path


class BudgetPlan:
    def __init__(self):
        self.epoch = 0
        self.attached = False
        self.role = 'unknown'
        self.permission = False
        self.healthy = False
        self.configured_ma = 0
        self.suspended = False

    def invalidate(self):
        self.epoch += 1
        self.permission = self.healthy = False
        self.configured_ma = 0
        self.suspended = False

    def apply(self, event):
        kind = event.get('event')
        if kind not in ('attach', 'disconnect', 'role', 'permission', 'health',
                        'configured', 'reset', 'suspend', 'resume'):
            raise ValueError('unknown event')
        if kind in ('attach', 'disconnect', 'role'):
            role = event.get('role', 'unknown')
            if role not in ('unknown', 'device', 'host'):
                raise ValueError('role must be unknown, device or host')
            self.invalidate()
            if kind == 'attach':
                self.attached = True
            elif kind == 'disconnect':
                self.attached = False
            # Role information does not establish a physically attached source.
            self.role = role if kind != 'disconnect' else 'unknown'
        else:
            if type(event.get('epoch')) is not int or event['epoch'] != self.epoch:
                return self.result('stale or missing epoch; ignored')
            if kind in ('permission', 'health'):
                if type(event.get('valid')) is not bool:
                    raise ValueError('valid must be an explicit boolean')
                if not event['valid']:
                    # Revocation cancels queued grants as well as the current
                    # budget. Recovery requires fresh inputs for the new epoch.
                    self.invalidate()
                elif kind == 'permission':
                    self.permission = event['valid']
                else:
                    self.healthy = event['valid']
            elif kind == 'configured':
                ma = event.get('mA')
                if type(ma) is not int or not 0 <= ma <= 500:
                    raise ValueError('USB2 gadget report must be an integer in 0..500 mA')
                self.configured_ma = ma
            elif kind == 'reset':
                # Invalidate delayed pre-reset enumeration notifications too.
                # Requiring fresh permission/health is deliberately conservative.
                self.invalidate()
            elif kind == 'suspend':
                self.suspended = True
            elif kind == 'resume':
                self.suspended = False
            else:
                raise ValueError('unknown event')
        return self.result()

    def result(self, note=None):
        if not self.attached:
            reason = 'disconnected'
        elif self.role != 'device':
            reason = 'role is not device; sink budget denied'
        elif not self.permission:
            reason = 'source permission unavailable or refused'
        elif not self.healthy:
            reason = 'protection status unavailable or faulted'
        elif self.suspended:
            reason = 'suspended; charging budget denied'
        elif not self.configured_ma:
            reason = 'no nonzero configuration budget'
        else:
            reason = 'synthetic inputs permit requested configuration budget'
        allowed = self.attached and self.role == 'device' and self.permission and self.healthy
        budget = self.configured_ma if allowed and not self.suspended else 0
        return {'epoch': self.epoch, 'input_budget_mA': budget,
                'reason': reason, 'note': note, 'hardware_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('events', type=Path)
    args = parser.parse_args()
    try:
        events = json.loads(args.events.read_text())
        if not isinstance(events, list) or any(not isinstance(e, dict) for e in events):
            raise ValueError('expected a JSON array of event objects')
        plan = BudgetPlan()
        print(json.dumps({'format': 'p105-usb-budget-design-v1',
                          'limitations': ['synthetic input policy; no Linux callback integration',
                                          'no adapter negotiation or PMU protection implementation'],
                          'plans': [plan.apply(e) for e in events]}, indent=2))
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
