#!/usr/bin/env python3
"""Test actual Alpine archive filter without Docker, network or hardware."""
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import subprocess
import sys

source=Path(__file__).with_name('k93-prepare-alpine.py')
spec=importlib.util.spec_from_file_location('alpine',source)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
assert module.BROWSER_PACKAGES==['firefox-esr','xdotool','font-dejavu']
assert not set(module.BROWSER_PACKAGES)&set(module.PACKAGES)
# This contradiction must fail before any Docker invocation or output creation.
invalid=subprocess.run([sys.executable,str(source),'--base-only','--browser'],
                       capture_output=True,text=True)
assert invalid.returncode!=0 and 'Browser requires the graphical packages' in invalid.stderr

def archive(entries):
    data=io.BytesIO()
    with tarfile.open(fileobj=data,mode='w') as stream:
        for name,link in entries:
            item=tarfile.TarInfo(name)
            if link is None:
                item.size=4;stream.addfile(item,io.BytesIO(b'test'))
            else:
                item.type=tarfile.SYMTYPE;item.linkname=link;stream.addfile(item)
    data.seek(0)
    return tarfile.open(fileobj=data)

with tempfile.TemporaryDirectory(prefix='k93-alpine-filter-') as directory:
    root=Path(directory)/'root';root.mkdir()
    with archive([('bin/busybox',None),('usr/bin/yes','/bin/busybox')]) as stream:
        stream.extractall(root,filter=module.rootfs_filter)
    assert (root/'usr/bin/yes').read_bytes()==b'test'
    assert (root/'usr/bin/yes').resolve()==root/'bin/busybox'
    for entry in [('../outside',None),('bad','../../outside'),('bad','/../../outside')]:
        try:
            with archive([entry]) as stream:
                stream.extractall(root,filter=module.rootfs_filter)
        except tarfile.FilterError:
            pass
        else:
            raise AssertionError('Archive escape accepted: '+repr(entry))
    assert not (Path(directory)/'outside').exists()
print('PASS: actual rootfs filter rebases absolute applet links and rejects escapes; browser/base conflict fails before setup; no hardware')
