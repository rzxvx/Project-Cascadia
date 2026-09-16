#!/usr/bin/env python3
"""
Создаёт complzss payload с LZSS-сжатым Mach-O вокруг zImage.
Основан на реальном дизассемблере LoadImage_kernelcache_img3 из iBoot-636.66.

Цепочка проверок в iBEC:
  1. MEMZ_STRUCT_INIT(0x80000000)
  2. size < 0xF00000
  3. image_load('krnl')
  4. data[0:4] == 'comp'  (big-endian)
  5. data[4:8] == 'lzss'  (big-endian)
  6. LZSS decompress(data + 0x180, compressed_size)
  7. adler32(decompressed) == header.checksum
  8. decompressed[0:4] == 0xFEEDFACE  (Mach-O magic)

Использование:
    python3 create_lzss_payload.py zImage output.complzss

Затем:
    irecovery -f output.complzss
    irecovery -c bootx
"""

import struct, sys, zlib

# ─── Mach-O константы ────────────────────────────────────────────────────────

LOAD_ADDRESS           = 0x80000000
MH_MAGIC               = 0xfeedface
CPU_TYPE_ARM           = 12
CPU_SUBTYPE_V7         = 9
MH_EXECUTE             = 2
LC_SEGMENT             = 0x1
LC_UNIXTHREAD          = 0x5
VM_PROT_ALL            = 0x7
ARM_THREAD_STATE       = 1
ARM_THREAD_STATE_COUNT = 17

COMPLZSS_HEADER_SIZE   = 0x180

# ─── LZSS константы (совпадают с XNU bsd/kern/lzss.c) ───────────────────────

N          = 4096   # размер кольцевого буфера (степень двойки)
F          = 18     # максимальная длина совпадения
THRESHOLD  = 2      # минимум для кодирования ссылкой: THRESHOLD+1 = 3
MAX_CHAIN  = 32     # максимум кандидатов из хэш-таблицы


def build_macho(payload: bytes) -> bytes:
    """Оборачивает payload в минимальный Mach-O (ARM32, MH_EXECUTE)."""
    hdr_sz       = 28
    seg_sz       = 56
    thread_sz    = 8 + 4 + 4 + ARM_THREAD_STATE_COUNT * 4
    ncmds        = 2
    sizeofcmds   = seg_sz + thread_sz
    total_hdr    = hdr_sz + sizeofcmds
    entry_vmaddr = LOAD_ADDRESS + total_hdr

    mach_hdr = struct.pack("<7I",
        MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_V7,
        MH_EXECUTE, ncmds, sizeofcmds, 0)

    segname = b"__TEXT".ljust(16, b"\x00")
    seg_cmd = struct.pack("<2I16s8I",
        LC_SEGMENT, seg_sz, segname,
        LOAD_ADDRESS, total_hdr + len(payload),
        0,           total_hdr + len(payload),
        VM_PROT_ALL, VM_PROT_ALL, 0, 0)

    state = [0] * ARM_THREAD_STATE_COUNT
    state[15] = entry_vmaddr   # PC = entry point
    thread_cmd = (
        struct.pack("<4I",
            LC_UNIXTHREAD, thread_sz,
            ARM_THREAD_STATE, ARM_THREAD_STATE_COUNT) +
        struct.pack(f"<{ARM_THREAD_STATE_COUNT}I", *state))

    return mach_hdr + seg_cmd + thread_cmd + payload


