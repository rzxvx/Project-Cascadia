#!/usr/bin/env python3
"""Generate an isolated, unproven host-only build profile; never access USB.

This prepares source inputs, not a deployable boot bundle.  No VBUS/charger
registers are known, so there is intentionally no fake fixed-regulator node.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


HOST_SYMBOLS = {
    "USB_DWC2_HOST": "y",
    "USB_DWC2_PERIPHERAL": "n",
    "USB_DWC2_DUAL_ROLE": "n",
    "USB_GADGET": "n",
    "USB_CDC_COMPOSITE": "n",
    "USB_STORAGE": "y",
    "SCSI": "y",
    "BLK_DEV_SD": "y",
    "USB_HID": "y",
    "HID": "y",
    "HID_GENERIC": "y",
    "FAT_FS": "y",
    "VFAT_FS": "y",
    "NLS": "y",
    "NLS_CODEPAGE_437": "y",
    "NLS_ISO8859_1": "y",
    "NLS_UTF8": "y",
}
BLOCKERS = [
    "D1946/Tristar VBUS source and protection sequence unresolved",
    "Lightning accessory identification/data routing unresolved",
    "Host PHY tuning transition and return-to-device sequence unverified",
    "AIC independent clockevent must pass without peripheral SOF fallback",
    "Keyboard/hub/flash-drive enumeration and DMA never validated on our device",
]


def host_config(text):
    """Replace, rather than append, symbols: no contradictory fragment lines."""
    pattern = re.compile(r"^(?:# )?CONFIG_([A-Z0-9_]+)(?:=| is not set)")
    lines = []
    for line in text.splitlines():
        match = pattern.match(line)
        if match and match[1] in HOST_SYMBOLS:
            continue
        lines.append(line)
    lines += ["", "# EXPERIMENTAL HOST-ONLY: source preparation, NOT boot validated.",
              "# No VBUS source, accessory controller or PHY tuning is enabled.",
              "# Independent AIC clockevent required; no USB peripheral SOF tick."]
    for symbol, value in HOST_SYMBOLS.items():
        lines.append(f"CONFIG_{symbol}=y" if value == "y" else
                     f"# CONFIG_{symbol} is not set")
    return "\n".join(lines) + "\n"


def host_dts(text):
    # This is intentionally constrained to the audited p105ap source structure.
    # A generic replacement after upstream changes could silently target EHCI.
    node = re.search(r"(?P<head>\bdwc2:\s+usb@6100000\s*\{)(?P<body>[^{}]*)(?P<tail>\};)", text)
    if not node:
        raise ValueError("expected p105ap dwc2 node not found")
    body = node["body"]
    if len(re.findall(r'\bdr_mode\s*=\s*"peripheral"\s*;', body)) != 1:
        raise ValueError("expected exactly one peripheral dr_mode in dwc2")
    if re.search(r"\bvbus-supply\s*=", body):
        raise ValueError("upstream now supplies VBUS; review before generating")
    body = re.sub(r'\bdr_mode\s*=\s*"peripheral"\s*;',
                  'dr_mode = "host";', body)
    return ("/* EXPERIMENTAL, UNPROVEN HOST PROFILE. No VBUS regulator.\n"
            " * Do not boot before reviewing p105-usb-host.md and hardware power.\n"
            " * No speculative charger/PMU writes or PHY tuning values added. */\n"
            + text[:node.start()] + node["head"] + body + node["tail"]
            + text[node.end():])


def prepare(source_dir, output_dir):
    source_dir = source_dir.resolve()
    output_dir = output_dir.resolve()
    inputs = [source_dir / "config/p105ap.config", source_dir / "dts/p105ap.dts"]
    # Never overwrite the peripheral fragment, DTS, firmware, or reference build.
    allowed = source_dir / "build/usb-host-research"
    if output_dir != allowed and allowed not in output_dir.parents:
        raise ValueError("output must stay inside build/usb-host-research")
    if output_dir.exists() and any(output_dir.iterdir()):
        raise ValueError("output directory must be new or empty; preserve earlier evidence")
    config = host_config(inputs[0].read_text())
    dts = host_dts(inputs[1].read_text())
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "p105ap-usb-host.config").write_text(config)
    (output_dir / "p105ap-usb-host.dts").write_text(dts)
    report = {"status": "source-only; NOT hardware validated or boot approved",
              "input_sha256": {str(p.relative_to(source_dir)):
                               hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
              "blockers": BLOCKERS, "vbus_regulator_added": False,
              "physical_device_access": False, "default_peripheral_changed": False}
    (output_dir / "profile.json").write_text(json.dumps(report, indent=2) + "\n")
    return output_dir


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path,
                        default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    output = args.output_dir or args.source_dir / "build/usb-host-research/profile"
    try:
        prepared = prepare(args.source_dir, output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"Prepared {prepared}; source only, no USB access, not boot validated.")


if __name__ == "__main__":
    main()
