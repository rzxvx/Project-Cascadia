import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.io.FileWriter;

public class FindVramInit extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        PrintWriter out = new PrintWriter(new FileWriter("/tmp/vram_init.txt"));

        // Ищем функции которые пишут в DAT_8033daec (адрес vram-указателя)
        // и DAT_8031d558/DAT_8031d55c (размеры)
        var funcIter = currentProgram.getListing().getFunctions(true);
        while (funcIter.hasNext()) {
            Function func = funcIter.next();
            DecompileResults result = decompiler.decompileFunction(func, 30, new ConsoleTaskMonitor());
            if (!result.decompileCompleted()) continue;

            String code = result.getDecompiledFunction().getC();
            // Ищем функции, которые инициализируют vram указатель
            if ((code.contains("8033daec") || code.contains("8031d558") || 
                 code.contains("8031d55c")) && 
                (code.contains("0x9") || code.contains("vram") || 
                 code.contains("PE_init") || code.contains("display"))) {
                out.println("\n=== " + func.getName() + " @ " + func.getEntryPoint() + " ===");
                out.println(code);
                println("НАЙДЕНО: " + func.getName() + " @ " + func.getEntryPoint());
            }
        }
        out.close();
        decompiler.dispose();
        println("=== ГОТОВО: /tmp/vram_init.txt ===");
    }
}
