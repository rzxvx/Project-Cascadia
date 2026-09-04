#!/usr/bin/env python3
"""
Генерирует несколько вариантов обёртки вокруг zImage для эмпирического
тестирования на железе — на случай, если load_macho_image ожидает не
просто Mach-O, а ещё и complzss-заголовок поверх (или вместо) него.

Использование:
    python3 generate_wrapper_variants.py zImage output_dir/
"""

import struct
import sys
import os

LOAD_ADDRESS = 0x80000000

# ---------- Mach-O часть (как раньше) ----------

MH_MAGIC = 0xfeedface
CPU_TYPE_ARM = 12
MH_EXECUTE = 2
LC_SEGMENT = 0x1
LC_UNIXTHREAD = 0x5
VM_PROT_ALL = 0x7
ARM_THREAD_STATE = 1
ARM_THREAD_STATE_COUNT = 17


def build_macho(payload: bytes, cpusubtype: int) -> bytes:
    header_size = 28
    seg_cmd_size = 56
    thread_cmd_size = 8 + 4 + 4 + ARM_THREAD_STATE_COUNT * 4
    ncmds = 2
    sizeofcmds = seg_cmd_size + thread_cmd_size
    total_header = header_size + sizeofcmds
    entry_vmaddr = LOAD_ADDRESS + total_header

    mach_header = struct.pack("<7I", MH_MAGIC, CPU_TYPE_ARM, cpusubtype,
                               MH_EXECUTE, ncmds, sizeofcmds, 0)

    segname = b"__TEXT".ljust(16, b"\x00")
    seg_cmd = struct.pack("<2I16s8I", LC_SEGMENT, seg_cmd_size, segname,
                           LOAD_ADDRESS, total_header + len(payload),
                           0, total_header + len(payload),
                           VM_PROT_ALL, VM_PROT_ALL, 0, 0)

    state = [0] * ARM_THREAD_STATE_COUNT
    state[15] = entry_vmaddr
    thread_cmd = struct.pack("<4I", LC_UNIXTHREAD, thread_cmd_size,
                              ARM_THREAD_STATE, ARM_THREAD_STATE_COUNT) + \
                 struct.pack(f"<{ARM_THREAD_STATE_COUNT}I", *state)

    return mach_header + seg_cmd + thread_cmd + payload


# ---------- complzss-заголовок ----------

COMPLZSS_HEADER_SIZE = 0x180  # стандартный полный размер заголовка Apple

def build_complzss_header(payload: bytes, compressed: bool = False) -> bytes:
    """
    Если compressed=False — "паспорт-заглушка": CompressedSize == UncompressedSize,
    сигнализируя (в некоторых реализациях) что сжатия нет, данные как есть.
    """
    comp_type = 0x636f6d70  # "comp"
    signature = 0x6c7a7373  # "lzss"
    checksum = 0  # не знаем алгоритм, пробуем 0 — многие реализации не всегда строго его проверяют
    uncompressed_size = len(payload)
    compressed_size = len(payload)  # passthrough — считаем "сжатый" размер равным реальному

    header = struct.pack("<6I", comp_type, signature, checksum,
                          uncompressed_size, compressed_size, 0)
    padding = b"\x00" * (COMPLZSS_HEADER_SIZE - len(header))
    return header + padding


def main():
    if len(sys.argv) != 3:
        print(f"Использование: {sys.argv[0]} <zImage> <output_dir>")
        sys.exit(1)

    with open(sys.argv[1], "rb") as f:
        zimage = f.read()

    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    variants = {}

    # Вариант 1: обычный Mach-O, cpusubtype=V7 (то, что уже пробовали — для контроля)
    variants["variant1_macho_v7.bin"] = build_macho(zimage, cpusubtype=9)

    # Вариант 2: Mach-O с cpusubtype=ALL(0) вместо V7
    variants["variant2_macho_all.bin"] = build_macho(zimage, cpusubtype=0)

    # Вариант 3: complzss-заголовок (passthrough) + голый zImage (БЕЗ Mach-O внутри)
    variants["variant3_complzss_raw.bin"] = build_complzss_header(zimage) + zimage

    # Вариант 4: complzss-заголовок (passthrough) + Mach-O(zImage) внутри
    macho_v7 = build_macho(zimage, cpusubtype=9)
    variants["variant4_complzss_macho.bin"] = build_complzss_header(macho_v7) + macho_v7

    for name, data in variants.items():
        path = os.path.join(outdir, name)
        with open(path, "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} байт")

    print(f"\nВсе варианты сохранены в {outdir}/")
    print("Пробуй по очереди через irecovery -f <файл> && bootx, начиная с variant3 или variant4")
    print("(они основаны на более новой гипотезе про compression-заголовок)")


if __name__ == "__main__":
    main()
