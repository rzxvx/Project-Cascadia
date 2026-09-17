#!/usr/bin/env python3
"""
Convert an Apple iBoot device tree (ADT) blob to Linux DTS source.

Reads the decrypted IMG3 payload (e.g. devicetree.p105.decrypted.v2), parses
Apple's ADT format, applies bus address translation (arm-io ranges and nested
usb-complex ranges), and emits a dtc-compatible .dts file.

Usage:
  python3 adt_to_dts.py devicetree.p105.decrypted.v2 -o ipad2-5-p105.dts
  python3 adt_to_dts.py devicetree.p105.decrypted.v2 --full -o ipad2-5-p105-full.dts
"""

from __future__ import annotations

import argparse
import struct
import sys
from typing import Any

from dtparse import parse_node

# Properties that are Apple/iBoot-specific and not useful in Linux DTS.
SKIP_PROPS = {
    "AAPL,phandle",
    "secure-root-prefix",
    "config-number",
    "model-number",
    "platform-name",
    "mlb-serial-number",
    "region-info",
    "serial-number",
    "regulatory-model-number",
    "firmware-version",
    "nvram-proxy-data",
    "random-seed",
    "root-matching",
    "root-ticket-hash",
    "bootp-response",
    "software-bundle-version",
    "software-behavior",
    "boot-nonce",
    "die-id",
    "consistent-debug-root",
    "gid-aes-key",
    "uid-aes-key",
    "dram-vendor",
    "production-cert",
    "development-cert",
    "populate-registry-time",
    "load-kernel-start",
    "start-time",
    "debug-wait-start",
}

SKIP_PREFIXES = (
    "function-",
    "MemoryMapReserved-",
)

# Nodes omitted from the default Linux-oriented export (still in --full output).
DEFAULT_SKIP_NODES = {
    "flash-controller0",
    "wlan",
    "bluetooth",
    "audio-codec",
    "audio-speaker0",
    "audio-speaker1",
    "audio-data",
    "audio-bluetooth",
    "audio-codec-voice",
    "gpu",
    "sgx",
    "7890803",
    "i2c0",
    "i2c1",
    "i2c2",
    "spi2",
    "uart1",
    "uart2",
    "uart3",
    "uart5",
    "pwm",
    "pke",
    "cdma",
    "sha2",
    "trace",
    "iop",
    "dwi",
    "ae2",
    "i2s-switch",
    "mca0",
    "mca1",
    "jpeg",
    "scaler0",
    "scaler1",
    "vxd",
    "vxe",
    "isp",
    "dart-nrt",
    "dart-rt",
    "perfcounter",
    "camera",
    "backlight",
    "charger",
    "buttons",
    "dock",
    "tristar",
    "product",
    "target-type",
    "compatible-machines",
}

MVP_EXTRA_SKIP_PROPS = {
    "bridge-settings",
    "device-clocks",
    "device-clocks-max",
    "device-clocks-min",
    "ema-settings0",
    "ema-settings1",
    "firmware-m-perf-states",
    "firmware-p-perf-state",
    "firmware-v-perf-states",
    "ipid-mask",
    "target-destinations",
    "pdmvr-rules",
    "performance-domain-features",
    "voltage-states0",
    "voltage-states1",
    "dma-channels",
    "dma-parent",
    "clock-gates",
    "clock-ids",
    "clock-mask",
    "power-gates",
}

LINUX_COMPAT_MAP = {
    "pl310,s5l8940x": "arm,pl310-cache",
    "usb-ehci,s5l8940x": "generic-ehci",
    "usb-ohci,s5l8940x": "generic-ohci",
}


def node_name(node: dict[str, Any]) -> str:
    raw = node["props"].get("name")
    if not raw:
        return "?"
    return raw.split(b"\x00")[0].decode("ascii", errors="replace")


def u32_cells(data: bytes) -> list[int]:
    return [struct.unpack("<I", data[i : i + 4])[0] for i in range(0, len(data), 4)]


def u32_prop(props: dict[str, bytes], key: str, default: int = 0) -> int:
    val = props.get(key)
    if not val:
        return default
    return struct.unpack("<I", val[:4])[0]


def decode_strings(data: bytes) -> list[str]:
    parts = [part.decode("ascii", errors="replace") for part in data.split(b"\x00") if part]
    return parts


