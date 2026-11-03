import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;

import java.math.BigInteger;
import java.io.PrintWriter;
import java.io.FileWriter;

public class FixThumbRegionAndDump extends GhidraScript {

    // Диапазон вокруг найденных Thumb-прологов и нашей строки "Kernelcache image not valid"
    long RANGE_START = 0x8403a000L;
    long RANGE_END   = 0x8403d000L; // с запасом вокруг 0x8403b1ae

    @Override
    public void run() throws Exception {
        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        Address start = toAddr(RANGE_START);
        Address end = toAddr(RANGE_END);

        println("=== Чистим и помечаем как Thumb: " + start + " - " + end + " ===");
        clearListing(start, end);
        currentProgram.getProgramContext().setValue(tmode, start, end, BigInteger.ONE);
        disassemble(start);

        println("=== Запускаем полный повторный автоанализ ===");
        analyzeAll(currentProgram);

        println("=== Анализ завершён, декомпилируем функции в диапазоне и ищем совпадения ===");

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        String outPath = "/tmp/ibec_thumb_region_dump.txt";
        PrintWriter out = new PrintWriter(new FileWriter(outPath));

        var funcIter = currentProgram.getListing().getFunctions(start, true);
        int total = 0;
        int matches = 0;

        while (funcIter.hasNext()) {
            Function func = funcIter.next();
            if (func.getEntryPoint().getOffset() > RANGE_END) break;

            total++;
            DecompileResults result = decompiler.decompileFunction(func, 30, new ConsoleTaskMonitor());
            if (!result.decompileCompleted()) continue;

            String code = result.getDecompiledFunction().getC();
            out.println("\n\n=== " + func.getName() + " @ " + func.getEntryPoint() + " ===");
            out.println(code);

            if (code.contains("valid") || code.contains("macho") || code.contains("Mach") ||
                code.contains("Kernelcache") || code.contains("feedface") || code.contains("0xfeedface")) {
                matches++;
                println("\n>>>>> СОВПАДЕНИЕ: " + func.getName() + " @ " + func.getEntryPoint() + " <<<<<");
                println(code);
            }
        }

        out.close();
        decompiler.dispose();

        println("\n=== ГОТОВО: обработано " + total + " функций в диапазоне, совпадений: " + matches + " ===");
        println("=== Полный дамп региона сохранён в: " + outPath + " ===");
    }
}
