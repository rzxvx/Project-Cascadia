#!/usr/bin/env python3
"""Optional private-tree install only; never invokes a build or device operation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile

KERNEL_PIN = 'adc218676eef25575469234709c2d87185ca223a'
PURPOSE = 'offline-h2fmi-lifetime-complete-link'
ACK = '.cascadia-lifetime-tree.json'
RECEIPT = '.cascadia-lifetime-install.json'
PROPOSAL_HASHES = {
    'source-provenance.json': '9234adbe00ce559d6e58b51b6e1e90261b24f019ccc40ef2aec5f7ea66cb0494',
    'mtd-context-lifetime.patch': 'df1ed9aaedc712a82f98f02876575372d41946a659fb0bf8f1807c34981b8041',
    'mtd-apple-h2fmi.c': 'cab5b9a796d0d8e67f6484c6e51ba0839eb5a69f313257ba06ec74c536eb06fa',
}
BASELINE = {
    'drivers/mtd/devices/mtd-apple-h2fmi.c': '820ce09ca39bef023ec4289f05bc2110d5e415b48e0a6f1d43b503dead83a58a',
    'drivers/mtd/mtdcore.c': 'a14842ae73acebc5601e74bcbde1b2dfac594979fd2512a38ea30d406b57540e',
    'include/linux/mtd/mtd.h': '7ffee580bb12556e4074f07e6fdf13d56d9a5ca18d2eb69ebb4d3c0c9896badb',
    'drivers/mtd/mtdpart.c': '33f69ba6f9d8ff0445790b8d7cf5bf279dfe79856a0dc362bd9a8e8dd7c89a8f',
}
INSTALLED = {
    'drivers/mtd/devices/mtd-apple-h2fmi.c': PROPOSAL_HASHES['mtd-apple-h2fmi.c'],
    'drivers/mtd/mtdcore.c': 'fc6b3a8056d16832e014fe4cffdb4bc091afde5948ba7a469e371686450590d4',
    'include/linux/mtd/mtd.h': '2ced38a2a796b13a5bfcc0492972683d8dddc4c9fecbde501baba715ee7d3aa9',
    'drivers/mtd/mtdpart.c': '8adbf30aea21f169a18dffb32a9f0d90236dd302d7d92dea903c363e8431fb5e',
}
MAKEFILE_HASH = '96ec6a92139d7af01fa3c4be4f8f521f86f48d249861c578dc00f4ff38df1b61'
REQUIRED = ('CONFIG_ARM', 'CONFIG_ARCH_APPLE_S5L', 'CONFIG_MTD', 'CONFIG_MTD_APPLE_H2FMI')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def no_symlinks(path):
    if not path.is_absolute() or '..' in path.parts:
        raise ValueError('absolute path without .. required: ' + str(path))
    for parent in reversed([path, *path.parents]):
        if parent.is_symlink():
            raise ValueError('symlink path refused: ' + str(parent))


def checked_file(path, private=False):
    no_symlinks(path)
    st = path.stat(follow_symlinks=False)
    if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_nlink != 1:
        raise ValueError('caller-owned regular single-link file required: ' + str(path))
    if private and st.st_mode & 0o077:
        raise ValueError('private file permissions required: ' + str(path))
    return path.read_bytes()


def exact_json(data):
    def pairs(items):
        obj = {}
        for key, value in items:
            if key in obj:
                raise ValueError('duplicate JSON key: ' + key)
            obj[key] = value
        return obj
    return json.loads(data, object_pairs_hook=pairs)


def acknowledgement(tree):
    return {'schema': 1, 'purpose': PURPOSE, 'kernel_pin': KERNEL_PIN,
            'tree': str(tree), 'owner_uid': os.getuid()}


def config_check(tree):
    data = checked_file(tree / '.config').decode('utf-8')
    generated = checked_file(tree / 'include/generated/autoconf.h').decode('utf-8')
    values = {}
    for line in data.splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key, value = line.split('=', 1)
            if key in values:
                raise ValueError('duplicate config symbol: ' + key)
            values[key] = value
    for key in REQUIRED:
        if values.get(key) != 'y' or generated.splitlines().count('#define ' + key + ' 1') != 1:
            raise ValueError('prepared built-in configuration required: ' + key)
        if '#define ' + key + '_MODULE 1' in generated.splitlines():
            raise ValueError('modular generated configuration refused: ' + key)
    if values.get('CONFIG_MTD_PARTITIONED_MASTER', 'n') not in ('n', 'y'):
        raise ValueError('invalid partitioned-master configuration')
    return {'config_sha256': sha(data.encode()), 'autoconf_sha256': sha(generated.encode())}


def local_git_environment():
    env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
    env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null', GIT_OPTIONAL_LOCKS='0')
    return env


def verify_repository_binding(proposal, protected_repo):
    # The supplied repository must be the real local checkout containing this
    # exact published proposal, not a lexical exclusion path or a typo.
    no_symlinks(protected_repo)
    no_symlinks(proposal)
    if not protected_repo.is_dir() or not proposal.is_dir():
        raise ValueError('existing protected repository/proposal required')
    if proposal != protected_repo / 'experiments/p105-h2fmi-lifetime':
        raise ValueError('proposal must belong to the actual protected repository')
    # Ignore ambient Git directory/worktree/config overrides. rev-parse is a
    # local discovery command, not a hook, fetch or checkout operation.
    env = local_git_environment()
    result = subprocess.run(['/usr/bin/git', '--no-optional-locks', '-c',
                             'core.fsmonitor=false', '-c', 'protocol.allow=never',
                             '-C', str(protected_repo), 'rev-parse', '--show-toplevel'],
                            check=True, capture_output=True, text=True, timeout=10, env=env)
    if Path(result.stdout.strip()) != protected_repo:
        raise ValueError('protected path must be the actual checkout root')


def inspect(tree, proposal, protected_repo):
    no_symlinks(tree)
    verify_repository_binding(proposal, protected_repo)
    if tree.is_relative_to(protected_repo) or protected_repo.is_relative_to(tree):
        raise ValueError('normal repository tree/ancestor refused')
    st = tree.stat(follow_symlinks=False)
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077:
        raise ValueError('caller-owned private directory (0700) required')
    ack = exact_json(checked_file(tree / ACK, private=True))
    if ack != acknowledgement(tree) or type(ack.get('schema')) is not int or type(ack.get('owner_uid')) is not int:
        raise ValueError('explicit exact private-tree acknowledgement required')
    for name, digest in PROPOSAL_HASHES.items():
        if sha(checked_file(proposal / name)) != digest:
            raise ValueError('frozen proposal hash mismatch: ' + name)
    provenance = exact_json((proposal / 'source-provenance.json').read_bytes())
    if provenance.get('kernel_pin') != KERNEL_PIN or provenance.get('normal_image_activation') is not False:
        raise ValueError('unexpected proposal provenance')
    if sha(checked_file(tree / 'Makefile')) != MAKEFILE_HASH:
        raise ValueError('pinned Linux6.12 Makefile mismatch')
    # Read-only local commit identity, no hooks, fetch, maintenance or lock writes.
    no_symlinks(tree / '.git')
    if not (tree / '.git').is_dir():
        raise ValueError('prepared local Git tree required')
    checked_file(tree / '.git/HEAD')
    pinned = subprocess.run(['/usr/bin/git', '--no-optional-locks', '-c',
                             'core.fsmonitor=false', '-c', 'protocol.allow=never',
                             '--git-dir', str(tree / '.git'), '--work-tree', str(tree),
                             'rev-parse', '--verify', 'HEAD^{commit}'],
                            check=True, capture_output=True, text=True, timeout=10, env=local_git_environment())
    if pinned.stdout.strip() != KERNEL_PIN:
        raise ValueError('local kernel commit pin mismatch')
    config = config_check(tree)
    files = {name: checked_file(tree / name) for name in BASELINE}
    hashes = {name: sha(data) for name, data in files.items()}
    state = 'baseline' if hashes == BASELINE else 'installed' if hashes == INSTALLED else 'mixed-or-unknown'
    receipt = None
    if (tree / RECEIPT).exists() or (tree / RECEIPT).is_symlink():
        receipt = exact_json(checked_file(tree / RECEIPT, private=True))
        if (receipt.get('schema') != 1 or receipt.get('state') != 'complete' or
                receipt.get('tree') != str(tree) or receipt.get('hashes') != INSTALLED or
                receipt.get('kernel_pin') != KERNEL_PIN):
            raise ValueError('interrupted/foreign install receipt; no automatic recovery')
    if state == 'mixed-or-unknown' or (state == 'baseline' and receipt):
        raise ValueError('mixed/unknown source state; no files modified')
    if state == 'installed' and not receipt:
        raise ValueError('installed sources lack owned complete receipt')
    return state, files, config


def json_bytes(value):
    return (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()


def write_exclusive(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as out:
        out.write(data)
        out.flush()
        os.fsync(out.fileno())


def apply(tree, proposal, files, config):
    # Only the reviewed exact two-path patch is accepted; no fetched script.
    with tempfile.TemporaryDirectory(prefix='lifetime-stage-', dir=tree) as name:
        staging = Path(name)
        for relative, data in files.items():
            path = staging / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        subprocess.run(['/usr/bin/patch', '--batch', '--forward', '--fuzz=0', '-p1',
                        '-d', str(staging), '-i', str(proposal / 'mtd-context-lifetime.patch')],
                       check=True, timeout=20)
        (staging / 'drivers/mtd/devices/mtd-apple-h2fmi.c').write_bytes(
            (proposal / 'mtd-apple-h2fmi.c').read_bytes())
        final = {relative: (staging / relative).read_bytes() for relative in files}
        if {relative: sha(data) for relative, data in final.items()} != INSTALLED:
            raise ValueError('reconstructed source hashes do not match frozen proposal')
        # Caller guarantees serialized use. Recheck all inputs before any mutation.
        for relative, original in files.items():
            if checked_file(tree / relative) != original:
                raise ValueError('source changed during preparation')
        record = {'schema': 1, 'tree': str(tree), 'kernel_pin': KERNEL_PIN,
                  'state': 'in-progress', 'hashes': INSTALLED, **config}
        write_exclusive(tree / RECEIPT, json_bytes(record))
        for index, (relative, data) in enumerate(final.items()):
            destination = tree / relative
            temp = destination.parent / ('.lifetime-' + str(os.getpid()) + '-' + str(index))
            write_exclusive(temp, data)
            os.replace(temp, destination)
        record['state'] = 'complete'
        temp = tree / ('.lifetime-receipt-' + str(os.getpid()))
        write_exclusive(temp, json_bytes(record))
        os.replace(temp, tree / RECEIPT)
    # Multi-file install is not crash-atomic. Interrupted/mixed state stays denied.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--proposal', type=Path, required=True)
    parser.add_argument('--protected-repo', type=Path, required=True)
    parser.add_argument('--apply', action='store_true', help='modify only acknowledged private tree')
    args = parser.parse_args()
    state, files, config = inspect(args.tree, args.proposal, args.protected_repo)
    if args.apply and state == 'baseline':
        apply(args.tree, args.proposal, files, config)
        state = 'installed'
    print(json.dumps({'source_state': state, 'applied': args.apply, 'build_executed': False,
                      'device_access': False, 'normal_image_activation': False, **config}))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit('refused: ' + str(error))
