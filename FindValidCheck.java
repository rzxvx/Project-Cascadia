// Ghidra Java-скрипт (не требует PyGhidra/Jython)
// Ищет строки, содержащие заданные термины, находит функции,
// которые на них ссылаются, и печатает декомпилированный псевдокод.

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.util.HashSet;
import java.util.Set;

public class FindValidCheck extends GhidraScript {

    String[] SEARCH_TERMS = { "valid", "Kernelcache", "kernelcache" };

    @Override
    public void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        Set<Address> seenFunctions = new HashSet<>();

        println("=== Ищем строки ===");

        var dataIter = currentProgram.getListing().getDefinedData(true);
        while (dataIter.hasNext()) {
            Data data = dataIter.next();
            if (!data.hasStringValue()) continue;

            Object val = data.getValue();
            if (val == null) continue;
            String valStr = val.toString();

            boolean matched = false;
            for (String term : SEARCH_TERMS) {
                if (valStr.contains(term)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) continue;

            Address addr = data.getAddress();
            println("\n--- Найдена строка @ " + addr + ": " + valStr + " ---");

            Reference[] refs = getReferencesTo(addr);
            if (refs.length == 0) {
                println("  (нет ссылок на эту строку)");
                continue;
            }

            for (Reference ref : refs) {
                Address fromAddr = ref.getFromAddress();
                Function func = getFunctionContaining(fromAddr);
                if (func == null) {
                    println("  Ссылка из " + fromAddr + " — вне функции, пропускаем");
                    continue;
                }

                Address funcKey = func.getEntryPoint();
                if (seenFunctions.contains(funcKey)) continue;
                seenFunctions.add(funcKey);

                println("\n=== Функция: " + func.getName() + " @ " + funcKey + " ===");

                DecompileResults result = decompiler.decompileFunction(func, 60, new ConsoleTaskMonitor());
                if (result.decompileCompleted()) {
                    println(result.getDecompiledFunction().getC());
                } else {
                    println("  (декомпиляция не удалась: " + result.getErrorMessage() + ")");
                }
            }
        }

        decompiler.dispose();
        println("\n=== ГОТОВО ===");
    }
}
