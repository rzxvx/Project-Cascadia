"""Shared profile/manifest helpers; importing this never touches USB."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

A12 = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('a12_device_profile', A12/'profile.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def run(*args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def wrap(raw, out, kind, ticket):
    run('img4', '-i', raw, '-o', out, '-A', '-T', kind, '-M', ticket,
        stdout=subprocess.DEVNULL)
    verify = Path(str(out)+'.verify')
    run('img4', '-i', out, '-o', verify, stdout=subprocess.DEVNULL)
    if verify.read_bytes() != Path(raw).read_bytes():
        raise ValueError('IMG4 extraction mismatch: '+str(out))
    verify.unlink()


def write_manifest(path, p, artifacts, **extra):
    data = dict(schema=1, device=p['name'], product=p['product'],
                board=p['board'], cpid=p['cpid'], bdid=p['bdid'],
                artifacts={Path(f).name: sha(f) for f in artifacts}, **extra)
    Path(path).write_text(json.dumps(data, indent=2)+'\n')


def verify_manifest(path, p):
    path = Path(path)
    data = json.loads(path.read_text())
    for k in ('device', 'product', 'board', 'cpid', 'bdid'):
        expected = p['name'] if k == 'device' else p[k]
        if data.get(k) != expected:
            raise ValueError('boot manifest does not match selected device: '+k)
    for name, digest in data['artifacts'].items():
        if Path(name).name != name or sha(path.parent/name) != digest:
            raise ValueError('artifact checksum mismatch: '+name)
    return data
