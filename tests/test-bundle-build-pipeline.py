"""Run the actual CLI bundle pipeline with a failing local builder fixture."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Pipeline(unittest.TestCase):
    def test_failure_survives_tail_and_historical_control_loses_it(self):
        source = (ROOT / "cascadia").read_text()
        commands = re.findall(r'in_docker "([^"]*bash build-bundle\.sh[^\"]*)"', source)
        self.assertEqual(len(commands), 1)
        command = commands[0]
        # Only the inspected fixed command is executed, against local fake bash.
        self.assertEqual(command, "set -o pipefail; bash build-bundle.sh 2>&1 | tail -5")
        with tempfile.TemporaryDirectory() as directory:
            fake = Path(directory) / "bash"
            fake.write_text('#!/bin/sh\necho "controlled bundle failure"\nexit 73\n')
            fake.chmod(0o700)
            environment = dict(os.environ, PATH=directory + ":" + os.environ["PATH"])
            environment.pop("BASH_ENV", None)
            environment.pop("ENV", None)
            fixed = subprocess.run(["/bin/bash", "-c", command], cwd=directory,
                                   env=environment, capture_output=True, text=True)
            old = subprocess.run(["/bin/bash", "-c", command.removeprefix("set -o pipefail; ")],
                                 cwd=directory, env=environment, capture_output=True, text=True)
        self.assertEqual(fixed.returncode, 73)
        self.assertEqual(old.returncode, 0)
        self.assertEqual(fixed.stdout, "controlled bundle failure\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
