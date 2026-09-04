#!/usr/bin/env python3
"""
Оборачивает Linux zImage в минимальный Mach-O (ARM32, MH_EXECUTE)
с LC_SEGMENT + LC_UNIXTHREAD, чтобы пройти структурную проверку
load_macho_image() в iBEC (используется командой bootx).

ВАЖНО: это experimental best-effort реализация на основе публично
задокументированного формата Mach-O (не реверс конкретно твоего
iBEC). Если не сработает с первой попытки — следующий шаг будет
реверс load_macho_image в Ghidra, чтобы увидеть точные условия
проверки именно в твоей версии iBoot (1537.9.55).

Использование:
    python3 wrap_macho.py zImage kernel.macho
"""

import struct
import sys

# --- Константы Mach-O (ARM32) ---
MH_MAGIC        = 0xfeedface
CPU_TYPE_ARM    = 12
CPU_SUBTYPE_V7  = 9
MH_EXECUTE      = 2

LC_SEGMENT      = 0x1
LC_UNIXTHREAD   = 0x5

VM_PROT_ALL     = 0x7  # read+write+execute

# ARM_THREAD_STATE (flavor=1), count = 17 регистров (r0-r12, sp, lr, pc, cpsr)
ARM_THREAD_STATE       = 1
ARM_THREAD_STATE_COUNT = 17

LOAD_ADDRESS = 0x80000000  # база RAM для A5 (подтверждено из devicetree + логов bootx)


def build_macho(payload: bytes) -> bytes:
    header_size = 28              # mach_header
    seg_cmd_size = 56             # segment_command (без секций)
    thread_cmd_size = 8 + 4 + 4 + ARM_THREAD_STATE_COUNT * 4  # cmd+cmdsize+flavor+count+state[17]

    ncmds = 2
    sizeofcmds = seg_cmd_size + thread_cmd_size
    total_header = header_size + sizeofcmds

    # payload (реальный zImage) начинается сразу после всех заголовков
    entry_vmaddr = LOAD_ADDRESS + total_header

    # --- mach_header ---
    mach_header = struct.pack(
        "<7I",
        MH_MAGIC,
        CPU_TYPE_ARM,
        CPU_SUBTYPE_V7,
        MH_EXECUTE,
        ncmds,
        sizeofcmds,
        0  # flags
    )

    # --- LC_SEGMENT (покрывает весь файл целиком, включая заголовки) ---
    segname = b"__TEXT".ljust(16, b"\x00")
    seg_cmd = struct.pack(
        "<2I16s8I",
        LC_SEGMENT,
        seg_cmd_size,
        segname,
        LOAD_ADDRESS,                       # vmaddr
        total_header + len(payload),        # vmsize
        0,                                   # fileoff
        total_header + len(payload),        # filesize
        VM_PROT_ALL,                         # maxprot
        VM_PROT_ALL,                         # initprot
        0,                                    # nsects
        0                                     # flags
    )

    # --- LC_UNIXTHREAD (ARM thread state, r0-r12/sp/lr/pc/cpsr) ---
    # Все регистры 0, кроме PC (entry point)
    state = [0] * ARM_THREAD_STATE_COUNT
    state[15] = entry_vmaddr  # pc — 16-й регистр в ARM_THREAD_STATE (индекс 15)

    thread_cmd = struct.pack(
        "<4I",
        LC_UNIXTHREAD,
        thread_cmd_size,
        ARM_THREAD_STATE,
        ARM_THREAD_STATE_COUNT
    ) + struct.pack(f"<{ARM_THREAD_STATE_COUNT}I", *state)

    return mach_header + seg_cmd + thread_cmd + payload


def main():
    if len(sys.argv) != 3:
        print(f"Использование: {sys.argv[0]} <zImage> <output.macho>")
        sys.exit(1)

    with open(sys.argv[1], "rb") as f:
        payload = f.read()

    macho = build_macho(payload)

    with open(sys.argv[2], "wb") as f:
        f.write(macho)

    print(f"Готово: {sys.argv[2]} ({len(macho)} байт)")
    print(f"Entry point (PC) будет установлен на 0x{LOAD_ADDRESS + 28 + 56 + (8+4+4+ARM_THREAD_STATE_COUNT*4):08x}")


if __name__ == "__main__":
    main()
