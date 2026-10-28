#!/usr/bin/env python3
"""Prepare and inspect local UBIFS/UBI files, never format or attach a device.

Geometry is an explicit offline assumption, not detected hardware geometry.
The output remains unsuitable for installation until its NAND backend and
exclusive SLC domain have been validated separately.
"""
import argparse
import hashlib
import json
from pathlib import Path
import stat
import struct
import subprocess
import zlib


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def geometry(data):
    fields = {"purpose", "min_io", "peb_size", "volume_lebs"}
    if not isinstance(data, dict) or set(data) != fields or data["purpose"] != "offline-image-only":
        raise ValueError("geometry must declare purpose=offline-image-only and exact fields")
    for field in fields - {"purpose"}:
        if type(data[field]) is not int:
            raise ValueError(f"{field} must be an integer")
    unit, peb, count = (data[k] for k in ("min_io", "peb_size", "volume_lebs"))
    if not 512 <= unit <= 65536 or unit & (unit - 1):
        raise ValueError("min_io must be a power of two between 512 and 65536")
    if not 8 * unit <= peb <= 16 * 1024 * 1024 or peb % unit:
        raise ValueError("invalid eraseblock size or alignment")
    if not 32 <= count <= 65536 or peb * count > 16 * 1024**3:
        raise ValueError("volume size outside the bounded offline builder range")
    return {**data, "subpage_size": unit, "vid_offset": unit,
            "data_offset": 2 * unit, "leb_size": peb - 2 * unit}


def media_crc(data):
    # Linux crc32_le(0xffffffff, bytes), with no final complement.
    return zlib.crc32(data) ^ 0xffffffff


def inspect_ubifs(path, geo):
    if not path.is_file():
        raise ValueError("UBIFS input must be a regular file")
    with path.open("rb") as source:
        node = source.read(4096)
    if len(node) != 4096:
        raise ValueError("truncated UBIFS superblock")
    magic, crc = struct.unpack_from("<II", node)
    length = struct.unpack_from("<I", node, 16)[0]
    if magic != 0x06101831 or length != 4096 or node[20] != 6:
        raise ValueError("invalid UBIFS superblock identity")
    if media_crc(node[8:]) != crc:
        raise ValueError("UBIFS superblock CRC mismatch")
    unit, leb, used, maximum = struct.unpack_from("<IIII", node, 32)
    if (unit, leb, maximum) != (geo["min_io"], geo["leb_size"], geo["volume_lebs"]):
        raise ValueError("UBIFS geometry mismatch")
    if not 1 <= used <= maximum or path.stat().st_size != used * leb:
        raise ValueError("UBIFS size does not match the superblock")
    return {"min_io": unit, "leb_size": leb, "used_lebs": used,
            "max_lebs": maximum, "superblock_crc_checked": True}