def decode_reg(data: bytes, ac: int, sc: int) -> list[tuple[int, ...]]:
    cells = u32_cells(data)
    stride = ac + sc
    if stride <= 0:
        return []
    return [tuple(cells[i : i + stride]) for i in range(0, len(cells), stride) if len(cells[i : i + stride]) == stride]


def decode_ranges(data: bytes, ac: int, sc: int) -> list[tuple[int, int, int]]:
    cells = u32_cells(data)
    if ac == 1 and sc == 1 and len(cells) % 3 == 0:
        return [tuple(cells[i : i + 3]) for i in range(0, len(cells), 3)]  # type: ignore[misc]
    return []


def translate_address(local: int, translations: list[tuple[int, int, int]]) -> int:
    for child_base, parent_base, length in translations:
        if child_base <= local < child_base + length:
            return parent_base + (local - child_base)
    return local


def sanitize_label(name: str) -> str:
    out = []
    for ch in name:
        if ch.isalnum() or ch in "_":
            out.append(ch)
        else:
            out.append("_")
    label = "".join(out)
    if not label or label[0].isdigit():
        label = f"n_{label}"
    return label


def should_skip_prop(name: str, mvp: bool = False) -> bool:
    if name in SKIP_PROPS or name == "name":
        return True
    if mvp and name in MVP_EXTRA_SKIP_PROPS:
        return True
    return any(name.startswith(prefix) for prefix in SKIP_PREFIXES)


def dts_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def format_prop(name: str, value: bytes, ac: int = 1, sc: int = 1, mvp: bool = False) -> str | None:
    if should_skip_prop(name, mvp):
        return None

    if name in ("#address-cells", "#size-cells", "#interrupt-cells", "#gpio-cells", "cache-level"):
        if len(value) >= 4:
            return f"{name} = <{struct.unpack('<I', value[:4])[0]}>;"
        return None

    if name in ("interrupt-controller", "gpio-controller", "cache-unified", "boot-console", "no-flow-control"):
        return f"{name};"

    if name == "compatible":
        strings = decode_strings(value)
        if strings:
            mapped = []
            for item in strings:
                mapped.append(LINUX_COMPAT_MAP.get(item, item))
            quoted = ", ".join(f'"{s}"' for s in mapped)
            return f"compatible = {quoted};"
        return None

    if name == "device_type":
        text = decode_strings(value)
        if text:
            return f"device_type = {dts_string(text[0])};"
        return None

    if name == "model":
        text = decode_strings(value)
        if text:
            return f"model = {dts_string(text[0])};"
        return None

    if name == "reg":
        regs = decode_reg(value, ac, sc)
        if not regs:
            return None
        cells: list[str] = []
        for entry in regs:
            cells.extend(hex(x) for x in entry)
        return f"reg = <{' '.join(cells)}>;"

    if name in ("interrupts", "interrupt-parent", "clocks", "clock-ids", "clock-gates"):
        nums = u32_cells(value)
        if not nums:
            return None
        return f"{name} = <{' '.join(str(n) for n in nums)}>;"

    if name == "status":
        text = decode_strings(value)
        if text:
            return f"status = {dts_string(text[0])};"
        return None

    if len(value) == 4:
        num = struct.unpack("<I", value)[0]
        if value.rstrip(b"\x00") == b"":
            return f"{name} = <0>;"
        stripped = value.rstrip(b"\x00")
        if stripped and all(32 <= b < 127 for b in stripped):
            return f"{name} = {dts_string(stripped.decode('ascii', errors='replace'))};"
        return f"{name} = <{num}>;"

    stripped = value.rstrip(b"\x00")
    if stripped and all(32 <= b < 127 or b == 0 for b in stripped):
        strings = decode_strings(value)
        if len(strings) == 1:
            if len(strings[0]) == 1 and strings[0] in '\\"':
                return None
            return f"{name} = {dts_string(strings[0])};"
        if strings:
            quoted = ", ".join(dts_string(s) for s in strings)
            return f"{name} = {quoted};"

    if not value:
        return f"{name};"

    byte_str = " ".join(f"{b:02x}" for b in value)
    return f"{name} = [{byte_str}];"


