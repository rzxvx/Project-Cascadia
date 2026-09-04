import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;

import java.math.BigInteger;

public class ForceThumbDecompile extends GhidraScript {

    // ближайшие кандидаты снизу к нашему адресу (0x84034156), по возрастанию удалённости
    long[] CANDIDATES = {
        0x84033fecL,
        0x84033fdcL,
        0x84033fb0L
    };

    long BLOCK_LEN = 0x400; // с запасом, чтобы захватить всю функцию

    @Override
    public void run() throws Exception {
        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        for (long candidateAddr : CANDIDATES) {
            Address start = toAddr(candidateAddr);
            Address end = start.add(BLOCK_LEN);

            println("\n\n########## Кандидат @ " + start + " ##########");

            try {
                // 1. Чистим предыдущий (неверный ARM) анализ в этом диапазоне
                //    ВАЖНО: делать это ДО смены контекстного регистра,
                //    иначе Ghidra ругается на конфликт с существующими инструкциями
                clearListing(start, end);

                // 2. Помечаем регион как Thumb в контексте процессора
                currentProgram.getProgramContext().setValue(tmode, start, end, BigInteger.ONE);

                // 3. Дизассемблируем заново, уже в Thumb-режиме
                disassemble(start);

                // 4. Пытаемся создать функцию с этой точки
                Function func = createFunction(start, null);
                if (func == null) {
                    println("Не удалось создать функцию по этому адресу, пропускаем");
                    continue;
                }

                println("Функция создана: " + func.getName() + " @ " + func.getEntryPoint());

                // 5. Декомпилируем
                DecompileResults result = decompiler.decompileFunction(func, 30, new ConsoleTaskMonitor());
                if (result.decompileCompleted()) {
                    String code = result.getDecompiledFunction().getC();
                    println(code);

                    if (code.contains("valid") || code.contains("macho") || code.contains("Mach")) {
                        println(">>> ЭТА ФУНКЦИЯ СОДЕРЖИТ ИНТЕРЕСУЮЩИЕ КЛЮЧЕВЫЕ СЛОВА! <<<");
                    }
                } else {
                    println("Декомпиляция не удалась: " + result.getErrorMessage());
                }

            } catch (Exception e) {
                println("Ошибка при обработке кандидата " + start + ": " + e.getMessage());
            }
        }

        decompiler.dispose();
        println("\n=== ГОТОВО ===");
    }
}
