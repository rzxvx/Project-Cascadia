// Ищем назад от заданного адреса (2-байтовыми шагами) ближайший
// правдоподобный Thumb function prologue: PUSH {..., LR}
// В Thumb PUSH кодируется как 0xB5xx (младший байт = битовая маска регистров,
// старший байт всегда 0xB5 для "PUSH {reglist, LR}")

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

public class FindThumbPrologue extends GhidraScript {

    // адрес, от которого ищем назад (наш "b" из "bootx" в Thumb-режиме)
    long SEARCH_FROM = 0x8403b1aeL;
    int MAX_BACK = 0x2000; // как далеко назад искать (8KB должно с запасом хватить)

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Address startAddr = toAddr(SEARCH_FROM);

        println("=== Ищем Thumb PUSH{...,LR} назад от " + startAddr + " ===");

        int found = 0;
        for (long offset = 0; offset < MAX_BACK; offset += 2) {
            Address addr = startAddr.subtract(offset);
            try {
                byte b0 = mem.getByte(addr);
                byte b1 = mem.getByte(addr.add(1));

                // little-endian: младший байт первый
                // PUSH {reglist, LR} в Thumb: старший байт == 0xB5
                int highByte = b1 & 0xFF;
                if (highByte == 0xB5) {
                    println("Кандидат @ " + addr + "  bytes: " +
                            String.format("%02x %02x", b0 & 0xFF, b1 & 0xFF));
                    found++;
                    if (found >= 15) break; // не заваливать вывод, первые 15 кандидатов
                }
            } catch (Exception e) {
                // вышли за пределы memory block, просто пропускаем
            }
        }

        println("=== Найдено кандидатов: " + found + " ===");
    }
}
