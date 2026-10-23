#!/usr/bin/env python3
"""Real Git/CLI containment of optional source installer; no kernel/build inputs."""
import argparse
import importlib.util
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile


def clean_git_environment():
    env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
    env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null', GIT_OPTIONAL_LOCKS='0')
    return env


def checkout(path):
    path.mkdir(mode=0o700)
    result = subprocess.run(['/usr/bin/git', '-c', 'core.fsmonitor=false', '-c',
                             'protocol.allow=never', 'init', '--quiet', str(path)],
                            env=clean_git_environment(), capture_output=True, text=True, timeout=10)
    if result.returncode:
        raise AssertionError('local Git fixture failed: ' + result.stderr)
    proposal = path / 'experiments/p105-h2fmi-lifetime'
    proposal.mkdir(parents=True)
    # No proposal/core/header bodies are needed: these boundary checks must run
    # before source hashes or an install write. No commit/private object is made.
    (path / 'protected-sentinel.txt').write_text('fixture protected content\n')
    return proposal


def snapshot(root):
    values = {}
    for path in [root, *sorted(root.rglob('*'))]:
        mode = path.lstat().st_mode
        kind = stat.S_IFMT(mode)
        value = str(path.readlink()) if path.is_symlink() else path.read_bytes() if stat.S_ISREG(mode) else None
        values[str(path.relative_to(root))] = (kind, stat.S_IMODE(mode), value)
    return values


def refuse(helper, fixture, tree, proposal, protected, diagnostic, extra_env=None):
    before = snapshot(fixture)
    env = clean_git_environment()
    env.update(extra_env or {})
    result = subprocess.run([sys.executable, str(helper), '--tree', str(tree),
                             '--proposal', str(proposal), '--protected-repo', str(protected), '--apply'],
                            env=env, capture_output=True, text=True, timeout=15)
    assert result.returncode != 0, result.stdout
    assert 'refused:' in result.stderr and diagnostic in result.stderr, result.stderr
    assert snapshot(fixture) == before, 'CLI modified fixture before boundary refusal'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper', type=Path, default=Path(__file__).resolve().parents[1] /
                        'experiments/p105-h2fmi-lifetime/prepare-lifetime.py')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    helper = args.helper.resolve(strict=True)
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('h2fmi_private_application', helper)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    cases = 0
    with tempfile.TemporaryDirectory(prefix='h2fmi-private-boundary-', dir=args.output_dir) as name:
        fixture = Path(name).resolve()
        project = fixture / 'project'
        proposal = checkout(project)
        other = fixture / 'unrelated-project'
        checkout(other)
        private = fixture / 'caller-private-linux'
        private.mkdir(mode=0o700)
        normal = project / 'caller-mistaken-normal-target'
        normal.mkdir(mode=0o700, parents=True)
        (normal / 'normal-sentinel.c').write_text('fixture original source\n')
        before = snapshot(fixture)
        module.verify_repository_binding(proposal, project)
        assert snapshot(fixture) == before
        cases += 1
        # Environment pollution must not change actual checkout discovery.
        pollution = {'GIT_DIR': str(other / '.git'), 'GIT_WORK_TREE': str(other),
                     'GIT_CONFIG_COUNT': '1', 'GIT_CONFIG_KEY_0': 'core.worktree',
                     'GIT_CONFIG_VALUE_0': str(other)}
        saved = {key: os.environ.get(key) for key in pollution}
        try:
            os.environ.update(pollution)
            module.verify_repository_binding(proposal, project)
        finally:
            for key, value in saved.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
        assert snapshot(fixture) == before
        cases += 1
        refuse(helper, fixture, normal, proposal, project, 'normal repository tree')
        refuse(helper, fixture, fixture, proposal, project, 'normal repository tree')
        refuse(helper, fixture, normal, proposal, project, 'normal repository tree', pollution)
        cases += 3
        refuse(helper, fixture, private, proposal, fixture / 'missing-project',
               'existing protected repository')
        refuse(helper, fixture, private, proposal, other, 'actual protected repository')
        cases += 2
        nested = project / 'nested-path'
        nested.mkdir()
        nested_proposal = nested / 'experiments/p105-h2fmi-lifetime'
        nested_proposal.mkdir(parents=True)
        refuse(helper, fixture, private, nested_proposal, nested, 'actual checkout root')
        cases += 1
        proposal_alias = fixture / 'proposal-alias'
        proposal_alias.symlink_to(proposal, target_is_directory=True)
        refuse(helper, fixture, private, proposal_alias, project, 'symlink path refused')
        cases += 1
    print('PASS', cases, 'public-helper actual Git/CLI boundary controls; kernel/build/device inputs=0')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
