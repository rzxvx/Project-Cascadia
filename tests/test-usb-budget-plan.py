#!/usr/bin/env python3
"""Exercise the actual offline budget planner; no firmware or hardware needed."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('budget_plan', ROOT / 'tools/power/usb-budget-plan.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class BudgetLifecycle(unittest.TestCase):
    def ready(self, ma=500):
        p = m.BudgetPlan()
        p.apply({'event': 'attach', 'role': 'device'})
        for event in ({'event': 'permission', 'valid': True},
                      {'event': 'health', 'valid': True},
                      {'event': 'configured', 'mA': ma}):
            p.apply(dict(event, epoch=p.epoch))
        return p

    def test_current_cascadia_zero_descriptor_never_grants_charge_budget(self):
        self.assertEqual(self.ready(0).result()['input_budget_mA'], 0)

    def test_configuration_requires_both_source_and_protection_knowledge(self):
        p = m.BudgetPlan()
        p.apply({'event': 'attach', 'role': 'device'})
        for e in ({'event': 'configured', 'mA': 500}, {'event': 'permission', 'valid': True}):
            self.assertEqual(p.apply(dict(e, epoch=1))['input_budget_mA'], 0)
        self.assertEqual(p.apply({'event': 'health', 'valid': True, 'epoch': 1})['input_budget_mA'], 500)

    def test_suspend_resume_and_bus_reset(self):
        p = self.ready()
        self.assertEqual(p.apply({'event': 'suspend', 'epoch': 1})['input_budget_mA'], 0)
        self.assertEqual(p.apply({'event': 'resume', 'epoch': 1})['input_budget_mA'], 500)
        self.assertEqual(p.apply({'event': 'reset', 'epoch': 1})['input_budget_mA'], 0)
        self.assertEqual(p.apply({'event': 'configured', 'mA': 500, 'epoch': 1})['input_budget_mA'], 0)
        self.assertEqual(p.apply({'event': 'resume', 'epoch': 2})['input_budget_mA'], 0)

    def test_detach_invalidates_budget_and_late_notifications(self):
        p = self.ready()
        self.assertEqual(p.apply({'event': 'disconnect'})['input_budget_mA'], 0)
        self.assertEqual(p.apply({'event': 'configured', 'mA': 500, 'epoch': 1})['input_budget_mA'], 0)
        p.apply({'event': 'attach', 'role': 'device'})
        self.assertEqual(p.apply({'event': 'health', 'valid': True, 'epoch': 1})['input_budget_mA'], 0)
        self.assertEqual(p.result()['epoch'], 3)

    def test_host_role_invalidates_sink_budget(self):
        p = self.ready()
        p.apply({'event': 'role', 'role': 'host'})
        for e in ({'event': 'permission', 'valid': True}, {'event': 'health', 'valid': True},
                  {'event': 'configured', 'mA': 500}):
            self.assertEqual(p.apply(dict(e, epoch=2))['input_budget_mA'], 0)

    def test_source_refusal_or_fault_immediately_denies_budget(self):
        for event in ('permission', 'health'):
            p = self.ready()
            self.assertEqual(p.apply({'event': event, 'valid': False, 'epoch': 1})['input_budget_mA'], 0)
            self.assertEqual(p.apply({'event': event, 'valid': True, 'epoch': 1})['input_budget_mA'], 0)
            self.assertEqual(p.result()['epoch'], 2)

    def test_role_report_does_not_create_an_attached_source(self):
        for detached_first in (False, True):
            p = m.BudgetPlan()
            if detached_first:
                p = self.ready()
                p.apply({'event': 'disconnect'})
            p.apply({'event': 'role', 'role': 'device'})
            for e in ({'event': 'permission', 'valid': True}, {'event': 'health', 'valid': True},
                      {'event': 'configured', 'mA': 500}):
                self.assertEqual(p.apply(dict(e, epoch=p.epoch))['input_budget_mA'], 0)
            self.assertFalse(p.attached)

    def test_invalid_input_cannot_be_coerced_to_permission(self):
        for value in (-1, 501, 2100, '500', True, None):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    self.ready(value)
        p = self.ready()
        for value in (1, 'true', None):
            with self.assertRaises(ValueError):
                p.apply({'event': 'permission', 'valid': value, 'epoch': 1})
        with self.assertRaises(ValueError):
            p.apply({'event': 'attach', 'role': 'adapter'})
        for event in ({}, {'event': 'invalid'}, {'event': 'invalid', 'epoch': 0}):
            with self.assertRaises(ValueError):
                p.apply(event)


if __name__ == '__main__':
    unittest.main(verbosity=2)
