#!/usr/bin/env python3
"""
Regression tests for src/adt_to_dts.py's SoC and board detection.

The converter used to recognise only the A5, A6 and A6X by name: an A5X
(S5L8945X, iPad 3) tree came out as a plain apple,s5l8940x, and every header
said iPad2,5 whatever the input was.  The A5X has to keep s5l8940x after its
own compatible, or the tree matches no machine in mach-apple and boots
without the AIC or a tick.  The ADT also spells the CPU vendor "ARM,", which
Linux does not match.

The ADTs are built here, a few nodes each, rather than read from an IPSW: no
Apple binaries in the repository.

    python3 tests/test-adt-to-dts.py
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "src"))
APPLE_C = os.path.join(REPO, "patches", "files", "arch", "arm", "mach-apple", "apple.c")

import adt_to_dts  # noqa: E402
from dtparse import parse_node  # noqa: E402

FAILED = []


def check(label, got, want):
    if got == want:
        print(f"  PASS  {label}")
    else:
        FAILED.append(label)
        print(f"  FAIL  {label}\n        got  {got!r}\n        want {want!r}")


def prop(name, value):
    if isinstance(value, int):
        value = struct.pack("<I", value)
    elif isinstance(value, str):
        value = value.encode() + b"\x00"
    pad = b"\x00" * (-len(value) % 4)
    return name.encode().ljust(32, b"\x00") + struct.pack("<I", len(value)) + value + pad


def node(props, children=()):
    out = struct.pack("<II", len(props), len(children))
    out += b"".join(prop(k, v) for k, v in props)
    return out + b"".join(children)


def adt(board, model, soc, cpu_compat):
    cpu = node([("name", "cpu0"), ("compatible", cpu_compat), ("reg", 0)])
    cpus = node([("name", "cpus")], [cpu])
    ranges = struct.pack("<III", 0, 0x30000000, 0x10000000)
    armio = node([("name", "arm-io"), ("compatible", f"arm-io,{soc}"),
                  ("ranges", ranges)])
    root = node([
        ("name", "device-tree"),
        ("compatible", f"{board}\x00{model}\x00AppleARM"),
        ("model", model),
    ], [cpus, armio])
    tree, _ = parse_node(root, 0)
    return adt_to_dts.AdtToDts(tree).convert()


def line(dts, start):
    for text in dts.splitlines():
        if text.strip().startswith(start):
            return text.strip()
    return None


def machine_compats():
    """The strings in mach-apple's apple_s5l_dt_compat[]."""
    text = open(APPLE_C, encoding="utf-8").read()
    body = text.split("apple_s5l_dt_compat[]", 1)[1].split("};", 1)[0]
    return {part.split('"')[1] for part in body.split(",") if '"' in part}


def root_compats(dts):
    text = line(dts, "compatible =")
    return [part.split('"')[1] for part in text.split(",") if '"' in part]


def main():
    machine = machine_compats()
    j1 = adt("J1AP", "iPad3,1", "s5l8945x", "ARM,cortex-a9\x00ARM,v7")
    check("A5X: root compatible is apple,s5l8945x, then apple,s5l8940x",
          line(j1, "compatible ="),
          'compatible = "apple,s5l8945x", "apple,s5l8940x", "apple,ipad3,1";')
    check("A5X: the root matches mach-apple's machine",
          bool(machine & set(root_compats(j1))), True)
    check("A5X: header names the board in the ADT",
          line(j1, "* Target:"), "* Target: iPad3,1 (J1AP / S5L8945X, Apple A5X).")
    check("CPU vendor lowered to the spelling Linux matches",
          line(j1, 'compatible = "arm,cortex-a9"'), 'compatible = "arm,cortex-a9";')

    p105 = adt("P105AP", "iPad2,5", "s5l8942x", "ARM,cortex-a9\x00ARM,v7")
    check("A5: root compatible unchanged",
          line(p105, "compatible ="), 'compatible = "apple,s5l8942x", "apple,ipad2,5";')
    check("A5: header unchanged",
          line(p105, "* Target:"), "* Target: iPad2,5 (P105AP / S5L8942X, Apple A5).")
    check("A5: the root matches mach-apple's machine",
          bool(machine & set(root_compats(p105))), True)

    p102 = adt("P102AP", "iPad3,4", "s5l8955x", "apple,swift")
    check("A6X: root compatible unchanged",
          line(p102, "compatible ="), 'compatible = "apple,s5l8955x", "apple,ipad3,4";')
    check("A6X: apple,swift kept as it is",
          line(p102, 'compatible = "apple,swift"'), 'compatible = "apple,swift";')

    other = adt("X1AP", "iPad9,9", "s5l9999x", "ARM,cortex-a9")
    check("unknown SoC still falls back to apple,s5l8940x",
          line(other, "compatible ="), 'compatible = "apple,s5l8940x", "apple,ipad9,9";')
    check("unknown SoC: the header says so",
          line(other, "* Target:"), "* Target: iPad9,9 (X1AP / unknown SoC, treated as S5L8940X).")

    print()
    if FAILED:
        print(f"{len(FAILED)} failure(s)")
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
