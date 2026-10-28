"""Unbound production PPN C; original trace oracle, never real NAND or DMA."""
import json
from pathlib import Path
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / "experiments/p105-ppn"


def main():
    oracle = json.loads((SOURCE / "original-program-oracle.json").read_text())
    assert oracle["kernelcache_sha256"] == "db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c"
    assert oracle["iop_sha256"] == "6dfa5dcc6f8cb38be41e48c0edc543b2014a026f7909c35c9ef050ffd23c22d4"
    assert oracle["synthetic_hardware_responses"] is True
    with tempfile.TemporaryDirectory(prefix="cascadia-ppn-engine-") as temp:
        for name, flags in (("plain", []), ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"])):
            binary = Path(temp) / name
            subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            *flags, str(SOURCE / "ppn_stage_engine.c"),
                            str(SOURCE / "test_ppn_stage.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            for bus, mode in ((0, "--trace"), (1, "--trace-bus1")):
                rows = subprocess.check_output([str(binary), mode], text=True).splitlines()
                actual = [tuple(map(int, row.split()[1:])) if row.startswith("M ")
                          else (row[0], *map(int, row.split()[1:])) for row in rows]
                cases = [c for c in oracle["cases"] if c["bus"] == bus]
                assert len(cases) == 1
                expected = []
                for row in cases[0]["trace"]:
                    if row[0] == "virtual-mmio":
                        expected.append((row[1], row[2]))
                    elif row[0] == "dma-start":
                        expected.append(("S", row[1], row[3], row[4], row[5]))
                    elif row[0] == "dma-wait":
                        expected.append(("W", row[1]))
                assert len(expected) == 25 and actual == expected
    print("PPN actual C and both original synthetic bus traces PASS; durability remains unknown")


if __name__ == "__main__":
    main()