def lzss_compress(data: bytes) -> bytes:
    """
    LZSS-компрессия, совместимая с decompress_lzss() из XNU bsd/kern/lzss.c.

    Формат ссылки (2 байта):
        byte1 = position & 0xFF
        byte2 = ((position >> 8) & 0xF) << 4 | (length - 3)
    Минимальная длина ссылки = 3 байта (THRESHOLD+1).
    Флаг-бит 1 = литерал, 0 = ссылка (LSB first).
    """
    n    = len(data)
    ring = bytearray(b' ' * N)   # кольцевой буфер, инициализирован пробелами
    r    = N - F                  # позиция записи (совпадает с декодером)

    htab: dict[int, list] = {}   # 3-байтовый ключ → список позиций кольцевого буфера

    def hkey(i: int) -> int:
        return data[i] << 16 | data[i+1] << 8 | data[i+2]

    def htab_add(k: int, pos: int):
        lst = htab.setdefault(k, [])
        lst.append(pos)
        if len(lst) > MAX_CHAIN * 2:
            del lst[:MAX_CHAIN]

    out  = bytearray()
    flag = 0
    fc   = 0
    ibuf = bytearray()

    i         = 0
    last_pct  = -1
    step_log  = max(1, n // 100)

    while i < n:
        if i // step_log != last_pct:
            last_pct = i // step_log
            print(f"\r  LZSS: {i*100//n:3d}%  {i//1024}/{n//1024} KB   ", end='', flush=True)

        avail    = min(F, n - i)
        best_len = THRESHOLD   # должен быть > THRESHOLD чтобы использовать ссылку
        best_pos = 0

        if avail >= 3:
            k = hkey(i)
            cands = htab.get(k)
            if cands:
                for pos in cands[-MAX_CHAIN:]:
                    ml = 0
                    while ml < avail and ring[(pos + ml) & (N-1)] == data[i + ml]:
                        ml += 1
                    if ml > best_len:
                        best_len = ml
                        best_pos = pos
                        if ml >= F:
                            break

        if best_len >= THRESHOLD + 1:   # длина >= 3: кодируем ссылкой
            enc_l = best_len - (THRESHOLD + 1)           # 0..15 → 4 бита
            ibuf.append(best_pos & 0xFF)                  # byte1: low 8 bits позиции
            ibuf.append(((best_pos >> 8) & 0xF) << 4 | enc_l)  # byte2: high 4 bits + длина
            # флаг-бит = 0 (ссылка)

            for k in range(best_len):
                c = data[i + k]
                ring[r] = c
                if i + k + 2 < n:
                    htab_add(hkey(i + k), r)
                r = (r + 1) & (N - 1)
            i += best_len

        else:   # кодируем литералом
            flag |= (1 << fc)
            ibuf.append(data[i])

            ring[r] = data[i]
            if i + 2 < n:
                htab_add(hkey(i), r)
            r = (r + 1) & (N - 1)
            i += 1

        fc += 1
        if fc == 8:
            out.append(flag)
            out.extend(ibuf)
            flag = 0
            fc   = 0
            ibuf = bytearray()

    if fc > 0:
        out.append(flag)
        out.extend(ibuf)

    print(f"\r  LZSS: 100%  {n//1024}/{n//1024} KB   ")
    return bytes(out)


def build_complzss(uncompressed: bytes, compressed: bytes) -> bytes:
    """
    Строит complzss-заголовок (big-endian, 0x180 байт) + compressed.
    adler32 считается от несжатых данных (Mach-O).
    """
    checksum = zlib.adler32(uncompressed, 1) & 0xFFFFFFFF

    hdr = struct.pack(">6I",
        0x636f6d70,        # 'comp'
        0x6c7a7373,        # 'lzss'
        checksum,
        len(uncompressed),
        len(compressed),
        0)                 # version
    pad = b'\x00' * (COMPLZSS_HEADER_SIZE - len(hdr))
    return hdr + pad + compressed


def main():
    if len(sys.argv) != 3:
        print(f"Использование: {sys.argv[0]} <zImage> <output.complzss>")
        sys.exit(1)

    zimage_path, out_path = sys.argv[1], sys.argv[2]

    with open(zimage_path, 'rb') as f:
        zimage = f.read()
    print(f"zImage: {len(zimage)} байт")

    print("Строим Mach-O wrapper...")
    macho = build_macho(zimage)
    print(f"Mach-O: {len(macho)} байт")

    if len(macho) > 0xF00000:
        print(f"ОШИБКА: {len(macho)} байт > 0xF00000 — iBEC отклонит ('Kernelcache too large')")
        sys.exit(1)

    print("LZSS-сжатие (может занять 1-3 минуты для ~10MB файла)...")
    compressed = lzss_compress(macho)
    ratio = len(compressed) * 100 // len(macho)
    print(f"Сжато: {len(compressed)} байт ({ratio}% от оригинала)")

    print("Строим complzss payload...")
    payload = build_complzss(macho, compressed)

    with open(out_path, 'wb') as f:
        f.write(payload)

    print(f"\n✓ Готово: {out_path} ({len(payload)} байт)")
    print(f"\nДальше:")
    print(f"  # (вся стандартная цепочка: primepwn → iBEC → ramdisk → devicetree)")
    print(f"  irecovery -f {out_path}")
    print(f"  irecovery -c bootx")
    print(f"\nЕсли снова 'error loading kernelcache' — попробуем img3maker:")
    print(f"  ./img3maker -f {out_path} -t krnl -s s5l8942x -o kernel.img3")
    print(f"  irecovery -f kernel.img3 && irecovery -c bootx")


if __name__ == '__main__':
    main()
