#!/usr/bin/env python3
"""Compile actual checked C against pinned original cancellation trace metadata.

No firmware execution, host devices, IRQs or system calls through fake callbacks.
Original instructions were executed separately with strict Unicorn Fake RAM.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import atexit

parser = argparse.ArgumentParser()
parser.add_argument('--output-dir', type=Path)
parser.add_argument('--sanitize', action=argparse.BooleanOptionalAction, default=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
root = repo/'experiments/p105-ppn'
fixture = repo/'tests/fixtures/ppn-cdma-cancel'
if args.output_dir is None:
    temp = tempfile.TemporaryDirectory(prefix='cascadia-cancel-')
    atexit.register(temp.cleanup)
    out = Path(temp.name)
else:
    out = args.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
pins = {
    'original-cancel-traces.json': 'bff3e9f3fe902bcb2b012c638daf027529becab967488fb77b97639ce5ac5182',
    'ppn_cdma_completion.h': 'a4396187cb1b326d9cf46bd2b813e8e149f3d0782dc404dab1689d5d9ceb69cc',
    'ppn_cdma_plan.h': '3a1dd654ecd6def8a0923a51263754f953c547dd0aae343b04941abc22e3eb6f',
}
for name, digest in pins.items():
    path = (fixture if name == 'original-cancel-traces.json' else root) / name
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, name
original = json.loads((fixture / 'original-cancel-traces.json').read_text())
assert original['result'] == 'PASS'
assert original['original_cancel_runs'] == 576
assert original['injected_late_irq_runs'] == 288
assert original['hardware_calls'] == 0
rows = [row for row in original['rows']
        if row['software_phase'] and row['outcome'] == 'returned']
assert len(rows) == 288
header = [
    'struct cancel_oracle { unsigned int channel, enabled, control, clear_after; };',
    'static const struct cancel_oracle cancel_oracles[] = {',
]
for row in rows:
    # This oracle is constructed from raw original instruction traces, not C output.
    writes = [event for event in row['trace'] if event[0] == 'dma-write']
    expected = []
    if not row['initial_global_enabled']:
        expected.append(['dma-write', '0x37000000', 1 << row['channel']])
    if row['control'] & 0x30000 == 0x10000:
        expected.append(['dma-write', hex(0x37000000 + (row['channel'] << 12)), 2])
    expected.append(['dma-write', '0x37000008', 1 << row['channel']])
    assert writes == expected
    callbacks = [event for event in row['injected_late_irq_trace']
                 if event[0] == 'callback']
    assert callbacks == [['callback', 0xface0055]]
    header.append('{%du,%du,%du,%du},' % (
        row['channel'], row['initial_global_enabled'], row['control'],
        row['abort_clear_after_reads'] or 0))
header.append('};')
(out / 'cancel_oracle.h').write_text('\n'.join(header) + '\n')
command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
           '-I' + str(root), '-I' + str(fixture), '-I' + str(out)]
if args.sanitize:
    command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-fno-pie', '-no-pie']
executable = out / 'cancel'
subprocess.run([*command, str(root / 'ppn_cdma_cancel.c'),
                str(fixture / 'test_cancel.c'), '-o', str(executable)], check=True)
environment = os.environ.copy()
environment['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
environment['UBSAN_OPTIONS'] = 'halt_on_error=1'
subprocess.run([str(executable)], check=True, env=environment, timeout=20)
print('PASS original-trace binding + checked C, no hardware/syscalls/IRQ/backend binding')
