#!/usr/bin/env python3
"""Actual wrapper functions with local fake controls; no mounts/signals/devices."""
from pathlib import Path
import subprocess
import tempfile
import os
root = Path(__file__).resolve().parents[1]
source = (root / 'initramfs/usr/bin/ios').read_text().split('case "$1" in')[0]
# A stale global capacity was the old wrapper's incorrect readiness predicate.
assert 'while ! /sbin/iosnandctl ready "$RUN/pid"' in source
assert 'kill ' not in source and 'echo $!' not in source
cases = [
    ('own-ready', True, 3),
    ('foreign-positive-capacity', False, 0),
    ('stale-generation-ready', False, 0),
    ('dead-worker', False, 0),
    ('reused-worker', False, 0),
]
with tempfile.TemporaryDirectory(prefix='cascadia-ios-wrapper-test-') as tmp:
    work = Path(tmp)
    helper = work / 'fake-control'
    helper.write_text('''#!/bin/sh
printf '%s\\n' "$1" >> "$TRACE"
case "$1" in
check-directory|clean) exit 0 ;;
status) exit 0 ;;
ready)
    n=$(cat "$COUNT"); n=$((n+1)); printf '%s' "$n" > "$COUNT"
    [ "$SCENARIO" = own-ready ] && [ "$n" -ge 3 ] && exit 0
    exit 1 ;;
*) exit 9 ;;
esac
''')
    helper.chmod(0o700)
    transformed = source.replace('/sbin/iosnandctl', str(helper)).replace('/sbin/iosnand --pid-record', 'fake_daemon --pid-record')
    transformed = transformed.replace('/sys/block/', str(work / 'sys') + '/')
    transformed = transformed.replace('[ -b /dev/nbd1 ]', 'false')
    # Run the inert fake daemon synchronously; no OS daemon/fork lifecycle.
    transformed = transformed.replace('> "$LOG" 2>&1 &', '> "$LOG" 2>&1')
    for name, success, ready_count in cases:
        trace = work / 'trace'; trace.write_text('')
        count = work / 'count'; count.write_text('0')
        log = work / 'iosnand.log'; log.write_text('')
        run = work / 'run'; run.mkdir(mode=0o700, exist_ok=True)
        # Existing kernel size stays positive in every scenario. It must not
        # authorize mount before the generation/worker check says ready.
        size = work / 'sys/nbd0/size'; size.parent.mkdir(parents=True, exist_ok=True); size.write_text('99999\n')
        script = transformed + f'''\nRUN='{run}'; LOG='{log}'; MNT='{work / 'mnt'}'
need() {{ :; }}
scan_notice() {{ :; }}
fake_daemon() {{ printf 'daemon\\n' >> "$TRACE"; }}
mount() {{ printf 'MOUNT\\n' >> "$TRACE"; }}
sleep() {{ :; }}
serving() {{ grep -q daemon "$TRACE" && [ "$(cat "$COUNT")" -lt 4 ]; }}
start
'''
        env = dict(os.environ, TRACE=str(trace), COUNT=str(count), SCENARIO=name)
        result = subprocess.run(['sh', '-c', script], env=env, capture_output=True, text=True, timeout=10)
        events = trace.read_text().splitlines()
        if success:
            assert result.returncode == 0, result.stderr
            assert events.count('MOUNT') == 1 and events[:events.index('MOUNT')].count('ready') == ready_count, events
        else:
            assert result.returncode != 0 and 'MOUNT' not in events, (name, events, result.stderr)
    helper.write_text('''#!/bin/sh
printf '%s\\n' "$1" >> "$TRACE"
case "$1" in
status)
    case "$SCENARIO" in
    reused-PID|untrusted-record) exit 1 ;;
    stopping) [ -s "$COUNT" ] && [ "$(cat "$COUNT")" = stopped ] && exit 1 ;;
    esac
    exit 0 ;;
stop)
    [ "$SCENARIO" = stop-refused ] && exit 1
    [ "$SCENARIO" = stopping ] && printf stopped > "$COUNT"
    exit 0 ;;
clean)
    [ "$SCENARIO" = untrusted-record ] && exit 1
    rm -- "$2"; exit $? ;;
*) exit 9 ;;
esac
''')
    stop_cases = [('stopping', True, True), ('reused-PID', True, False),
                  ('untrusted-record', False, False), ('stop-refused', False, True),
                  ('slow-exit', False, True)]
    for name, success, stop_expected in stop_cases:
        trace.write_text(''); count.write_text('')
        record = run / 'pid'; record.write_text('fixture record\n')
        script = transformed + f"\nRUN='{run}'\nsleep() {{ :; }}\nstop_serving\n"
        env = dict(os.environ, TRACE=str(trace), COUNT=str(count), SCENARIO=name)
        result = subprocess.run(['sh', '-c', script], env=env, capture_output=True, text=True, timeout=10)
        events = trace.read_text().splitlines()
        assert (result.returncode == 0) == success, (name, events, result.stderr)
        assert ('stop' in events) == stop_expected, (name, events)
        assert record.exists() != success, (name, events)
        if not success:
            assert ('clean' in events) == (name == 'untrusted-record'), (name, events)
print(f'ios wrapper: {len(cases) + len(stop_cases)} offline fake-control ready/stop cases PASS')
