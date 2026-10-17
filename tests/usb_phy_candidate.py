"""Reconstruct the reviewable PHY/USB candidate in a temporary local tree.

Original-source controls use exact baseline Git bytes; candidate regressions
compile the separately applied 0002 patch. The production tree is never edited.
"""
from functools import lru_cache
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('phy_baseline_fixture', ROOT / 'tests/test-usb-phy-core-lifecycle.py')
BASELINE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BASELINE)
HASHES = {
    'drivers/phy/phy-core.c': '259e56ec9a9064cd120ab6f46e1457a5d78e089d56dbf989a0bb51297fe143f8',
    'include/linux/phy/phy.h': '7ba2f0830074185434c1029144fab43792ea16624b6bb5e327a8fd1834dcb528',
    'drivers/usb/core/phy.c': '700f01a45cbfd4fa08778c1507ad352cc88ae11655437e0ffdad05a876efb7c6',
    'drivers/usb/core/phy.h': '8e70e5905aa2cd07c48be10059ae1ed8ccc450f5abe01a25f5b901b4b3c776be',
    'drivers/usb/core/hcd.c': 'deeb79afb85362dfeda477e240e70b1b44bd2995b75d06aa92cf3a49d711e495',
}
PATCH = ROOT / 'patches/tree/0002-phy-power-reference-recovery.patch'


@lru_cache(maxsize=1)
def sources():
    patch = PATCH.read_text()
    before = re.findall(r'^--- a/([^\n]+)$', patch, re.M)
    after = re.findall(r'^\+\+\+ b/([^\n]+)$', patch, re.M)
    if len(before) != len(HASHES) or before != after or set(before) != set(HASHES):
        raise ValueError('candidate patch must modify only the five audited PHY/USB files')
    with tempfile.TemporaryDirectory(prefix='cascadia-phy-candidate-') as td:
        tree = Path(td)
        for relative, expected_hash in HASHES.items():
            target = tree / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(BASELINE.baseline_source(relative, expected_hash))
        for check in (True, False):
            command = ['git', 'apply', '--whitespace=error-all']
            if check:
                command.append('--check')
            command.append(str(PATCH))
            subprocess.run(command, cwd=tree, capture_output=True, check=True)
        result = {relative: (tree / relative).read_text() for relative in HASHES}
    if not re.search(r'\bint\s+power_off_error\s*;', result['include/linux/phy/phy.h']):
        raise ValueError('candidate PHY structure is missing its error latch')
    return result
