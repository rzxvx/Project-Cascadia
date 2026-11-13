#!/usr/bin/env python3
"""Install J171 backlight permissions and optional Omarchy labwc session wiring."""
import argparse
from pathlib import Path
import re
import shutil

HERE = Path(__file__).resolve().parent
MARKER = '# Cascadia J171 native display/G11 session (software fallback included).'

def install(root, device):
    root = root.resolve()
    if device != 'ipad8':
        raise ValueError('J171 display setup requires --device ipad8')
    if root == Path('/') or not (root/'etc/os-release').is_file():
        raise ValueError('provide an existing target rootfs, never host /')
    session = root/'usr/local/libexec/arch-ipad-login-session'
    updated = None
    if session.is_file():
        if not session.resolve().is_relative_to(root):
            raise ValueError('session path escapes rootfs')
        text = session.read_text()
        if MARKER not in text:
            # Migrate only our exact earlier RAM-opt-in experiment, if present.
            text = re.sub(r'^# iPad-only, RAM opt-in:.*?^fi\n', '', text,
                          flags=re.MULTILINE | re.DOTALL)
            if text.count('exec /usr/bin/labwc -m') != 1:
                raise ValueError('unrecognized Omarchy session; no files changed')
            updated = text.replace('exec /usr/bin/labwc -m', MARKER +
                '\nexec /usr/local/bin/cascadia-ipad-labwc -m > "$XDG_RUNTIME_DIR/agx-desktop.log" 2>&1')
    files = [
        ('etc/udev/rules.d/70-cascadia-j171-backlight.rules', 0o644),
        ('usr/local/bin/cascadia-ipad-labwc', 0o755),
    ]
    for relative, mode in files:
        dst = root/relative
        if not dst.parent.resolve().is_relative_to(root):
            raise ValueError("display parent escapes rootfs")
        if dst.is_symlink() or not dst.resolve().is_relative_to(root):
            raise ValueError('display installation path escapes rootfs')
    for relative, mode in files:
        dst = root/relative
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(HERE/'display-overlay'/relative, dst)
        dst.chmod(mode)
    if updated is not None:
        backup = session.with_name(session.name+'.before-cascadia-display')
        if not backup.exists():
            shutil.copy2(session, backup)
        session.write_text(updated)
    print('Installed iPad backlight permissions; '+
          ('Omarchy labwc wired to guarded G11 session' if session.is_file()
           else 'no Omarchy session found; desktop unchanged'))

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', required=True, type=Path)
    p.add_argument('--device', required=True, choices=['xr', 'ipad8', 'xsmax'])
    args = p.parse_args()
    install(args.root, args.device)
