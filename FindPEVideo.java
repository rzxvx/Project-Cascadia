import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.PrintWriter;
import java.io.FileWriter;

public class FindPEVideo extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        PrintWriter out = new PrintWriter(new FileWriter("/tmp/pe_video.txt"));

        String[] targets = {"PE_init", "initialize_screen", "PE_Video",
                            "video_scroll", "video_clear", "vc_progress"};

        var iter = currentProgram.getListing().getFunctions(true);
        while (iter.hasNext()) {
            Function f = iter.next();
            String name = f.getName();
            boolean match = false;
            for (String t : targets)
                if (name.contains(t)) { match = true; break; }
            if (!match) continue;

            DecompileResults r = dec.decompileFunction(f, 60, new ConsoleTaskMonitor());
            if (!r.decompileCompleted()) continue;
            String code = r.getDecompiledFunction().getC();
            out.println("\n=== " + name + " @ " + f.getEntryPoint() + " ===");
            out.println(code);
            println(">> " + name + " @ " + f.getEntryPoint());
        }
        out.close();
        dec.dispose();
        println("ГОТОВО: /tmp/pe_video.txt");
    }
}
