#!/usr/bin/env python3
"""Extract USB/PMU ADT evidence from the pinned official IPSW, offline only.

Public DeviceTree keys are those used by Legacy iOS Kit's pinned key database.
No USB device, /dev/mem, I2C interface, firmware upload or system service access.
The full decrypted Apple blob stays under ignored build/usb-host-research/.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import dtparse

IPSW_SHA256 = "ce6cd59a4a3271eccb93b31c8d40e94ad966112999576663b3ab235196154756"
ADT_SHA256 = "5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72"
MEMBER = "Firmware/all_flash/all_flash.p105ap.production/DeviceTree.p105ap.img3"
KEY_URL = ("https://raw.githubusercontent.com/LukeZGD/Legacy-iOS-Kit-Keys/"
           "af6bf5934dc61ed557a967a3f42ab7fb8ed8c45e/iPad2,5/12H321/index.html")
IV = "c9c12cfe3a16823f1d678775c7aedc88"
KEY = "050f2457801615132e7455b622c8c8dfb65f09d4ff1eb86b5c0a1160678117c2"
NODES = {"tristar", "otgphyctrl", "usb-complex", "usb-device", "dock", "pmu", "charger",
         "usb-ehci", "usb-ehci-port1", "usb-ohci0", "usb-ohci0-port1"}


def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def img3_data(raw):
    if len(raw) < 20 or raw[:4] != b"3gmI":
        raise ValueError("not an IMG3 file")
    declared_total = struct.unpack_from("<I", raw, 4)[0]
    if declared_total != len(raw):
        raise ValueError("IMG3 declared total length does not match input")
    off = 20
    while off + 12 <= len(raw):
        tag, total, size = struct.unpack_from("<4sII", raw, off)
        if total < 12 or off + total > len(raw) or size > total - 12:
            raise ValueError("malformed IMG3 tag bounds")
        if tag == b"ATAD":
            cipher = raw[off + 12:off + total]
            if not size or not cipher or len(cipher) % 16:
                raise ValueError("DATA ciphertext must be AES block aligned")
            return cipher, size
        off += total
    raise ValueError("IMG3 DATA tag missing")


def select_nodes(root):
    selected = []
    phandles = {}
    def index(node, path=""):
        name = node["props"].get("name", b"?").rstrip(b"\0").decode("ascii")
        path += "/" + name
        phandle = node["props"].get("AAPL,phandle")
        if phandle is not None and len(phandle) == 4:
            phandles[struct.unpack("<I", phandle)[0]] = (path, node)
        for child in node["children"]:
            index(child, path)
    index(root)
    def walk(node, path=""):
        name = node["props"].get("name", b"?").rstrip(b"\0").decode("ascii")
        path = path + "/" + name
        if name in NODES or name == "arm-io":
            props = node["props"] if name != "arm-io" else {
                k: v for k, v in node["props"].items()
                if k in {"name", "AAPL,phandle", "hsic-enabled"}}
            # Hex is the authoritative representation. Printable-number decoding
            # is convenient, but not an I2C address or phandle interpretation.
            entry = {"path": path, "props": {
                key: {"raw_hex": value.hex(), "decoded": dtparse.decode_value(value)}
                for key, value in props.items()}, "function_refs": {}}
            for key, value in props.items():
                if key.startswith("function-") and len(value) >= 8:
                    phandle = struct.unpack_from("<I", value)[0]
                    provider = phandles.get(phandle)
                    entry["function_refs"][key] = {
                        "phandle_le": hex(phandle),
                        "provider_path": provider[0] if provider else None,
                        "fourcc_le_integer": hex(struct.unpack_from("<I", value, 4)[0]),
                        "fourcc": value[4:8][::-1].decode("ascii", errors="replace"),
                        "args_le_u32": list(struct.unpack(
                            "<" + "I" * ((len(value) - 8) // 4), value[8:]))
                            if not (len(value) - 8) % 4 else None,
                        "interpretation": "ADT dispatch only; arguments are not proven register writes"}
            selected.append(entry)
        for child in node["children"]:
            walk(child, path)
    walk(root)
    return selected


def extract(ipsw, output):
    allowed = ROOT / "build/usb-host-research"
    output = output.resolve()
    if output != allowed and allowed not in output.parents:
        raise ValueError("output must be under build/usb-host-research")
    if output.exists() and any(output.iterdir()):
        raise ValueError("output directory must be new or empty")
    if digest_file(ipsw) != IPSW_SHA256:
        raise ValueError("IPSW SHA256 mismatch: only official p105 12H321 accepted")
    with zipfile.ZipFile(ipsw) as archive:
        raw = archive.read(MEMBER)
    cipher, size = img3_data(raw)
    result = subprocess.run(["openssl", "enc", "-aes-256-cbc", "-d", "-nopad",
                             "-K", KEY, "-iv", IV], input=cipher,
                            capture_output=True, check=True)
    plaintext = result.stdout[:size]
    digest = hashlib.sha256(plaintext).hexdigest()
    if digest != ADT_SHA256:
        raise ValueError("decrypted DeviceTree SHA256 mismatch")
    root, end = dtparse.parse_node(plaintext, 0)
    if end != len(plaintext):
        raise ValueError("ADT parser did not consume full payload")
    output.mkdir(parents=True, exist_ok=True)
    (output / "DeviceTree.p105ap.raw").write_bytes(plaintext)
    report = {"ipsw_sha256": IPSW_SHA256, "member": MEMBER,
              "img3_sha256": hashlib.sha256(raw).hexdigest(),
              "adt_sha256": digest, "public_key_source": KEY_URL,
              "device_access": False, "selected": select_nodes(root)}
    (output / "adt-usb-power.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipsw", type=Path)
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/usb-host-research/adt-evidence")
    args = parser.parse_args()
    try:
        report = extract(args.ipsw, args.output_dir)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"Offline ADT evidence: {len(report['selected'])} selected nodes, "
          f"SHA256 {report['adt_sha256']}; no device access.")


if __name__ == "__main__":
    main()
