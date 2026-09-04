#!/usr/bin/env python3
"""
Вариант 4 показал прогресс — complzss+Mach-O проходит структурную проверку,
но падает, скорее всего, на checksum. Генерируем варианты с корректным Adler32.

Использование:
    python3 generate_v4_checksums.py zImage output_dir/
"""

import struct
import sys
import os
import zlib

LOAD_ADDRESS = 0x80000000
COMPLZSS_HEADER_SIZE = 0x180

MH_MAGIC = 0xfeedface
CPU_TYPE_ARM = 12
MH_EXECUTE = 2
LC_SEGMENT = 0x1
LC_UNIXTHREAD = 0x5
VM_PROT_ALL = 0x7
ARM_THREAD_STATE = 1
ARM_THREAD_STATE_COUNT = 17


def build_macho(payload: bytes, cpusubtype: int = 9) -> bytes:
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


def build_complzss(payload: bytes, checksum: int) -> bytes:
    comp_type = 0x636f6d70  # "comp"
    signature = 0x6c7a7373  # "lzss"
    uncompressed_size = len(payload)
    compressed_size = len(payload)  # passthrough — данные не сжаты

    header = struct.pack("<6I", comp_type, signature, checksum,
                          uncompressed_size, compressed_size, 0)
    padding = b"\x00" * (COMPLZSS_HEADER_SIZE - len(header))
    return header + padding + payload


def main():
    if len(sys.argv) != 3:
        print(f"Использование: {sys.argv[0]} <zImage> <output_dir>")
        sys.exit(1)

    with open(sys.argv[1], "rb") as f:
        zimage = f.read()

    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    macho = build_macho(zimage, cpusubtype=9)

    # Считаем checksum от разных частей — не знаем, что именно считает Apple
    adler_macho = zlib.adler32(macho) & 0xFFFFFFFF
    adler_macho_0 = zlib.adler32(macho, 0) & 0xFFFFFFFF  # начальное значение 0
    adler_zimage = zlib.adler32(zimage) & 0xFFFFFFFF       # может быть от zImage, а не Mach-O

    variants = {
        # Вариант А: checksum = adler32(Mach-O payload), начальное значение 1 (стандарт zlib)
        "v4a_complzss_macho_adler_from1.bin":
            build_complzss(macho, adler_macho),

        # Вариант Б: checksum = adler32(Mach-O payload), начальное значение 0
        "v4b_complzss_macho_adler_from0.bin":
            build_complzss(macho, adler_macho_0),

        # Вариант В: checksum = adler32(zImage), — вдруг считается от "сырого" содержимого
        "v4c_complzss_macho_adler_zimage.bin":
            build_complzss(macho, adler_zimage),

        # Вариант Г: checksum = 0 (как раньше, для контроля — вдруг checksum вообще не проверяется)
        "v4d_complzss_macho_zero_checksum.bin":
            build_complzss(macho, 0),
    }

    for name, data in variants.items():
        path = os.path.join(outdir, name)
        with open(path, "wb") as f:
            f.write(data)

        # Определяем какой checksum использован для наглядности
        if "zero" in name:
            cs = "0x00000000"
        elif "from1" in name:
            cs = f"0x{adler_macho:08x}"
        elif "from0" in name:
            cs = f"0x{adler_macho_0:08x}"
        else:
            cs = f"0x{adler_zimage:08x}"

        print(f"{name}: {len(data)} байт, checksum={cs}")

    print(f"\nВсе варианты в {outdir}/")
    print("Пробуй по очереди — смотри, изменится ли сообщение об ошибке!")
    print("Если любой из них даст что-то кроме 'error loading kernelcache' — это прогресс")


if __name__ == "__main__":
    main()
