// Ghidra Java-скрипт: декомпилирует КАЖДУЮ функцию в программе
// и пишет результат в один большой текстовый файл.
// Так мы не зависим от того, нашла ли Ghidra автоматические xref'ы —
// нужный код просто найдём через grep по ключевым словам локально.

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.io.FileWriter;

public class DumpAllFunctions extends GhidraScript {

    @Override
    public void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        String outPath = "/tmp/ibec_all_functions.txt";
        PrintWriter out = new PrintWriter(new FileWriter(outPath));

        var funcIter = currentProgram.getListing().getFunctions(true);
        int count = 0;

        while (funcIter.hasNext()) {
            Function func = funcIter.next();
            count++;

            out.println("\n\n=== Функция: " + func.getName() + " @ " + func.getEntryPoint() + " ===");

            DecompileResults result = decompiler.decompileFunction(func, 30, new ConsoleTaskMonitor());
            if (result.decompileCompleted()) {
                out.println(result.getDecompiledFunction().getC());
            } else {
                out.println("  (декомпиляция не удалась: " + result.getErrorMessage() + ")");
            }
        }

        out.close();
        decompiler.dispose();

        println("=== ГОТОВО: обработано " + count + " функций ===");
        println("=== Результат сохранён в: " + outPath + " ===");
    }
}
