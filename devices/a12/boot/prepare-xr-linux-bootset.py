#!/usr/bin/env python3
"""Prepare the ticketed first-stage Linux boot set for an iPhone XR.

This deliberately keeps the Apple DeviceTree pristine. The bundled XR iBSS
patcher applies two validation bypasses and the selected boot-args literal.
Linux-specific hardware description belongs in the
separate DTB appended to the later m1n1 payload, not in Apple's ADT.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
# A12_BOARD: n841 (XR, the default) or d331p (XS Max), which has its own
# directories under the ignored boot/inputs and boot/extracted.
BOARD = os.environ.get("A12_BOARD", "n841")
SUB = "" if BOARD == "n841" else "/" + BOARD
DEFAULT_OUT = ROOT / f"a12/boot/inputs{SUB}"
DEFAULT_IBSS = ROOT / f"a12/boot/extracted{SUB}/iBSS.raw"
DEFAULT_ADT = ROOT / f"a12/boot/extracted{SUB}/DeviceTree.raw"
DEFAULT_TICKET = ROOT / f"a12/boot/extracted{SUB}/apticket.im4m"
DEFAULT_M1N1 = ROOT / ("a12/build/payload/m1n1-idevice.macho" if BOARD == "n841"
                       else f"a12/build/payload/m1n1-{BOARD}.macho")

# These are consumed by iBoot; the Linux command line is carried separately.
IBSS_BOOTARGS = "-v wdt=-1"


def run(*args: object) -> None:
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run([str(x) for x in args], check=True)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def require_file(path: Path, label: str) -> None:
    if not path.is_file() or path.stat().st_size == 0:
        raise SystemExit(f"missing {label}: {path}")


def wrap(source: Path, output: Path, fourcc: str, ticket: Path) -> None:
    run("img4", "-i", source, "-o", output, "-A", "-T", fourcc, "-M", ticket)


def verify_img4(image: Path, expected: Path, ticket: Path, scratch: Path) -> None:
    payload = scratch / f".{image.name}.payload"
    embedded = scratch / f".{image.name}.im4m"
    run("img4", "-i", image, "-o", payload)
    run("img4", "-i", image, "-m", embedded)
    if payload.read_bytes() != expected.read_bytes():
        raise SystemExit(f"payload verification failed: {image}")
    if embedded.read_bytes() != ticket.read_bytes():
        raise SystemExit(f"IM4M verification failed: {image}")


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--ibss", type=Path, default=DEFAULT_IBSS)
    p.add_argument("--devicetree", type=Path, default=DEFAULT_ADT)
    p.add_argument("--ticket", type=Path, default=DEFAULT_TICKET)
    p.add_argument("--m1n1", type=Path, default=DEFAULT_M1N1)
    p.add_argument("--output", type=Path, default=DEFAULT_OUT)
    serial_options = p.add_mutually_exclusive_group()
    serial_options.add_argument("--serial", dest="serial", action="store_true",
                                help="enable DCSD console with serial=3 (default)")
    serial_options.add_argument("--no-serial", dest="serial", action="store_false",
                                help="omit the serial boot argument")
    p.set_defaults(serial=True)
    args = p.parse_args()
    boot_args = IBSS_BOOTARGS + (" serial=3" if args.serial else "")

    for path, label in ((args.ibss, "raw iBSS"),
                        (args.devicetree, "pristine XR DeviceTree"),
                        (args.ticket, "XR IM4M"),
                        (args.m1n1, "XR m1n1 payload")):
        require_file(path, label)
    if shutil.which("img4") is None:
        raise SystemExit("img4 is not installed or not in PATH")

    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # This directory also holds independently generated radio firmware and
    # the later Linux payload. Never remove it wholesale when rebuilding the
    # first-stage boot set.
    scratch = out / ".verify"
    if scratch.exists():
        shutil.rmtree(scratch)
    for name in (
        "iBSS.linux.serial.raw", f"DeviceTree.{BOARD}ap.raw",
        "RestoreTrustCache.empty.raw", "DeviceTree.img4",
        "RestoreTrustCache.img4", "m1n1.img4", "m1n1-idevice.macho",
        "bootset.json", "SHA256SUMS",
    ):
        (out / name).unlink(missing_ok=True)
    scratch.mkdir()

    patched_ibss = out / "iBSS.linux.serial.raw"
    ibss_input = out / ".iBSS.pristine.raw"
    shutil.copy2(args.ibss, ibss_input)
    run(sys.executable, Path(__file__).with_name("patch_iboot.py"), ibss_input,
        boot_args, "--serial" if args.serial else "--no-serial",
        # The XS Max boots with any IM4M, as in badapple. Its own ticket
        # alone left iBoot in recovery at bootx.
        *(["--any-im4m"] if BOARD == "d331p" else []))
    generated = Path(str(ibss_input) + ".patched")
    if not generated.is_file():
        raise SystemExit("patch_iboot did not produce its .patched output")
    generated.replace(patched_ibss)
    ibss_input.unlink()

    # Check the requested literal appears once and that the opt-out stays off.
    data = patched_ibss.read_bytes()
    if data.count((boot_args + "\0").encode()) != 1:
        raise SystemExit("requested iBSS boot-args literal was not installed exactly once")
    if not args.serial and b"serial=3\0" in data:
        raise SystemExit("serial boot argument unexpectedly present")

    pristine_adt = out / f"DeviceTree.{BOARD}ap.raw"
    shutil.copy2(args.devicetree, pristine_adt)
    m1n1 = out / "m1n1-idevice.macho"
    shutil.copy2(args.m1n1, m1n1)

    # A minimally empty restore trust cache. iBoot only needs a valid ticketed
    # rtsc object before the rkrn handoff; m1n1 does not consume this payload.
    empty_tc = out / "RestoreTrustCache.empty.raw"
    empty_tc.write_bytes(b"\0\0\0\0")

    dt_img4 = out / "DeviceTree.img4"
    tc_img4 = out / "RestoreTrustCache.img4"
    krn_img4 = out / "m1n1.img4"
    wrap(pristine_adt, dt_img4, "rdtr", args.ticket)
    wrap(empty_tc, tc_img4, "rtsc", args.ticket)
    wrap(m1n1, krn_img4, "rkrn", args.ticket)

    verify_img4(dt_img4, pristine_adt, args.ticket, scratch)
    verify_img4(tc_img4, empty_tc, args.ticket, scratch)
    verify_img4(krn_img4, m1n1, args.ticket, scratch)
    shutil.rmtree(scratch)

    artifacts = [patched_ibss, pristine_adt, empty_tc, dt_img4, tc_img4, krn_img4, m1n1]
    records = {p.name: {"bytes": p.stat().st_size, "sha256": sha256(p)} for p in artifacts}
    manifest = {
        "schema": 1,
        "target": f"{BOARD}ap/T8020",
        "purpose": "m1n1 proxy bring-up",
        "ibssBootArgs": boot_args,
        "serial": 3 if args.serial else None,
        "deviceTreePolicy": "pristine Apple ADT; no iOS content-protect/diagnostics edits",
        "ticketSha256": sha256(args.ticket),
        "artifacts": records,
    }
    (out / "bootset.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    (out / "SHA256SUMS").write_text("".join(
        f"{records[p.name]['sha256']}  {p.name}\n" for p in artifacts))

    print(f"[+] XR Linux first-stage boot set ready: {out}")
    print("[+] iBSS DCSD console " + ("enabled with serial=3" if args.serial else "disabled"))
    print("[+] Apple DeviceTree is byte-identical to the pristine input")


if __name__ == "__main__":
    main()