class DtsWriter:
    def __init__(self) -> None:
        self.lines: list[str] = []

    def write(self, indent: int, text: str) -> None:
        self.lines.append("\t" * indent + text)

    def dump(self) -> str:
        return "\n".join(self.lines) + "\n"


class AdtToDts:
    def __init__(self, root: dict[str, Any], full: bool = False, mvp: bool = False) -> None:
        self.root = root
        self.full = full
        self.mvp = mvp
        self.writer = DtsWriter()
        self.labels: dict[int, str] = {}
        self.phandle_nodes: dict[int, dict[str, Any]] = {}
        self._collect_phandles(root)

    def _collect_phandles(self, node: dict[str, Any]) -> None:
        ph = node["props"].get("AAPL,phandle")
        if ph and len(ph) >= 4:
            self.phandle_nodes[struct.unpack("<I", ph[:4])[0]] = node
        for child in node["children"]:
            self._collect_phandles(child)

    def find_node(self, target: str) -> dict[str, Any] | None:
        if node_name(self.root) == target:
            return self.root
        for child in self.root["children"]:
            found = self._find_node(child, target)
            if found:
                return found
        return None

    def _find_node(self, node: dict[str, Any], target: str) -> dict[str, Any] | None:
        if node_name(node) == target:
            return node
        for child in node["children"]:
            found = self._find_node(child, target)
            if found:
                return found
        return None

    def label_for(self, node: dict[str, Any], preferred: str | None = None) -> str:
        ph = node["props"].get("AAPL,phandle")
        if ph and len(ph) >= 4:
            phandle = struct.unpack("<I", ph[:4])[0]
            if phandle in self.labels:
                return self.labels[phandle]
        base = sanitize_label(preferred or node_name(node))
        label = base
        suffix = 0
        while label in self.labels.values():
            suffix += 1
            label = f"{base}_{suffix}"
        if ph and len(ph) >= 4:
            self.labels[struct.unpack("<I", ph[:4])[0]] = label
        return label

    def emit_header(self) -> None:
        self.writer.write(0, "/*")
        self.writer.write(0, " * Generated by adt_to_dts.py from Apple iBoot ADT.")
        self.writer.write(0, " * Target: iPad2,5 (P105AP / S5L8942X, Apple A5).")
        self.writer.write(0, " *")
        self.writer.write(0, " * Register addresses under soc@30000000 use arm-io local bus")
        self.writer.write(0, " * addresses (child 0 -> parent 0x30000000, size 0x10000000).")
        self.writer.write(0, " * usb-complex children are flattened through usb-complex ranges")
        self.writer.write(0, " * (child 0 -> 0x06100000, size 0x00600000) before insertion.")
        self.writer.write(0, " *")
        self.writer.write(0, " * memory@0, chosen/linux,initrd-*, and framebuffer values are zero")
        self.writer.write(0, " * in the static ADT; patch them at boot from live iBoot state.")
        self.writer.write(0, " */")
        self.writer.write(0, "")
        self.writer.write(0, "/dts-v1/;")
        self.writer.write(0, "")

    def emit_root(self) -> None:
        dt = self.find_node("device-tree")
        if not dt:
            raise SystemExit("device-tree node not found")

        model = "iPad2,5"
        for key in ("model",):
            if key in dt["props"]:
                parts = decode_strings(dt["props"][key])
                if parts:
                    model = parts[0]

        self.writer.write(0, "/ {")
        self.writer.write(1, 'compatible = "apple,s5l8940x", "apple,ipad2,5";')
        self.writer.write(1, f'model = "Apple {model} (iPad mini Wi-Fi)";')
        self.writer.write(1, "#address-cells = <1>;")
        self.writer.write(1, "#size-cells = <1>;")
        self.writer.write(1, "")

        self.emit_chosen(dt)
        self.emit_memory(dt)
        self.emit_cpus(dt)
        self.emit_pl310(dt)
        self.emit_soc(dt)
        self.writer.write(0, "};")

    def emit_chosen(self, dt: dict[str, Any]) -> None:
        chosen = self._find_node(dt, "chosen")
        self.writer.write(1, "chosen {")
        self.writer.write(2, 'bootargs = "console=ttyAPL0,115200n8 root=/dev/ram0 rw earlyprintk";')
        self.writer.write(2, "stdout-path = &uart0;")
        self.writer.write(2, "linux,initrd-start = <0x0>;")
        self.writer.write(2, "linux,initrd-end = <0x0>;")
        if chosen:
            for key, val in sorted(chosen["props"].items()):
                if key.startswith("mac-address"):
                    text = val.rstrip(b"\x00").hex()
                    if text != "0" * len(text):
                        self.writer.write(2, f"/* ADT {key} = [{text}] */")
        self.writer.write(1, "};")
        self.writer.write(1, "")

    def emit_memory(self, dt: dict[str, Any]) -> None:
        mem = self._find_node(dt, "memory")
        self.writer.write(1, "memory@0 {")
        self.writer.write(2, 'device_type = "memory";')
        if mem and mem["props"].get("reg"):
            cells = u32_cells(mem["props"]["reg"])
            if len(cells) >= 2 and (cells[0] or cells[1]):
                self.writer.write(2, f"reg = <{hex(cells[0])} {hex(cells[1])}>;")
            else:
                self.writer.write(2, "reg = <0x0 0x20000000>; /* TODO: patch at boot; 512 MiB placeholder */")
        else:
            self.writer.write(2, "reg = <0x0 0x20000000>; /* TODO: patch at boot; 512 MiB placeholder */")
        self.writer.write(1, "};")
        self.writer.write(1, "")

    def emit_cpus(self, dt: dict[str, Any]) -> None:
        cpus = self._find_node(dt, "cpus")
        self.writer.write(1, "cpus {")
        self.writer.write(2, "#address-cells = <1>;")
        self.writer.write(2, "#size-cells = <0>;")
        if cpus:
            for idx, cpu in enumerate(cpus["children"]):
                name = node_name(cpu)
                reg = u32_prop(cpu["props"], "reg", idx)
                label = "cpu0" if reg == 0 else None
                prefix = f"{label}: " if label else ""
                self.writer.write(2, f"{prefix}cpu@{reg} {{")
                self.writer.write(3, 'device_type = "cpu";')
                self.writer.write(3, 'compatible = "arm,cortex-a9";')
                self.writer.write(3, f"reg = <{reg}>;")
                if reg != 0:
                    self.writer.write(3, 'status = "disabled";')
                self.writer.write(2, "};")
        self.writer.write(1, "};")
        self.writer.write(1, "")

    def emit_pl310(self, dt: dict[str, Any]) -> None:
        armio = self._find_node(dt, "arm-io")
        if not armio:
            return
        pl310 = self._find_node(armio, "pl310")
        if not pl310:
            return
        regs = decode_reg(pl310["props"]["reg"], 1, 1)
        if not regs:
            return
        addr, size = regs[0]
        phys = translate_address(addr, decode_ranges(armio["props"]["ranges"], 1, 1))
        self.writer.write(1, f"l2_cache: pl310@3e000000 {{")
        self.writer.write(2, 'compatible = "arm,pl310-cache";')
        self.writer.write(2, f"reg = <{hex(phys)} {hex(size)}>;")
        self.writer.write(2, "cache-unified;")
        self.writer.write(2, "cache-level = <2>;")
        if len(regs) > 1:
            addr2, size2 = regs[1]
            phys2 = translate_address(addr2, decode_ranges(armio["props"]["ranges"], 1, 1))
            self.writer.write(2, f"/* secondary ADT range {hex(phys2)}/{hex(size2)} not wired */")
        self.writer.write(1, "};")
        self.writer.write(1, "")

    def emit_soc(self, dt: dict[str, Any]) -> None:
        armio = self._find_node(dt, "arm-io")
        if not armio:
            return

        armio_ranges = decode_ranges(armio["props"]["ranges"], 1, 1)
        self.writer.write(1, "soc: soc@30000000 {")
        self.writer.write(2, 'compatible = "simple-bus";')
        self.writer.write(2, "#address-cells = <1>;")
        self.writer.write(2, "#size-cells = <1>;")
        self.writer.write(2, "ranges = <0x0 0x30000000 0x10000000>;")
        self.writer.write(2, "")

        for child in armio["children"]:
            self.emit_armio_child(child, armio_ranges, indent=2)

        if self.mvp:
            self.emit_mvp_framebuffer()

        self.writer.write(1, "};")
        self.writer.write(1, "")

    def emit_mvp_framebuffer(self) -> None:
        self.writer.write(2, "framebuffer@0 {")
        self.writer.write(3, 'compatible = "simple-framebuffer";')
        self.writer.write(3, "reg = <0x0 0x0>; /* TODO: patch from live iBoot vram */")
        self.writer.write(3, "width = <0>;")
        self.writer.write(3, "height = <0>;")
        self.writer.write(3, "stride = <0>;")
        self.writer.write(3, 'format = "a8r8g8b8";')
        self.writer.write(3, 'status = "disabled";')
        self.writer.write(2, "};")
        self.writer.write(2, "")

    def node_allowed(self, name: str) -> bool:
        if self.full:
            return True
        return name not in DEFAULT_SKIP_NODES

    def emit_armio_child(self, node: dict[str, Any], translations: list[tuple[int, int, int]], indent: int, flatten_usb: bool = False) -> None:
        name = node_name(node)
        if not self.node_allowed(name):
            return

        if name == "usb-complex":
            self.emit_usb_complex(node, translations, indent)
            return

        if name == "pl310":
            return

        if self.mvp and name in ("clcd", "mipi-dsim", "mipi_dsim"):
            return

        ac = u32_prop(node["props"], "#address-cells", 1) or 1
        sc = u32_prop(node["props"], "#size-cells", 1)
        if node["props"].get("reg"):
            reg_cells = len(u32_cells(node["props"]["reg"]))
            if reg_cells == 2:
                ac, sc = 1, 1
            elif sc == 0 and reg_cells >= 2:
                sc = 1
        props = dict(node["props"])

        if self.mvp and name == "pmgr":
            props["status"] = b"disabled\x00"

        if self.mvp and name == "wdt":
            props["status"] = b"disabled\x00"

        if self.mvp and name == "spi1":
            props["#size-cells"] = struct.pack("<I", 0)

        label = self.label_for(node, preferred=name if name not in ("6515041", "7627895", "7174000") else None)
        if name == "6515041":
            label = "aic"
        if name == "uart0":
            label = "uart0"
        if name == "gpio":
            label = "gpio"

        reg_hint = ""
        if props.get("reg"):
            first = u32_cells(props["reg"])[0]
            reg_hint = f"@{first:x}"

        self.writer.write(indent, f"{label}: {sanitize_label(name)}{reg_hint} {{")
        self.emit_properties(node, props, ac, sc, indent + 1, translations)

        child_indent = indent + 1
        for child in node["children"]:
            cname = node_name(child)
            if not self.node_allowed(cname):
                continue
            if name == "spi1" and cname == "multi-touch":
                self.emit_spi_touch(child, indent + 1)
                continue
            self.emit_armio_child(child, translations, child_indent)

        self.writer.write(indent, "};")
        self.writer.write(indent, "")

    def emit_usb_complex(self, node: dict[str, Any], parent_translations: list[tuple[int, int, int]], indent: int) -> None:
        local_ranges = decode_ranges(node["props"]["ranges"], 1, 1)
        complex_base = local_ranges[0][1] if local_ranges else 0

        for child in node["children"]:
            cname = node_name(child)
            if not self.node_allowed(cname):
                continue
            if cname == "usb-device":
                continue

            props = dict(child["props"])
            ac = u32_prop(child["props"], "#address-cells", 1)
            sc = u32_prop(child["props"], "#size-cells", 1)
            if props.get("reg"):
                regs = decode_reg(props["reg"], ac, sc)
                translated = []
                for entry in regs:
                    addr = translate_address(entry[0], local_ranges)
                    translated.append((addr, *entry[1:]))
                packed: list[int] = []
                for entry in translated:
                    packed.extend(entry)
                props["reg"] = b"".join(struct.pack("<I", x) for x in packed)

            label = self.label_for(child, preferred=cname)
            first = u32_cells(props["reg"])[0] if props.get("reg") else 0
            self.writer.write(indent, f"{label}: {sanitize_label(cname)}@{first:x} {{")
            self.emit_properties(child, props, ac, sc, indent + 1, parent_translations)
            self.writer.write(indent, "};")
            self.writer.write(indent, "")

    def emit_spi_touch(self, node: dict[str, Any], indent: int) -> None:
        irq = u32_cells(node["props"].get("interrupts", b""))
        pin = irq[0] if irq else 0
        flags = irq[1] if len(irq) > 1 else 0
        self.writer.write(indent, "touchscreen@0 {")
        self.writer.write(indent + 1, 'compatible = "linux,spidev";')
        self.writer.write(indent + 1, "reg = <0>;")
        self.writer.write(indent + 1, "spi-max-frequency = <1000000>;")
        self.writer.write(indent + 1, "interrupt-parent = <&gpio>;")
        self.writer.write(indent + 1, f"interrupts = <{pin} {flags}>;")
        self.writer.write(indent, "};")

    def emit_properties(
        self,
        node: dict[str, Any],
        props: dict[str, bytes],
        ac: int,
        sc: int,
        indent: int,
        translations: list[tuple[int, int, int]],
    ) -> None:
        priority = [
            "compatible",
            "device_type",
            "reg",
            "ranges",
            "interrupt-parent",
            "interrupts",
            "interrupt-controller",
            "#interrupt-cells",
            "gpio-controller",
            "#gpio-cells",
            "#address-cells",
            "#size-cells",
            "status",
        ]

        emitted: set[str] = set()
        for key in priority:
            if key not in props:
                continue
            line = self.format_node_prop(key, props[key], ac, sc, node)
            if line:
                self.writer.write(indent, line)
            emitted.add(key)

        for key in sorted(props):
            if key in emitted:
                continue
            line = self.format_node_prop(key, props[key], ac, sc, node)
            if line:
                self.writer.write(indent, line)

    def format_node_prop(self, key: str, value: bytes, ac: int, sc: int, node: dict[str, Any]) -> str | None:
        if key == "interrupt-parent" and len(value) >= 4:
            phandle = struct.unpack("<I", value[:4])[0]
            target = self.phandle_nodes.get(phandle)
            if target:
                label = self.label_for(target)
                return f"interrupt-parent = <&{label}>;"
            return f"interrupt-parent = <{phandle}>;"

        if key == "compatible" and not self.full:
            strings = decode_strings(value)
            if not strings:
                return None
            mapped = []
            for item in strings:
                if item.startswith("uart-") or item.startswith("spi-"):
                    mapped.append(f"apple,s5l8940x-{item.split(',')[0]}")
                elif item.startswith("gpio,"):
                    mapped.append("apple,s5l8940x-gpio")
                elif item.startswith("aic,"):
                    mapped.append("apple,s5l8940x-aic")
                elif item.startswith("clcd,"):
                    mapped.append("apple,s5l8940x-clcd")
                elif item.startswith("mipi-dsim"):
                    mapped.append("apple,s5l8940x-mipi-dsim")
                elif item.startswith("otgphyctrl"):
                    mapped.append("apple,s5l8940x-otgphyctrl")
                elif item.startswith("pmgr,"):
                    mapped.append("apple,s5l8940x-pmgr")
                elif item.startswith("wdt,"):
                    mapped.append("apple,s5l8940x-wdt")
                else:
                    mapped.append(LINUX_COMPAT_MAP.get(item, item))
            quoted = ", ".join(f'"{s}"' for s in mapped)
            return f"compatible = {quoted};"

        if key == "pmgr" or node_name(node) == "pmgr":
            if key == "device_type":
                return None

        line = format_prop(key, value, ac, sc, self.mvp)
        if line and key == "reg" and "compatible" not in node["props"]:
            pass
        return line

    def convert(self) -> str:
        self.emit_header()
        self.emit_root()
        return self.writer.dump()


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert Apple ADT blob to Linux DTS")
    parser.add_argument("input", help="Decrypted Apple device tree blob")
    parser.add_argument("-o", "--output", help="Output .dts path (default: stdout)")
    parser.add_argument("--full", action="store_true", help="Include all ADT nodes/properties")
    parser.add_argument("--mvp", action="store_true", help="Boot-focused subset for LXDE MVP")
    args = parser.parse_args()

    with open(args.input, "rb") as fh:
        data = fh.read()

    root, end = parse_node(data, 0)
    if end != len(data):
        print(f"warning: parsed {end:#x} of {len(data):#x} bytes", file=sys.stderr)

    dts = AdtToDts(root, full=args.full, mvp=args.mvp).convert()

    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(dts)
        print(f"Wrote {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(dts)


if __name__ == "__main__":
    main()
