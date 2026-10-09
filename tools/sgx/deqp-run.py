#!/usr/bin/env python3
"""deqp-run.py -- dEQP-GLES2 on the iPad through the SGX driver (docs/
research/p105-mesa.md, M24): the cases under each prefix given, a process
at a time; a process that dies or goes quiet for a minute leaves its
current case a Crash or a Timeout, and the next one goes on from the case
after.  Results, a line a case ("case status detail"), into OUT/<prefix>.txt
and a summary to stdout.

    python3 deqp-run.py DEQPDIR OUT PREFIX...     (on the device)

DEQPDIR holds deqp-gles2, its gles2/ data and cases.txt (a case a line:
deqp-gles2 --deqp-runmode=stdout-caselist).
"""
import os, re, select, signal, subprocess, sys, time

QUIET = 60      # seconds without output: a hang
ARGS = ['--deqp-surface-type=pbuffer', '--deqp-gl-config-name=rgba8888d24s8ms0',
        '--deqp-surface-width=256', '--deqp-surface-height=256', '--deqp-log-images=disable',
        '--deqp-log-shader-sources=disable', '--deqp-log-filename=/tmp/deqp.qpa']
STATUS = re.compile(r'^  (Pass|Fail|NotSupported|QualityWarning|CompatibilityWarning|'
                    r'InternalError|ResourceError|Crash|Timeout) \((.*)\)$')


def run(deqp, cases, log):
    """the cases, as far as one process gets: {case: (status, detail)}"""
    with open('/tmp/deqp-caselist.txt', 'w') as f:
        f.write('\n'.join(cases) + '\n')
    env = dict(os.environ, EGL_PLATFORM='surfaceless')
    p = subprocess.Popen(['sgx-gl', os.path.join(deqp, 'deqp-gles2'),
                          '--deqp-caselist-file=/tmp/deqp-caselist.txt'] + ARGS,
                         cwd=deqp, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         bufsize=0)
    done, current, buf = {}, None, b''
    while True:
        r, _, _ = select.select([p.stdout], [], [], QUIET)
        if not r:
            p.kill()
            if current:
                done[current] = ('Timeout', 'no output for %d s' % QUIET)
            break
        chunk = os.read(p.stdout.fileno(), 65536)
        if not chunk:
            break
        buf += chunk
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            line = line.decode(errors='replace')
            log.write(line + '\n')
            m = re.match(r"^Test case '(.*)'\.\.$", line)
            if m:
                current = m.group(1)
                continue
            m = STATUS.match(line)
            if m and current:
                done[current] = (m.group(1), m.group(2))
                current = None
    p.wait()
    if current and current not in done:
        done[current] = ('Crash', 'the process ended (%d)' % p.returncode)
    return done


def main():
    deqp, out, prefixes = sys.argv[1], sys.argv[2], sys.argv[3:]
    every = [l.strip() for l in open(os.path.join(deqp, 'cases.txt')) if l.strip()]
    os.makedirs(out, exist_ok=True)
    for prefix in prefixes:
        todo = [c for c in every if c.startswith(prefix)]
        results, t0 = {}, time.time()
        with open(os.path.join(out, prefix + '.log'), 'w') as log:
            while todo:
                done = run(deqp, todo, log)
                if not done:            # nothing ran: the first case, a crash
                    done = {todo[0]: ('Crash', 'nothing ran')}
                results.update(done)
                todo = [c for c in todo if c not in results]
        with open(os.path.join(out, prefix + '.txt'), 'w') as f:
            for c in sorted(results):
                f.write('%s %s %s\n' % (c, results[c][0], results[c][1]))
        count = {}
        for s, _ in results.values():
            count[s] = count.get(s, 0) + 1
        print('%-50s %5d cases, %4.0f s: %s' % (prefix, len(results), time.time() - t0,
              ', '.join('%s %d' % kv for kv in sorted(count.items()))), flush=True)


if __name__ == '__main__':
    main()
