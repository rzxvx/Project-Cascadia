#!/usr/bin/env python3
"""
Патчит iBEC.raw (уже расшифрованный и распакованный из img3, БЕЗ обёртки):
1. Находит запись команды "bootx" в таблице команд (struct iboot32_cmd_t)
2. Дописывает в конец файла крошечный ARM-стаб, который:
   - выставляет r0=0, r1=0xFFFFFFFF (сигнал "грузи по Device Tree"), r2=0
   - прыгает на 0x80000000 (куда irecovery -f кладёт присланный файл)
3. Перенаправляет cmd_ptr команды "bootx" на этот стаб

Использование:
    python3 patch_bootx_handler.py iBEC.raw iBEC.stubpatched
"""

import struct
import sys

BASE_ADDR = 0x84000000       # база загрузки iBEC (подтверждено ранее)
ZIMAGE_STAGE_ADDR = 0x80000000  # куда irecovery -f кладёт присланный файл

def find_cmd_string(data: bytes, cmd: str) -> int:
    """Ищет '\\0<cmd>\\0' и возвращает offset начала самой строки (после первого \\0)."""
    needle = b"\x00" + cmd.encode() + b"\x00"
    idx = data.find(needle)
    if idx == -1:
        raise ValueError(f"Строка команды '{cmd}' не найдена (с обрамляющими \\0)")
    return idx + 1  # +1 чтобы пропустить leading \0, встать на начало самой строки


def find_cmd_table_entry(data: bytes, cmd_str_offset: int) -> int:
    """
    Ищет 4-байтовый little-endian указатель в ДИАПАЗОНЕ вокруг cmd_str_offset
    (на случай смещения на несколько байт из-за особенностей компоновки строк).
    """
    target_addr = BASE_ADDR + cmd_str_offset
    search_range = 32  # +/- 32 байта вокруг найденной строки

    for delta in range(-search_range, search_range + 1):
        candidate_addr = target_addr + delta
        needle = struct.pack("<I", candidate_addr)
        idx = data.find(needle)
        if idx != -1:
            print(f"  (найдено с отклонением {delta:+d} байт от точного адреса строки)")
            return idx

    raise ValueError(
        f"Не найден указатель на строку (адрес 0x{target_addr:08x}, "
        f"проверен диапазон +/-{search_range} байт) — таблица команд не найдена этим методом"
    )


def build_stub() -> bytes:
    """
    ARM (не Thumb) код:
        MOV  R0, #0
        MVN  R1, #0          ; R1 = 0xFFFFFFFF
        MOV  R2, #0
        LDR  PC, [PC, #-4]   ; прыжок по адресу из литерал-пула ниже
        .word ZIMAGE_STAGE_ADDR
    """
    code = b""
    code += struct.pack("<I", 0xE3A00000)  # MOV R0, #0
    code += struct.pack("<I", 0xE3E01000)  # MVN R1, #0
    code += struct.pack("<I", 0xE3A02000)  # MOV R2, #0
    code += struct.pack("<I", 0xE51FF004)  # LDR PC, [PC, #-4]
    code += struct.pack("<I", ZIMAGE_STAGE_ADDR)  # литерал: адрес назначения
    return code


def main():
    if len(sys.argv) != 3:
        print(f"Использование: {sys.argv[0]} <iBEC.raw> <output>")
        sys.exit(1)

    with open(sys.argv[1], "rb") as f:
        data = bytearray(f.read())

    print("=== Ищем строку команды 'bootx' ===")
    cmd_str_offset = find_cmd_string(bytes(data), "bootx")
    print(f"Строка 'bootx' найдена по file-offset 0x{cmd_str_offset:x} "
          f"(адрес 0x{BASE_ADDR + cmd_str_offset:08x})")

    print("=== Ищем запись в таблице команд (ссылку на эту строку) ===")
    table_entry_offset = find_cmd_table_entry(bytes(data), cmd_str_offset)
    print(f"Запись таблицы найдена по file-offset 0x{table_entry_offset:x}")

    old_cmd_ptr = struct.unpack("<I", data[table_entry_offset+4:table_entry_offset+8])[0]
    print(f"Текущий cmd_ptr (адрес cmd_bootx): 0x{old_cmd_ptr:08x}")

    print("=== Дописываем стаб в конец файла ===")
    stub_offset = len(data)
    # выравниваем на 4 байта на всякий случай
    if stub_offset % 4 != 0:
        pad = 4 - (stub_offset % 4)
        data.extend(b"\x00" * pad)
        stub_offset = len(data)

    stub = build_stub()
    data.extend(stub)
    stub_addr = BASE_ADDR + stub_offset
    print(f"Стаб дописан по file-offset 0x{stub_offset:x} (адрес 0x{stub_addr:08x})")

    print("=== Патчим cmd_ptr на адрес нашего стаба ===")
    data[table_entry_offset+4:table_entry_offset+8] = struct.pack("<I", stub_addr)

    with open(sys.argv[2], "wb") as f:
        f.write(data)

    print(f"\nГотово: {sys.argv[2]}")
    print(f"Теперь команда 'bootx' будет прыгать на 0x{stub_addr:08x},")
    print(f"который выставит r0=0, r1=0xFFFFFFFF, r2=0 и прыгнет на 0x{ZIMAGE_STAGE_ADDR:08x}")


if __name__ == "__main__":
    main()
