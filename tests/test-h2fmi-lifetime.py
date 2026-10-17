#!/usr/bin/env python3
"""Real H2FMI/MTD functions with process-local resource lifetime controls."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

HISTORICAL_HASHES = {
    'original-mtdpart.c': '33f69ba6f9d8ff0445790b8d7cf5bf279dfe79856a0dc362bd9a8e8dd7c89a8f',
    'prior-lifetime-mtdcore.c': 'ff78cbff164d983cda7faf06f73260212a565fc6369e486fd35bbf39b5457199',
    'original-h2fmi.c': '820ce09ca39bef023ec4289f05bc2110d5e415b48e0a6f1d43b503dead83a58a',
    'original-mtdcore.c': 'a14842ae73acebc5601e74bcbde1b2dfac594979fd2512a38ea30d406b57540e',
}
CORE_FUNCTIONS = [
    'mtd_release', 'mtd_device_release', 'del_mtd_device',
    'mtd_device_unregister', '__get_mtd_device', '__put_mtd_device',
    'mtd_nvmem_reg_read', 'mtd_nvmem_add', 'add_mtd_device',
    'mtd_device_parse_register',
]
FORWARD_CORE = '''
struct mtd_part_parser_data;
struct mtd_partition;
static int mtd_nvmem_add(struct mtd_info *);
int del_mtd_device(struct mtd_info *);
int mtd_device_parse_register(struct mtd_info *, const char *const *,
                             struct mtd_part_parser_data *,
                             const struct mtd_partition *, int);
int __get_mtd_device(struct mtd_info *);
void __put_mtd_device(struct mtd_info *);
int mtd_device_unregister(struct mtd_info *);
static void mtd_device_release(struct kref *);
static void mtd_release(struct device *);
static int mtd_device_register(struct mtd_info *, const struct mtd_partition *, int);
'''
REGISTER_WRAPPER = '''
static int mtd_device_register(struct mtd_info *m, const struct mtd_partition *p, int n)
{
    return mtd_device_parse_register(m, NULL, NULL, p, n);
}
'''


def function(source, name):
    pattern = (r'\n(?:static[^\n]*\n)?(?:static )?'
               r'(?:int|void|size_t|ssize_t)[^\n]*\b' + re.escape(name) + r'\(')
    match = re.search(pattern, source)
    if not match:
        raise ValueError('actual function missing: ' + name)
    brace = source.index('{', match.start())
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if not depth:
            return source[match.start() + 1:end + 1]
    raise ValueError('unterminated function: ' + name)


def fixture(template, driver, core, historical, part=None):
    geometry = driver[driver.index('#define H2FMI_BUSES'):driver.index('/* The ADT')]
    registers = driver[driver.index('#define H2FMI_CE'):driver.index('/* The allowlist is')]
    structures = driver[driver.index('struct h2fmi_bus {'):driver.index('/*\n * h2fmi_wait')]
    source = template.replace('/* GEOMETRY */', geometry + registers)
    source = source.replace('/* DRIVER_STRUCTS */', structures.replace('__iomem', ''))
    bodies = [function(core, name) for name in (CORE_FUNCTIONS if historical else ['mtd_release_final','mtd_release_work',*CORE_FUNCTIONS])]
    if not historical:
        if part is None:
            raise ValueError('candidate partition source required')
        bodies.insert(0,function(part,'put_partition_constructor'))
    names = ['h2fmi_split_page', 'h2fmi_read_page_at',
             'h2fmi_bitflips', 'h2fmi_account_ecc']
    if not historical:
        names += ['h2fmi_mtd_read_oob_locked', 'h2fmi_mtd_block_isbad_locked',
                  'h2fmi_release', 'h2fmi_get_device', 'h2fmi_put_device']
    names += ['h2fmi_mtd_read_oob', 'h2fmi_mtd_block_isbad']
    if not historical:
        names += ['h2fmi_unregister']
    names += ['h2fmi_setup_mtd', 'h2fmi_probe', 'h2fmi_remove']
    bodies.extend(function(driver, name) for name in names)
    source = source.replace('/* FORWARD_CORE */', FORWARD_CORE)
    return source.replace('/* ACTUAL_FUNCTIONS */', '\n\n'.join(bodies) + REGISTER_WRAPPER)


def main():
    group = Path(__file__).resolve().parent
    default_repo = group.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', '--root', type=Path, default=default_repo)
    parser.add_argument('--driver', type=Path)
    parser.add_argument('--core', type=Path)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    fixtures = args.fixtures or group / 'fixtures'
    if not (fixtures / 'original-h2fmi.c').exists():
        fixtures = group / 'fixtures/h2fmi-lifetime'
    for name, expected in HISTORICAL_HASHES.items():
        actual = hashlib.sha256((fixtures / name).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError('historical actual-source hash mismatch: ' + name)
    experiment = args.repo / 'experiments/p105-h2fmi-lifetime'
    driver = args.driver or experiment / 'mtd-apple-h2fmi.c'
    if args.core:
        core_source = args.core.read_text()
    else:
        # Reconstruct the proposed core/header in a fresh disposable tree.
        expected_header = '7ffee580bb12556e4074f07e6fdf13d56d9a5ca18d2eb69ebb4d3c0c9896badb'
        if hashlib.sha256((fixtures / 'original-mtd.h').read_bytes()).hexdigest() != expected_header:
            raise ValueError('historical MTD header hash mismatch')
        with tempfile.TemporaryDirectory(prefix='h2fmi-core-reconstruction-') as temp:
            temp = Path(temp)
            for name, baseline in [('drivers/mtd/mtdcore.c', 'original-mtdcore.c'),
                                   ('include/linux/mtd/mtd.h', 'original-mtd.h'),
                                   ('drivers/mtd/mtdpart.c','original-mtdpart.c')]:
                destination = temp / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes((fixtures / baseline).read_bytes())
            subprocess.run(['git', 'apply', '--whitespace=error',
                            str((experiment / 'mtd-context-lifetime.patch').resolve())],
                           cwd=temp, check=True)
            core_source = (temp / 'drivers/mtd/mtdcore.c').read_text()
            if (temp / 'drivers/mtd/mtdpart.c').read_bytes() != (experiment / 'mtdpart.c').read_bytes():
                raise ValueError('reconstructed partition differs from exported candidate')

    template = (fixtures / 'fixture.c.in').read_text()
    sources = [
        ('old', (fixtures / 'original-h2fmi.c').read_text(),
         (fixtures / 'original-mtdcore.c').read_text(), True),
        ('candidate', driver.read_text(), core_source, False),
    ]
    flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-Wno-unused-parameter', '-Wno-sign-compare', '-O1', '-g', '-pthread',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
    env = os.environ.copy()
    env.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='h2fmi-lifetime-', dir=args.output_dir) as temp:
        out = Path(temp)
        for label, driver_source, core_source, historical in sources:
            src = out / (label + '.c')
            src.write_text(fixture(template, driver_source, core_source, historical, (experiment / 'mtdpart.c').read_text()))
            controls = [(0, []), (1, [])]
            if not historical:
                controls += [(0, ['-DCONTRACT_REJECTION', '-DCORE_BUILTIN=0']),
                             (0, ['-DCONTRACT_REJECTION', '-DCORE_BUILTIN=1']),
                             (0, ['-DMODULE_PROBE_CONTROL', '-DMODULE'])]
            for serial, (partitioned, control) in enumerate(controls):
                exe = out / (label + str(serial))
                options = ['-DHISTORICAL_CONTROL'] if historical else []
                subprocess.run([args.cc, *flags, *options, *control,
                                '-DPARTITIONED_MASTER=' + str(partitioned),
                                str(src), '-o', str(exe)], check=True, timeout=60)
                subprocess.run([str(exe)], check=True, timeout=20, env=env)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
