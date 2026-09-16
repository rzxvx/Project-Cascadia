// FindTouchPower.java — locate & decompile the touch power-up path in the
// kernelcache: AppleSamsungSPIController::start (SPI1 pins + clock request),
// AppleARMPWM Cmwp/grape-clk handler, AppleMultitouchSPI.
// kext symbols are stripped, so we locate functions by XREF to their log/ADT
// strings, with a literal-pool memory-scan fallback and Thumb forcing.
// Output: /tmp/touch_power_dump.txt
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class FindTouchPower extends GhidraScript {

    // substrings that live inside/near the functions we want
    String[] TARGETS = {
        "_spiBaseAddress",          // AppleSamsungSPIController::start
        "AppleSamsungSPIController",
        "AppleARMPWM",              // grape-clk / Cmwp provider
        "function-clock_enable",    // the Cmwp ADT function
        "AppleMultitouchSPI",       // Z2 driver
        "multi-touch,p105",
        "grape"
    };

    PrintWriter out;
    DecompInterface decomp;
    Set<Long> dumped = new HashSet<Long>();

    public void run() throws Exception {
        out = new PrintWriter(new FileWriter("/tmp/touch_power_dump.txt"));
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        // 1) find defined strings that contain a target substring
        List<Address> strAddrs = new ArrayList<Address>();
        Listing lst = currentProgram.getListing();
        DataIterator di = lst.getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            for (String t : TARGETS) {
                if (s.contains(t)) {
                    strAddrs.add(d.getAddress());
                    log("STRING @" + d.getAddress() + " : " + s.substring(0, Math.min(70, s.length())));
                    break;
                }
            }
        }
        log("total target strings: " + strAddrs.size());

        // 2) references TO each string -> containing function -> decompile
        ReferenceManager rm = currentProgram.getReferenceManager();
        for (Address sa : strAddrs) {
            ReferenceIterator ri = rm.getReferencesTo(sa);
            boolean any = false;
            while (ri.hasNext()) {
                any = true;
                dumpFuncAt(ri.next().getFromAddress(), "xref->" + sa);
            }
            if (!any) log("  no ghidra xref to " + sa + " (memscan will try)");
        }

        // 3) fallback: scan memory for a 4-byte LE pointer == string address
        //    (Thumb literal pool). The function holding that word is our target.
        Memory mem = currentProgram.getMemory();
        for (Address sa : strAddrs) {
            long a = sa.getOffset();
            byte[] pat = new byte[] {
                (byte)(a & 0xff), (byte)((a >> 8) & 0xff),
                (byte)((a >> 16) & 0xff), (byte)((a >> 24) & 0xff)
            };
            Address from = currentProgram.getMinAddress();
            int found = 0;
            while (from != null && found < 8) {
                Address hit = mem.findBytes(from, pat, null, true, new ConsoleTaskMonitor());
                if (hit == null) break;
                dumpFuncAt(hit, "litpool->" + sa);
                found++;
                from = hit.add(1);
            }
        }

        out.close();
        decomp.dispose();
        println("=== DONE -> /tmp/touch_power_dump.txt (" + dumped.size() + " funcs) ===");
    }

    void log(String s) { println(s); out.println("# " + s); out.flush(); }

    void dumpFuncAt(Address a, String why) {
        try {
            Function f = getFunctionContaining(a);
            if (f == null) {
                // try to make it code (Thumb) then re-find
                try { disassemble(a); } catch (Exception e) {}
                f = getFunctionContaining(a);
            }
            if (f == null) {
                out.println("\n### ref " + why + " at " + a + " -> NO FUNCTION (navigate here in GUI)");
                out.flush();
                return;
            }
            long key = f.getEntryPoint().getOffset();
            if (dumped.contains(key)) return;
            dumped.add(key);
            DecompileResults dr = decomp.decompileFunction(f, 60, new ConsoleTaskMonitor());
            out.println("\n\n===== " + f.getName() + " @ " + f.getEntryPoint()
                        + "   (via " + why + ") =====");
            if (dr != null && dr.decompileCompleted())
                out.println(dr.getDecompiledFunction().getC());
            else
                out.println("(decompile failed; disassembly may need Thumb force at "
                            + f.getEntryPoint() + ")");
            out.flush();
            println(">>> dumped " + f.getName() + " @ " + f.getEntryPoint() + " (" + why + ")");
        } catch (Exception e) {
            out.println("\n### error at " + a + ": " + e);
            out.flush();
        }
    }
}
