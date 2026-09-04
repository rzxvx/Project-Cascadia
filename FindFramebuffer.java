import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.io.FileWriter;

public class FindFramebuffer extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        PrintWriter out = new PrintWriter(new FileWriter("/tmp/framebuffer_search.txt"));

        var funcIter = currentProgram.getListing().getFunctions(true);
        while (funcIter.hasNext()) {
            Function func = funcIter.next();
            String name = func.getName();
            if (!name.contains("CLCD") && !name.contains("Framebuffer") && 
                !name.contains("display") && !name.contains("vram")) continue;

            DecompileResults result = decompiler.decompileFunction(func, 30, new ConsoleTaskMonitor());
            if (!result.decompileCompleted()) continue;

            String code = result.getDecompiledFunction().getC();
            if (code.contains("0x9") || code.contains("vram") || code.contains("frame")) {
                out.println("\n=== " + func.getName() + " @ " + func.getEntryPoint() + " ===");
                out.println(code);
                println("НАЙДЕНО: " + func.getName() + " @ " + func.getEntryPoint());
            }
        }
        out.close();
        decompiler.dispose();
        println("=== ГОТОВО, результат в /tmp/framebuffer_search.txt ===");
    }
}
