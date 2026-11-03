#!/usr/bin/env python3
"""Exercise actual startup failure branch with fake boundary; no X/device access."""
from pathlib import Path
import subprocess

source=(Path(__file__).resolve().parents[1]/'rootfs/k93/browser-test.sh').read_text()
assert 'MOZ_AVOID_OPENGL_ALTOGETHER=1' in source
assert 'MOZ_DISABLE_CONTENT_SANDBOX' not in source
assert '--no-sandbox' not in source
snapshot=source[source.index('startup_snapshot() {'):source.index('\ncleanup() {')]
assert snapshot.index('K93_BROWSER_WINDOW_SNAPSHOT_UPTIME')<snapshot.index('for status in /proc/')
assert 'for visibility in visible all' in snapshot
assert '[ "$task_count" -gt 8 ]' in snapshot
assert '[ "$traced" = 0 ]' in snapshot
start=source.index('if window=$(busybox timeout -k 1 100 ')
end=source.index('\ntest "$(printf',start)
branch=source[start:end]
for status in (0,143):
    fixture=f'''set -eu
busybox() {{ printf '77'; return {status}; }}
startup_snapshot() {{ echo SNAPSHOT_CALLED; }}
'''+branch+'\necho "WINDOW=$window"\n'
    result=subprocess.run(['sh','-s'],input=fixture,text=True,capture_output=True)
    assert result.returncode==status,(status,result)
    assert ('SNAPSHOT_CALLED' in result.stdout)==(status!=0)
    assert ('WINDOW=77' in result.stdout)==(status==0)
print('PASS: actual startup branch captures failure before exit, preserves timeout status and does not snapshot success; no hardware')
finish=source[source.index('finish() {'):source.index('\necho K93_DESKTOP_BEGIN')]
for status in (0,1,143):
    for already in (0,1):
        fixture=f'''set -eu
failure_snapshot_done={already}
startup_snapshot() {{ echo SNAPSHOT_CALLED; failure_snapshot_done=1; }}
cleanup() {{ echo CLEANUP_CALLED; }}
'''+finish+f'\nexit {status}\n'
        result=subprocess.run(['sh','-s'],input=fixture,text=True,capture_output=True,timeout=3)
        assert result.returncode==status,(status,already,result)
        assert result.stdout.count('SNAPSHOT_CALLED')==(status!=0 and not already)
        assert result.stdout.count('CLEANUP_CALLED')==1
print('PASS: actual exit trap snapshots later failures once, preserves status and always cleans up; no hardware')
