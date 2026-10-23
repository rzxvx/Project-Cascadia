#!/usr/bin/env python3
"""Test connection/overwrite boundaries and upload receipts without a device."""
import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ipad_session", ROOT / "tools/host/ipad-session.py")
session = importlib.util.module_from_spec(spec)
spec.loader.exec_module(session)


class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.key = self.dir / "identity"
        self.key.write_text("FAKE OFFLINE IDENTITY, NOT A KEY\n")
        self.pin = self.dir / "known_hosts"
        self.pin.write_text("cascadia-p105 ssh-ed25519 QUJDRA==\n")
        for p in (self.key, self.pin):
            p.chmod(0o600)
        self.argv = ["--identity", str(self.key), "--known-hosts", str(self.pin)]

    def tearDown(self):
        self.tmp.cleanup()

    def call(self, *args, result=None):
        result = result or subprocess.CompletedProcess([], 0, b"", b"")
        with patch.object(session.subprocess, "run", return_value=result) as run:
            with contextlib.redirect_stderr(io.StringIO()):
                rc = session.main(self.argv + list(args))
            return rc, run

    def test_missing_device_gate_never_connects(self):
        with patch.object(session.subprocess, "run") as run:
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                session.main(self.argv + ["status"])
            run.assert_not_called()

    def test_readonly_script_exact_and_strict_ssh_flags(self):
        out = self.dir / "status.log"
        rc, run = self.call("--device-access", "status", "--out", str(out))
        self.assertEqual(rc, 0)
        command = run.call_args.args[0]
        self.assertIn("StrictHostKeyChecking=yes", command)
        self.assertIn("HostKeyAlias=cascadia-p105", command)
        self.assertIn("BatchMode=yes", command)
        self.assertEqual(command[-2:], ["root@10.55.0.2", "sh -s"])
        self.assertEqual(run.call_args.kwargs["input"], session.STATUS.encode())
        self.assertEqual(out.stat().st_mode & 0o777, 0o600)

    def test_existing_output_refused_before_connection(self):
        out = self.dir / "preserve"
        out.write_text("existing user data")
        rc, run = self.call("--device-access", "logs", "--out", str(out))
        self.assertEqual(rc, 1)
        run.assert_not_called()
        self.assertEqual(out.read_text(), "existing user data")

    def test_bad_address_public_key_and_permissions_deny(self):
        for host in ("-oProxyCommand=bad", "example.org", "10.55.0.2;reboot"):
            rc, run = self.call("--host=" + host, "--device-access", "status")
            self.assertEqual(rc, 1)
            run.assert_not_called()
        self.key.chmod(0o644)
        rc, run = self.call("--device-access", "status")
        self.assertEqual(rc, 1)
        run.assert_not_called()
        self.key.chmod(0o600)
        self.pin.write_text("other-host ssh-ed25519 QUJDRA==\n")
        rc, run = self.call("--device-access", "status")
        self.assertEqual(rc, 1)
        run.assert_not_called()

    def test_timeout_and_remote_error_preserve_failure(self):
        with patch.object(session.subprocess, "run", side_effect=subprocess.TimeoutExpired("ssh", 45)):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(session.main(self.argv + ["--device-access", "test"]), 1)
        out = self.dir / "failure.log"
        rc, run = self.call("--device-access", "logs", "--out", str(out),
                            result=subprocess.CompletedProcess([], 255, b"partial", b"SSH failed"))
        self.assertEqual(rc, 255)
        self.assertEqual(out.read_bytes(), b"partialSSH failed")
        self.assertEqual(run.call_count, 1)

    def test_deploy_gate_and_file_bounds_before_connection(self):
        src = self.dir / "artifact"
        src.write_bytes(b"test")
        rc, run = self.call("--device-access", "deploy", "--file", str(src))
        self.assertEqual(rc, 1)
        run.assert_not_called()
        with src.open("wb") as f:
            f.truncate(session.MAX_DEPLOY + 1)
        rc, run = self.call("--device-access", "deploy", "--allow-ram-deploy", "--file", str(src))
        self.assertEqual(rc, 1)
        run.assert_not_called()
        os.mkfifo(self.dir / "fifo")
        rc, run = self.call("--device-access", "deploy", "--allow-ram-deploy", "--file", str(self.dir / "fifo"))
        self.assertEqual(rc, 1)
        run.assert_not_called()

    def test_real_child_timeout_retains_diagnostics_and_never_accepts_upload(self):
        real_run = subprocess.run
        child = ("import os,time; os.write(1,b'partial kernel output\\n'); "
                 "os.write(2,b'partial SSH error\\n'); time.sleep(10)")
        src = self.dir / "payload"
        src.write_bytes(b"offline payload")
        for action in ("logs", "deploy"):
            out = self.dir / (action + ".timeout.log")
            argv = self.argv + ["--device-access", action, "--out", str(out)]
            if action == "deploy":
                argv += ["--allow-ram-deploy", "--file", str(src)]
            def run_offline_child(command, **kwargs):
                # Real subprocess kill/reap/captured TimeoutExpired bytes;
                # substitute a local child before any SSH process can start.
                kwargs["timeout"] = 0.5
                return real_run([sys.executable, "-c", child], **kwargs)
            with patch.object(session.subprocess, "run", side_effect=run_offline_child) as run:
                with contextlib.redirect_stderr(io.StringIO()) as errors:
                    rc = session.main(argv)
            self.assertEqual(rc, 1)
            self.assertEqual(out.read_bytes(), b"partial kernel output\npartial SSH error\n")
            self.assertEqual(out.stat().st_mode & 0o777, 0o600)
            self.assertIn("remote completion is unconfirmed", errors.getvalue())
            self.assertEqual(run.call_count, 1)

    def test_deploy_checks_exact_hash_and_new_ram_path(self):
        src = self.dir / "artifact"
        src.write_bytes(b"test artifact")
        expected = hashlib.sha256(src.read_bytes()).hexdigest()
        remote = "/run/cascadia-dev.ABCDef12/artifact"
        out = self.dir / "receipt.log"
        receipt = (expected + "  " + remote + "\n" + remote + "\n").encode()
        rc, run = self.call("--device-access", "deploy", "--allow-ram-deploy", "--file", str(src),
                            "--out", str(out), result=subprocess.CompletedProcess([], 0, receipt, b""))
        self.assertEqual(rc, 0)
        self.assertEqual(run.call_args.kwargs["input"], src.read_bytes())
        self.assertIn("tmpfs", run.call_args.args[0][-1])
        self.assertNotIn("/dev/mem", run.call_args.args[0][-1])
        for i, bad in enumerate((b"unconfirmed", receipt.replace(remote.encode(), b"/etc/unsafe"),
                                 receipt.replace(expected.encode(), b"0" * 64))):
            rc, run = self.call("--device-access", "deploy", "--allow-ram-deploy", "--file", str(src),
                                "--out", str(self.dir / f"bad{i}"),
                                result=subprocess.CompletedProcess([], 0, bad, b""))
            self.assertEqual(rc, 1)
            self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
