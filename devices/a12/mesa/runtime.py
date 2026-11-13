#!/usr/bin/env python3
"""Stage/check the shared A12 Mesa runtime without publishing host libraries."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile

PREFIX = '/opt/cascadia/mesa'

def digest(p):
    h = hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''): h.update(b)
    return h.hexdigest()

def manifest(stage, revision, patch_sha):
    lib = stage/'usr/lib'
    required = ['libEGL_mesa.so.0', 'libgbm.so.1', 'gbm/dri_gbm.so']
    if any(not (lib/n).is_file() for n in required) or not list(lib.glob('libgallium-*.so')):
        raise ValueError('Mesa stage is incomplete')
    files = {str(p.relative_to(stage)): digest(p) for p in stage.rglob('*') if p.is_file() and p.name != 'cascadia-mesa.json'}
    (stage/'cascadia-mesa.json').write_text(json.dumps(dict(
        mesa_revision=revision, patch_sha256=patch_sha, files=files), indent=2)+'\n')

def install(stage, root, dependency_dir=None):
    root = root.resolve()
    if str(root) == '/' or not (root/'etc/os-release').is_file():
        raise ValueError('provide an existing target rootfs, never the host /')
    library_path = PREFIX+'/lib'
    if dependency_dir:
        if not re.fullmatch(r'/[A-Za-z0-9_./-]+', dependency_dir) or '..' in Path(dependency_dir).parts:
            raise ValueError('invalid target dependency directory')
        if not (root/dependency_dir.lstrip('/')).is_dir():
            raise ValueError('target dependency directory missing')
        library_path += ':'+dependency_dir
    meta = json.loads((stage/'cascadia-mesa.json').read_text())
    for name, expected in meta['files'].items():
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts or digest(stage/relative) != expected:
            raise ValueError('invalid or changed Mesa stage: '+name)
    out = root/PREFIX.lstrip('/')
    if out.is_symlink() or (out.exists() and not (out/'cascadia-mesa.json').is_file()):
        raise ValueError('refusing an unrecognized runtime directory')
    parent = out.parent
    parent.mkdir(parents=True, exist_ok=True)
    if not parent.resolve().is_relative_to(root):
        raise ValueError('runtime parent escapes target root')
    bindir = root/'usr/local/bin'
    bindir.mkdir(parents=True, exist_ok=True)
    if not bindir.resolve().is_relative_to(root):
        raise ValueError('wrapper directory escapes target root')
    candidate = Path(tempfile.mkdtemp(prefix='.mesa-candidate-', dir=parent))
    wrapper_tmp = None
    backup = None
    activated = False
    try:
        shutil.copytree(stage/'usr/lib', candidate/'lib', symlinks=True)
        shutil.copyfile(stage/'cascadia-mesa.json', candidate/'cascadia-mesa.json')
        candidate_target = '/' + str(candidate.relative_to(root))
        checked_path = candidate_target+'/lib'
        if dependency_dir: checked_path += ':'+dependency_dir
        libs = [candidate/'lib/libEGL_mesa.so.0', candidate/'lib/libgbm.so.1',
                *list((candidate/'lib').glob('libgallium-*.so'))]
        for lib in libs:
            target = '/' + str(lib.relative_to(root))
            result = subprocess.run(['chroot', str(root), '/usr/bin/env',
                'LD_LIBRARY_PATH='+checked_path, '/usr/bin/ldd', '-r', target],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            if result.returncode or any(word in result.stdout for word in ('not found', 'undefined symbol')):
                raise ValueError('target Mesa dependencies incompatible: '+target+'\n'+result.stdout)
        (candidate/'egl_vendor.json').write_text(json.dumps({'file_format_version':'1.0.0',
            'ICD':{'library_path':PREFIX+'/lib/libEGL_mesa.so.0'}}, indent=2)+'\n')
        fd, name = tempfile.mkstemp(prefix='.cascadia-gpu-', dir=bindir)
        os.close(fd)
        wrapper_tmp = Path(name)
        wrapper_tmp.write_text('#!/bin/sh\n# Explicit per-session selection; distro Mesa remains available.\n'
            'export LD_LIBRARY_PATH="'+library_path+'${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
            'export GBM_BACKENDS_PATH="'+PREFIX+'/lib/gbm"\n'
            'export __EGL_VENDOR_LIBRARY_FILENAMES="'+PREFIX+'/egl_vendor.json"\n'
            'unset LIBGL_ALWAYS_SOFTWARE\nexec "$@"\n')
        wrapper_tmp.chmod(0o755)
        # Validate the entire candidate before replacing a working installation.
        # Keep the previous tree until both runtime and wrapper are installed.
        if out.exists():
            backup = Path(tempfile.mkdtemp(prefix='.mesa-previous-', dir=parent))
            backup.rmdir()
            out.rename(backup)
        try:
            candidate.rename(out)
            activated = True
            os.replace(wrapper_tmp, bindir/'cascadia-gpu')
            wrapper_tmp = None
        except Exception:
            if activated and out.exists(): shutil.rmtree(out)
            if backup is not None and backup.exists(): backup.rename(out)
            raise
        if backup is not None and backup.exists(): shutil.rmtree(backup)
    finally:
        if candidate.exists(): shutil.rmtree(candidate)
        if wrapper_tmp is not None: wrapper_tmp.unlink(missing_ok=True)
    print('Installed shared A12 Mesa; launch a GPU session with cascadia-gpu COMMAND')

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('command', choices=['manifest', 'install'])
    p.add_argument('stage', type=Path)
    p.add_argument('--root', type=Path)
    p.add_argument('--revision'); p.add_argument('--patch-sha')
    p.add_argument('--dependency-dir', help='existing absolute dependency directory inside target root')
    a = p.parse_args()
    if a.command == 'manifest':
        if not a.revision or not a.patch_sha: p.error('manifest needs revision and patch SHA')
        manifest(a.stage, a.revision, a.patch_sha)
    else:
        if not a.root: p.error('install needs --root')
        install(a.stage, a.root, a.dependency_dir)
