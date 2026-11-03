#!/usr/bin/env python3
"""Exercise the generated desktop bus block with private real D-Bus daemons.

Only fixture Unix sockets, pid files and machine IDs are used. No host system
bus, packages, services or device nodes are changed. DESKTOP_SETUP_SOURCE can
select the unchanged setup script for a regression control.
"""
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("DESKTOP_SETUP_SOURCE", ROOT / "tools/desktop/xfce-setup.sh"))


class SystemBusTest(unittest.TestCase):
    def setUp(self):
        for tool in ("dbus-daemon", "dbus-send", "dbus-uuidgen", "timeout", "sh"):
            if not shutil.which(tool):
                self.skipTest(tool + " unavailable")
        self.tmp = tempfile.TemporaryDirectory(prefix="desktop-bus-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.pidfile = self.root / "dbus.pid"
        self.socket = self.root / "system_bus_socket"
        self.config = self.root / "system.conf"
        self.config.write_text(f'''<busconfig>
<type>system</type><fork/><listen>unix:path={self.socket}</listen>
<pidfile>{self.pidfile}</pidfile>
<policy context="default"><allow user="*"/><allow own="*"/>
<allow send_destination="*"/><allow receive_sender="*"/></policy>
</busconfig>''')
        self.env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(self.socket))
        self.children = []
        self.addCleanup(self.cleanup_daemons)
        text = SOURCE.read_text()
        block = text.split("# The system bus:", 1)[1].split("# touchegg", 1)[0]
        # Keep the actual production statements; isolate their filesystem and
        # daemon configuration. No command or process is mocked.
        block = "# The system bus:" + block
        block = block.replace("/run/dbus", str(self.root))
        block = block.replace("dbus-uuidgen --ensure", "dbus-uuidgen --ensure=" + str(self.root / "machine-id"))
        block = block.replace("dbus-daemon --system", "dbus-daemon --config-file=" + str(self.config))
        self.script = block

    def cleanup_daemons(self):
        # Only this test's configured daemon may be stopped, even if a test
        # deliberately put an invalid or reused PID in the fixture file.
        if self.pidfile.exists():
            value = self.pidfile.read_text().strip()
            if value.isdigit():
                pid = int(value)
                try:
                    cmd = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
                    if ("--config-file=" + str(self.config)).encode() in cmd:
                        os.kill(pid, signal.SIGCONT)
                        os.kill(pid, signal.SIGTERM)
                except (ProcessLookupError, FileNotFoundError):
                    pass
        for process in self.children:
            if process.poll() is None:
                process.send_signal(signal.SIGCONT)
                process.terminate()
            process.wait(timeout=3)

    def ready(self):
        return subprocess.run([
            "timeout", "1", "dbus-send", "--system", "--type=method_call",
            "--print-reply", "--reply-timeout=500", "--dest=org.freedesktop.DBus",
            "/org/freedesktop/DBus", "org.freedesktop.DBus.ListNames",
        ], env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0

    def start(self):
        p = subprocess.Popen(["dbus-daemon", "--config-file=" + str(self.config), "--nofork"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(p)
        for _ in range(50):
            if self.ready():
                return p
            time.sleep(0.02)
        self.fail("Private fixture bus did not start")

    def run_block(self):
        return subprocess.run(["sh", "-s"], input=self.script, text=True,
                              env=self.env, capture_output=True, timeout=8)

    def test_cold_start(self):
        result = self.run_block()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.ready())

    def test_live_bus_is_reused(self):
        p = self.start()
        result = self.run_block()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(int(self.pidfile.read_text()), p.pid)
        self.assertTrue(self.ready())

    def test_crashed_bus_socket_and_pid_are_recovered(self):
        p = self.start()
        p.kill()
        p.wait(timeout=3)
        self.assertTrue(self.socket.is_socket())
        self.assertEqual(int(self.pidfile.read_text()), p.pid)
        result = self.run_block()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.ready(), "A leftover socket is not a working bus")
        self.assertNotEqual(int(self.pidfile.read_text()), p.pid)

    def test_live_unresponsive_bus_is_not_replaced(self):
        p = self.start()
        p.send_signal(signal.SIGSTOP)
        result = self.run_block()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(int(self.pidfile.read_text()), p.pid)
        self.assertIsNone(p.poll())

    def test_invalid_pid_file_is_retained(self):
        self.pidfile.write_text("not-a-pid\n")
        result = self.run_block()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.pidfile.read_text(), "not-a-pid\n")
        self.assertFalse(self.socket.exists())

    def test_unowned_socket_is_retained(self):
        with socket.socket(socket.AF_UNIX) as sock:
            sock.bind(str(self.socket))
        result = self.run_block()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.socket.is_socket())
        self.assertFalse(self.pidfile.exists())


if __name__ == "__main__":
    unittest.main()
