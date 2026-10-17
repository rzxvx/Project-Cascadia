#!/usr/bin/env python3
"""Run repository Python test entry points, including hyphenated filenames.

These test suites use fixtures and mocked hardware.  This runner does not
invoke device tools, Docker, installation, boot or network configuration.
"""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=int, default=120,
                        help="maximum seconds per test entry point (default: 120)")
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("timeout must be positive")
    root = Path(__file__).resolve().parents[1]
    tests = sorted((root / "tests").glob("test*.py"))
    if not tests:
        parser.exit(1, "No offline test entry points found.\n")
    failures = []
    for test in tests:
        print(f"Running {test.relative_to(root)}", flush=True)
        try:
            result = subprocess.run([sys.executable, str(test)], cwd=root,
                                    timeout=args.timeout)
            if result.returncode:
                failures.append(f"{test.name}: exit {result.returncode}")
        except subprocess.TimeoutExpired:
            failures.append(f"{test.name}: timed out")
    print(f"Offline test entry points: {len(tests)}, failures: {len(failures)}")
    for failure in failures:
        print(failure, file=sys.stderr)
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
