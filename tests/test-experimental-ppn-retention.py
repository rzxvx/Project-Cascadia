"""Actual PPN engine retained-buffer warnings after rejected calls; no hardware."""
from pathlib import Path
import os, subprocess, tempfile
R = Path(__file__).resolve().parents[1]
source = R / "experiments/p105-ppn"
fixture = R / "tests/fixtures/ppn-stage-retention/test_retention.c"
with tempfile.TemporaryDirectory(prefix="cascadia-ppn-retention-") as td:
    binary = Path(td) / "retention"
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-fno-pie", "-no-pie", "-I" + str(source), str(fixture),
                    str(source / "ppn_stage_engine.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1",
                        "UBSAN_OPTIONS": "halt_on_error=1"})
