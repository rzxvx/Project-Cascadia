#!/usr/bin/env python3
"""Describe a partial firmware-derived USB PHY sequence, never execute it.

Requires the exact P105 12H321 kernelcache and ADT. This has no register/USB
access backend, no SSH, no deploy mode and no power controller implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import dtparse

KC_SHA256 = "db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c"
ADT_SHA256 = "5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72"
PHY_VA = 0x80CAB000
VTABLE_VA = 0x80CAD0A8


def macho(data, base=0):
    if base < 0 or base + 28 > len(data):
        raise ValueError("Mach-O header bounds invalid")
    magic, _, _, _, count, total, _ = struct.unpack_from("<7I", data, base)
    if magic != 0xFEEDFACE or count > 4096 or base + 28 + total > len(data):
        raise ValueError("unsupported or truncated 32-bit Mach-O")
    commands = []
    offset = base + 28
    end = offset + total
    for _ in range(count):
        if offset + 8 > end:
            raise ValueError("truncated Mach-O command")
        command, size = struct.unpack_from("<2I", data, offset)
        if size < 8 or offset + size > end:
            raise ValueError("Mach-O command size invalid")
        commands.append((command, offset, size))
        offset += size
    if offset != end:
        raise ValueError("Mach-O command lengths do not consume header")
    return commands


def address_offset(data, address):
    """Map a kernelcache VA through its top-level LC_SEGMENT, not guess math."""
    for command, offset, size in macho(data):
        if command != 1:
            continue
        if size < 56:
            raise ValueError("LC_SEGMENT too short")
        va, memory_size, file_offset, file_size = struct.unpack_from("<4I", data, offset + 24)
        if va <= address < va + memory_size:
            displacement = address - va
            if displacement >= file_size or file_offset + displacement >= len(data):
                raise ValueError("requested VA is not backed by file bytes")
            return file_offset + displacement
    raise ValueError(f"VA {address:#x} is outside mapped file segments")


def symbol_address(data, name):
    for command, offset, size in macho(data):
        if command != 2:
            continue
        if size < 24:
            raise ValueError("LC_SYMTAB too short")
        symbols, count, strings, string_size = struct.unpack_from("<4I", data, offset + 8)
        if count > 1000000 or symbols + count * 12 > len(data) or strings + string_size > len(data):
            raise ValueError("symbol table bounds invalid")
        for index in range(count):
            string_index, _, _, _, value = struct.unpack_from("<IBBHI", data, symbols + index * 12)
            if string_index >= string_size:
                raise ValueError("symbol string index out of bounds")
            start = strings + string_index
            end = data.find(b"\0", start, strings + string_size)
            if end < 0:
                raise ValueError("unterminated symbol name")
            if data[start:end] == name.encode("ascii"):
                return value
    raise ValueError(f"required firmware symbol {name} missing")


def pic_literal_pointer(data, load, add, register):
    """Decode the audited Thumb LDR-literal/ADD-PC pair without executing it."""
    instruction = struct.unpack_from("<H", data, address_offset(data, load))[0]
    addition = struct.unpack_from("<H", data, address_offset(data, add))[0]
    if instruction & 0xf800 != 0x4800 or (instruction >> 8) & 7 != register:
        raise ValueError("expected Thumb LDR literal in audited initializer")
    if addition != 0x4478 | register:
        raise ValueError("expected Thumb ADD PC in audited initializer")
    literal = ((load + 4) & ~3) + (instruction & 0xff) * 4
    value = struct.unpack_from("<I", data, address_offset(data, literal))[0]
    return (value + add + 4) & 0xffffffff


def cable_role_evidence(data):
    # A symbol creation call returns r0; the next iteration stores that result.
    # Therefore each string pair belongs to the following global store, not
    # the store just before its creation call. Verify both pairs and STR r0.
    entries = (("Detached", 0x804bbe72, 0x804bbe76, 0x804bbe80, 0x804bbe84, 2, 0x804bbe88, 0x804daed8, 1),
               ("USBHost", 0x804bbe82, 0x804bbe86, 0x804bbe90, 0x804bbe94, 2, 0x804bbe98, 0x804daedc, 3),
               ("USBHostAltConfig", 0x804bbe92, 0x804bbe96, 0x804bbea0, 0x804bbea4, 2, 0x804bbea8, 0x804daee0, 3),
               ("USBDevice", 0x804bbea2, 0x804bbea6, 0x804bbeb0, 0x804bbeb4, 2, 0x804bbeb8, 0x804daee4, 5),
               ("USBDeviceNeedsAuthentication", 0x804bbeb2, 0x804bbeb6, 0x804bbec2, 0x804bbeca, 3, 0x804bbed2, 0x804daee8, 5))
    result = []
    for name, string_load, string_add, global_load, global_add, reg, store, expected_global, criteria in entries:
        string_va = pic_literal_pointer(data, string_load, string_add, 1)
        string_offset = address_offset(data, string_va)
        if not data[string_offset:].startswith(name.encode("ascii") + b"\0"):
            raise ValueError("audited cable symbol string mismatch")
        global_va = pic_literal_pointer(data, global_load, global_add, reg)
        if global_va != expected_global:
            raise ValueError("audited cable symbol global mismatch")
        instruction = struct.unpack_from("<H", data, address_offset(data, store))[0]
        if instruction != 0x6000 | (reg << 3):
            raise ValueError("expected STR r0 in audited symbol initializer")
        result.append({"apple_cable_type": name, "symbol_global": hex(global_va),
                       "initializer_store": hex(store), "string_va": hex(string_va),
                       "publication_criteria": hex(criteria),
                       "usb_complex_control_bit0": (criteria >> 2) & 1,
                       "ipad_role": "host" if criteria & 4 else
                                    ("peripheral" if criteria & 2 else "detached"),
                       "role_usable_or_vbus_verified": False})
    return {"cable_types": result, "criteria_method": "0x80864454",
            "usb_complex_physical_base_from_adt": "0x3f108000",
            "control_offset": "0x0", "selector_mask": "0x1",
            "mapped_base_store": "0x80b399f8", "selector_write": "0x80b3a238..0x80b3a24c",
            "caveat": "firmware mux semantics only; this does not enable VBUS, authenticate an accessory, or prove a Linux host boot"}


def accessory_rail_evidence(data):
    table = pic_literal_pointer(data, 0x80ece7d0, 0x80ece7da, 0)
    if table != 0x80ed2ec8:
        raise ValueError("audited D1946 LDO table pointer mismatch")
    rails = []
    for index, function in ((5, "function-acc_pwr"), (22, "function-acc_sleep_pwr")):
        offset = address_offset(data, table + 12 * index)
        raw = data[offset:offset + 12]
        if len(raw) != 12:
            raise ValueError("truncated D1946 LDO entry")
        minimum, step, codes, voltage_mask, shared_mask, voltage_reg, enable_reg, enable_mask, _ = struct.unpack("<H4BHHBB", raw)
        rails.append({"adt_dock_function": function, "ldo_table_index": index,
                      "raw_entry_hex": raw.hex(), "voltage_minimum_firmware_units": minimum,
                      "voltage_maximum_firmware_units": minimum + step * codes,
                      "voltage_step_firmware_units": step,
                      "voltage_register": hex(voltage_reg), "voltage_mask": hex(voltage_mask),
                      "enable_register": hex(enable_reg), "enable_mask": hex(enable_mask),
                      "optional_shared_register": "0x7c", "shared_mask": hex(shared_mask),
                      "is_verified_5v_vbus_source": False,
                      "caveat": "static dispatch/table evidence; no measured rail or power-enable permission"})
    return {"set_ldo_method": "0x80ece79c", "ldo_function_call": "0x80ecf4a0",
            "ldo_table": hex(table), "entry_size": 12, "rails": rails,
            "important": "LDO 5 maximum 3300 firmware voltage units is not a 5 V source. Adapter-side conversion or external power requires separate evidence."}


def tristar_evidence(data):
    actual = pic_literal_pointer(data, 0x808869fa, 0x808869fc, 0) + 8
    if actual != 0x80889298:
        raise ValueError("audited THS7383 instance vtable mismatch")
    methods = ((0x370, 0x808870a1, "interrupt_dispatch"),
               (0x37c, 0x80886ba9, "read_system_id"),
               (0x380, 0x80886be9, "read_id_response"),
               (0x388, 0x80886cb1, "status_index"),
               (0x390, 0x80886d2d, "data_routing_fourcc"))
    for slot, target, _ in methods:
        value = struct.unpack_from("<I", data, address_offset(data, actual + slot))[0]
        if value != target:
            raise ValueError("audited THS7383 instance dispatch mismatch")
    # Verified linked import: MOVW ip,#0x6a8; MOVT ip,#0; ADD ip,pc;
    # LDR ip,[ip]; BX ip at 0x8088798c -> pointer 0x80888040.
    import_pointer = struct.unpack_from("<I", data, address_offset(data, 0x80888040))[0]
    if import_pointer != 0x807139b9:
        raise ValueError("audited Tristar ID report import mismatch")
    builtin_vtable = pic_literal_pointer(data, 0x80885368, 0x8088536a, 0) + 8
    bridge = struct.unpack_from("<I", data, address_offset(data, builtin_vtable + 0x36c))[0]
    if builtin_vtable != 0x80888998 or bridge != 0x8088586d:
        raise ValueError("audited BuiltIn USBd bridge mismatch")
    roles = cable_role_evidence(data)["cable_types"]
    transitions = [{"from": "THS7383 IRQ register 0x0d bit 0x08",
                    "to": "read IDResponse via 0x80885160",
                    "status": "firmware dispatch verified"},
                   {"from": "valid seven-byte register reply with status bit7 and marker 0x75",
                    "to": "six reversed bytes reported to IOAccessoryManager 0x807139b8",
                    "status": "firmware dispatch and synthetic execution verified"},
                   {"from": "six accessory ID bytes / IOAccessoryManager properties",
                    "to": "AppleCableType symbol selection",
                    "status": "UNRESOLVED: no camera-adapter ID mapping established"}]
    transitions += [{"from": role["apple_cable_type"],
                     "to": {"criteria": role["publication_criteria"],
                            "usb_complex_control_bit0": role["usb_complex_control_bit0"],
                            "ipad_role": role["ipad_role"]},
                     "status": "firmware consumer verified; live operation unverified"}
                    for role in roles]
    transitions += [{"from": "external host controller publication and data routing",
                     "to": "peripheral VBUS source, source budget and protection",
                     "status": "UNRESOLVED: neither role publication nor LDO5 proves 5 V VBUS"}]
    return {"actual_personality_from_adt": "AppleTHS7383 (ths7383/cbtl1608)",
            "instance_vtable": hex(actual), "metaclass_vtable_not_interchangeable": "0x80889658",
            "methods": [{"slot": hex(s), "target_thumb": hex(t), "description": n}
                        for s, t, n in methods],
            "id_response": {"register": "0x39", "register_read_bytes": 7,
                            "status_register": "0x0c", "required_status_mask": "0x80",
                            "last_register_byte_marker": "0x75", "output": "first six register bytes reversed",
                            "crc_check_visible_in_this_reader": False,
                            "wire_packet_layout_or_authentication_proven": False,
                            "not_ready_return": "0xe00002d9", "bad_marker_return": "0xe00002c6"},
            "irq_dispatch": [{"mask": "0x04", "target": "0x80885110", "meaning": "connection status notification, exact ID/API semantics unresolved"},
                             {"mask": "0x02", "target": "0x80885154", "meaning": "set dock flag+0xf8 and notify manager"},
                             {"mask": "0x08", "target": "0x80885160", "meaning": "read/report accessory IDResponse"},
                             {"mask": "0x01", "target": "0x808851fc", "meaning": "provider callback+0x3dc; exact power meaning unresolved"}],
            "id_report": {"tristar_import_stub": "0x8088798c", "pointer": "0x80888040",
                          "ioaccessory_manager_target": hex(import_pointer & ~1),
                          "six_byte_property_consumer": "0x807133d8"},
            "usb_dock_bridge": {"builtin_constructor": "0x80885350",
                                "instance_vtable": hex(builtin_vtable), "slot": "0x36c",
                                "target_thumb": hex(bridge), "adt_function": "function-usb_dock_control -> charger USBd",
                                "handshake_owner_chain": "ID IRQ -> 0x807139b8 -> 0x80714028 with same BuiltIn owner",
                                "success_or_debug_allow_call": "0x80714260, argument 0x80000",
                                "verification_failure_call": "0x80714cee, argument 0x100000",
                                "charger_api_consumer": "0x804c70e4, flag group 0x1c0000 at 0x804c7224",
                                "input_charge_permission_states": [{"flag": "0x40000", "source_inhibit": 1, "handshake_failure": 0},
                                                                   {"flag": "0x80000", "source_inhibit": 0, "handshake_failure": 0},
                                                                   {"flag": "0x100000", "source_inhibit": 1, "handshake_failure": 1}],
                                "inhibit_budget_consumer": "0x804c8200: inhibited port contributes zero before remaining limits",
                                "permission_for_outgoing_vbus": False,
                                "arguments_are_literal_hardware_register_writes": False,
                                "complete_power_permission_policy_verified": False},
            "data_routing_modes": ["usb0", "usb1", "urt0", "urt1", "urt2", "jtag", "mbus"],
            "accessory_id_to_routing": {"data_mode_decoder": "0x80713c84",
                                        "accessory_mode_decoder": "0x80713dc4",
                                        "normal_cable_fixture_id": "100c00000000",
                                        "normal_cable_data_modes": ["usb0", None],
                                        "platform_predicate_stub": "0x8071d56c (synthetic 0/1 in offline experiment)",
                                        "camera_adapter_fixture_known": False,
                                        "fourcc_is_not_apple_cable_type": True},
            "data_routing_modes_are_adapter_ids": False,
            "camera_adapter_id_known": False, "hardware_access": False,
            "state_transitions": transitions}


def phy_values(adt):
    node, end = dtparse.parse_node(adt, 0)
    if end != len(adt):
        raise ValueError("unconsumed ADT data")
    matches = []
    arm_io = []
    def walk(current):
        if current["props"].get("name", b"").rstrip(b"\0") == b"otgphyctrl":
            matches.append(current["props"])
        if current["props"].get("name", b"").rstrip(b"\0") == b"arm-io":
            arm_io.append(current["props"])
        for child in current["children"]:
            walk(child)
    walk(node)
    if len(matches) != 1:
        raise ValueError("expected one OTG PHY in ADT")
    values = {}
    for key in ("uotgtune1-device", "uotgtune2-device", "uotgtune1-host", "uotgtune2-host"):
        value = matches[0].get(key, b"")
        if len(value) != 4:
            raise ValueError(f"malformed ADT tuning property {key}")
        values[key] = struct.unpack("<I", value)[0]
    if len(arm_io) != 1:
        raise ValueError("expected one arm-io node in ADT")
    return values, "hsic-enabled" in arm_io[0]


def observed_host_sequence(values, enable):
    mode = "host" if enable else "device"
    return [
        {"operation": "write32", "offset": "0x40", "value": hex(values[f"uotgtune1-{mode}"]),
         "instruction": "0x80cac264"},
        {"operation": "write32", "offset": "0x44", "value": hex(values[f"uotgtune2-{mode}"]),
         "instruction": "0x80cac280"},
        {"operation": "or32", "offset": "0x8", "mask": "0x1", "instruction": "0x80cac29a"},
        {"operation": "delay_us", "value": 20, "condition": "object powerLevel >= 2", "instruction": "0x80cac2a2"},
        {"operation": "and32", "offset": "0x8", "mask": "0xfffffffe",
         "condition": "object powerLevel >= 2", "instruction": "0x80cac2b4"},
        {"operation": "delay_us", "value": 1000, "condition": "object powerLevel >= 2", "instruction": "0x80cac2ba"},
        {"operation": "or32" if enable else "and32", "offset": "0x64",
         "mask": "0x2" if enable else "0xfffffffd",
         "condition": "AppleUSBPhy hsic_enabled at object+0x50 == 0", "instruction": "0x80cac306"},
    ]


def plan(kernelcache, adt):
    if hashlib.sha256(kernelcache).hexdigest() != KC_SHA256:
        raise ValueError("kernelcache SHA256 mismatch; only audited P105 12H321 accepted")
    if hashlib.sha256(adt).hexdigest() != ADT_SHA256:
        raise ValueError("ADT SHA256 mismatch; only audited P105 12H321 accepted")
    phy_offset = address_offset(kernelcache, PHY_VA)
    macho(kernelcache, phy_offset)
    helpers = []
    for slot, target, operation in ((0x360, 0x80CAC33D, "read32"),
                                    (0x364, 0x80CAC335, "write32"),
                                    (0x368, 0x80CAC351, "or32"),
                                    (0x36C, 0x80CAC345, "and32")):
        pointer = struct.unpack_from("<I", kernelcache, address_offset(kernelcache, VTABLE_VA + slot))[0]
        if pointer != target:
            raise ValueError("audited PHY register helper vtable mismatch")
        helpers.append({"slot": hex(slot), "target_thumb": hex(pointer), "operation": operation,
                        "base": "object+0x78, mapped otgphyctrl registers"})
    delay = symbol_address(kernelcache, "_IODelay")
    delay_pointer = struct.unpack_from("<I", kernelcache, address_offset(kernelcache, 0x80CAD000))[0]
    if (delay_pointer & ~1) != delay:
        raise ValueError("PHY delay stub does not resolve to IODelay")
    values, hsic_property = phy_values(adt)
    return {"status": "partial firmware evidence; NOT a usable host-power implementation",
            "executable": False, "device_access": False, "vbus_enable_known": False,
            "source": {"build": "12H321", "kernelcache_sha256": KC_SHA256,
                       "adt_sha256": ADT_SHA256, "phy_va": hex(PHY_VA),
                       "phy_file_offset": hex(phy_offset), "host_method": "0x80cac200",
                       "IODelay_symbol": hex(delay)},
            "controller_role_evidence": cable_role_evidence(kernelcache),
            "accessory_rail_evidence": accessory_rail_evidence(kernelcache),
            "tristar_identification_evidence": tristar_evidence(kernelcache),
            "hsic_state": {"adt_hsic_enabled_property_present": hsic_property,
                           "firmware_bootarg_hsic_default": 1,
                           "object_field": "+0x50", "base_start": "0x8086180c",
                           "property_lookup": "0x80861898", "flag_store": "0x808618ac",
                           "default_firmware_skips_register_0x64_bit1": hsic_property,
                           "live_state_verified": False},
            "register_helpers": helpers, "adt_tuning": {k: hex(v) for k, v in values.items()},
            "preconditions": ["Apple object powerLevel > 0",
                              "firmware locking and provider clock callbacks also occur",
                              "HSIC flag is firmware object state; no Linux field may be substituted blindly",
                              "EHCI/OTG data routing and Lightning accessory role must be proven"],
            "observed_enable_host": observed_host_sequence(values, True),
            "observed_disable_host": observed_host_sequence(values, False),
            "unresolved": ["VBUS source/protection", "Tristar/accessory routing",
                           "full initial power-up/return-to-device state machine",
                           "hardware validation; no known-safe executable sequence"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernelcache", type=Path)
    parser.add_argument("adt", type=Path)
    args = parser.parse_args()
    try:
        result = plan(args.kernelcache.read_bytes(), args.adt.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"error: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