def inspect_ubi(path, geo, ubifs):
    if not path.is_file() or not ubifs.is_file():
        raise ValueError("UBI inspection requires regular input files")
    size = path.stat().st_size
    peb = geo["peb_size"]
    if not size or size % peb or size > (geo["volume_lebs"] + 2) * peb:
        raise ValueError("invalid UBI image size")
    volumes = {0: set(), 0x7fffefff: set()}
    tables = []
    with path.open("rb") as source:
        for index in range(size // peb):
            source.seek(index * peb)
            ec = source.read(64)
            if ec[:5] != b"UBI#\x01" or media_crc(ec[:60]) != struct.unpack_from(">I", ec, 60)[0]:
                raise ValueError(f"invalid UBI erase-counter header at PEB {index}")
            vid_offset, data_offset = struct.unpack_from(">II", ec, 16)
            if (vid_offset, data_offset) != (geo["vid_offset"], geo["data_offset"]):
                raise ValueError("UBI header offset mismatch")
            if struct.unpack_from(">Q", ec, 8)[0] or struct.unpack_from(">I", ec, 24)[0] != 1:
                raise ValueError("unexpected initial erase counter or image sequence")
            source.seek(index * peb + vid_offset)
            vid = source.read(64)
            if vid[:5] != b"UBI!\x01" or media_crc(vid[:60]) != struct.unpack_from(">I", vid, 60)[0]:
                raise ValueError(f"invalid UBI volume header at PEB {index}")
            volume, lnum = struct.unpack_from(">II", vid, 8)
            if volume not in volumes or lnum in volumes[volume] or vid[5] != 1:
                raise ValueError("unexpected volume identity, type or duplicate LEB")
            if vid[6] or vid[7] != (5 if volume else 0) or any(vid[16:60]):
                raise ValueError("unexpected initial UBI volume flags or reserved fields")
            volumes[volume].add(lnum)
            source.seek(index * peb + data_offset)
            if volume != 0:
                table = source.read(128 * 172)
                for offset in range(0, len(table), 172):
                    record = table[offset:offset + 172]
                    if len(record) != 172 or media_crc(record[:168]) != struct.unpack_from(">I", record, 168)[0]:
                        raise ValueError("UBI volume table CRC mismatch")
                if len(table) != 128 * 172:
                    raise ValueError("truncated UBI volume table")
                tables.append(table)
            else:
                expected_offset = lnum * geo["leb_size"]
                with ubifs.open("rb") as payload:
                    payload.seek(expected_offset)
                    expected = payload.read(geo["leb_size"])
                if len(expected) != geo["leb_size"] or source.read(len(expected)) != expected:
                    raise ValueError("UBI payload differs from its UBIFS input")
    used = ubifs.stat().st_size // geo["leb_size"]
    if volumes[0] != set(range(used)) or volumes[0x7fffefff] != {0, 1}:
        raise ValueError("incomplete rootfs or duplicated layout volume")
    if tables[0] != tables[1]:
        raise ValueError("UBI volume tables differ")
    empty = bytes(168) + struct.pack(">I", media_crc(bytes(168)))
    if tables[0][172:] != empty * 127:
        raise ValueError("unexpected additional UBI volume reservation")
    first = tables[0][:172]
    reserved, alignment, padding = struct.unpack_from(">III", first)
    name_len = struct.unpack_from(">H", first, 14)[0]
    if (reserved, alignment, padding, first[12], first[13], name_len,
        first[16:22], first[144]) != (geo["volume_lebs"], 1, 0, 1, 0, 6, b"rootfs", 0):
        raise ValueError("unexpected rootfs volume reservation or attributes")
    return {"pebs": size // peb, "header_crcs_checked": True,
            "both_volume_tables_checked": True, "all_ubifs_payload_checked": True}


def rootfs_digest(root):
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        mode = path.lstat().st_mode
        name = str(path.relative_to(root)).encode()
        if stat.S_ISREG(mode):
            value = sha256(path).encode()
        elif stat.S_ISLNK(mode):
            value = str(path.readlink()).encode()
        elif stat.S_ISDIR(mode):
            value = b"directory"
        else:
            raise ValueError("rootfs must not contain device nodes, sockets or FIFOs")
        metadata = f"{mode}:{path.lstat().st_mtime_ns}".encode()
        digest.update(name + b"\0" + metadata + b"\0" + value + b"\0")
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rootfs", type=Path, required=True)
    parser.add_argument("--geometry", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="new directory; existing paths are never overwritten")
    parser.add_argument("--mkfs", type=Path, required=True)
    parser.add_argument("--ubinize", type=Path, required=True)
    args = parser.parse_args()
    try:
        root = args.rootfs.resolve(strict=True)
        if not root.is_dir() or root == Path("/"):
            raise ValueError("rootfs must be a non-root directory")
        if any(p == root or p in root.parents for p in map(Path, ("/dev", "/proc", "/sys"))):
            raise ValueError("rootfs must be an ordinary filesystem directory")
        if not args.geometry.resolve(strict=True).is_file():
            raise ValueError("geometry input must be a regular file")
        output = args.output_dir.parent.resolve(strict=True) / args.output_dir.name
        if output == root or root in output.parents:
            raise ValueError("output must be outside the rootfs tree")
        geo = geometry(json.loads(args.geometry.read_text()))
        tools = [p.resolve(strict=True) for p in (args.mkfs, args.ubinize)]
        if not all(p.is_file() for p in tools):
            raise ValueError("tool paths must be regular files")
        for tool in tools:
            version = subprocess.check_output([str(tool), "--version"], timeout=10, text=True)
            if version.strip() != f"{tool.name} (mtd-utils) 2.3.0":
                raise ValueError("this builder requires reviewed mtd-utils 2.3.0")
        before = rootfs_digest(root)
        output.mkdir(mode=0o700)
        with (output / "build.log").open("x") as log:
            subprocess.run([str(tools[0]), "-r", str(root), "-m", str(geo["min_io"]),
                            "-e", str(geo["leb_size"]), "-c", str(geo["volume_lebs"]),
                            "-x", "zlib", "-U", "-o", "rootfs.ubifs"],
                           cwd=output, stdout=log, stderr=log, timeout=300, check=True)
            config = ("[rootfs]\nmode=ubi\nimage=rootfs.ubifs\nvol_id=0\n"
                      "vol_type=dynamic\nvol_name=rootfs\nvol_alignment=1\n"
                      f"vol_size={geo['volume_lebs'] * geo['leb_size']}\n")
            (output / "ubinize.ini").write_text(config)
            subprocess.run([str(tools[1]), "-p", str(geo["peb_size"]),
                            "-m", str(geo["min_io"]), "-s", str(geo["subpage_size"]),
                            "-O", str(geo["vid_offset"]), "-Q", "1",
                            "-o", "rootfs.ubi", "ubinize.ini"],
                           cwd=output, stdout=log, stderr=log, timeout=300, check=True)
        if rootfs_digest(root) != before:
            raise ValueError("rootfs changed during image creation")
        report = {"purpose": "offline-image-only", "hardware_validated": False,
                  "device_operations": False, "geometry": geo, "rootfs_tree_sha256": before,
                  "builder_source_sha256": sha256(Path(__file__).resolve()),
                  "validation_scope": "media headers and complete UBI payload; no mount or power-cut test",
                  "tools": {p.name: sha256(p) for p in tools},
                  "ubifs": inspect_ubifs(output / "rootfs.ubifs", geo),
                  "ubi": inspect_ubi(output / "rootfs.ubi", geo, output / "rootfs.ubifs"),
                  "files": {name: {"size": (output / name).stat().st_size,
                                   "sha256": sha256(output / name)}
                            for name in ("rootfs.ubifs", "rootfs.ubi")}}
        (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"UBIFS image not validated: {error}\n")


if __name__ == "__main__":
    main()
