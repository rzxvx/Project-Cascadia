#!/usr/bin/env python3
"""Exercise the base-package installer with an offline package transport."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/add-base-packages.sh"

class NetconfigTest(unittest.TestCase):
    def install(self, source, deliver=True):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scripts = root / "scripts"
            tree = root / "build/initramfs-root"
            scripts.mkdir()
            (tree / "etc").mkdir(parents=True)
            (tree / "usr/share/fonts/dejavu").mkdir(parents=True)
            (tree / "etc/passwd").write_text("root:x:0:0:root:/root:/bin/sh\n")
            (tree / "etc/group").write_text("root:x:0:root\n")
            (scripts / "add-base-packages.sh").write_text(source)
            helper = "#!/bin/bash\nset -eu\n"
            helper += "printf '%s\\n' \"$@\" > packages.requested\n"
            if deliver:
                helper += "for p; do if [ \"$p\" = libtirpc-conf ]; then printf 'tcp tpi_cots_ord v inet tcp - -\\n' > build/initramfs-root/etc/netconfig; fi; done\n"
            else:
                helper += ": # Simulate incomplete unpack with a successful exit.\n"
            (scripts / "add-apk-packages.sh").write_text(helper)
            result = subprocess.run(["bash", str(scripts / "add-base-packages.sh")], cwd=root, capture_output=True, text=True)
            self.assertIn("alsa-utils", (root / "packages.requested").read_text().splitlines())
            config = tree / "etc/netconfig"
            return result, config.read_text() if config.exists() else None

    def test_explicit_data_dependency_is_installed(self):
        result, config = self.install(SCRIPT.read_text())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("inet tcp", config)

    def test_missing_configuration_fails_before_later_setup(self):
        result, config = self.install(SCRIPT.read_text(), deliver=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing /etc/netconfig", result.stderr)
        self.assertIsNone(config)

    def test_existing_tree_repair_checks_both_nfs_and_audio(self):
        source = (ROOT / "cascadia").read_text()
        start = source.index('    if [ ! -x "$ROOT/build/initramfs-root/sbin/wpa_supplicant" ]')
        end = source.index('\n    fi', start) + len('\n    fi')
        block = source[start:end]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tree = root / "build/initramfs-root"
            (root / "scripts").mkdir()
            (root / "scripts/add-base-packages.sh").write_text('touch "$ROOT/repaired"\n')
            for name in ("sbin/wpa_supplicant", "usr/sbin/iw", "usr/bin/fbkeyboard",
                         "usr/lib/bluetooth/bluetoothd", "usr/bin/aplay"):
                path = tree / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch(); path.chmod(0o755)
            config = tree / "etc/netconfig"
            config.parent.mkdir()
            audio = tree / "usr/bin/aplay"
            marker = root / "repaired"
            for nfs_ready, audio_ready in ((True, True), (False, True), (True, False), (False, False)):
                with self.subTest(nfs_ready=nfs_ready, audio_ready=audio_ready):
                    config.write_text("tcp\n" if nfs_ready else "")
                    audio.chmod(0o755 if audio_ready else 0o644)
                    marker.unlink(missing_ok=True)
                    result = subprocess.run(["bash", "-c", 'export ROOT="$1"; fail() { exit 1; };\n' + block,
                                             "test", str(root)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(marker.exists(), not (nfs_ready and audio_ready))

if __name__ == "__main__":
    unittest.main()
