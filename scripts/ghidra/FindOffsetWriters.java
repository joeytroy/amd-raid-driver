// Find every function that writes to a specific structure offset
// (looking for "+ 0x3c8" or similar pointer-array setup).
//
// Args: <outdir> <offset_hex>
//
//@category AMD-RAID
//@runtime Java

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;

public class FindOffsetWriters extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args == null || args.length < 2) {
            println("usage: <outdir> <offset_hex>");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();
        String needle = args[1].toLowerCase();
        if (!needle.startsWith("0x")) needle = "0x" + needle;

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        FunctionIterator fi =
            currentProgram.getFunctionManager().getFunctions(true);
        LinkedHashSet<Function> hits = new LinkedHashSet<>();
        int n = 0;
        while (fi.hasNext()) {
            Function f = fi.next();
            n++;
            DecompileResults r = decomp.decompileFunction(f, 30,
                                    new ConsoleTaskMonitor());
            if (r.getDecompiledFunction() == null) continue;
            String c = r.getDecompiledFunction().getC();
            /* match write patterns "= ...; ... + 0x3c8" or "+0x3c8" appearing
             * in assignment LHS. We're not super strict — just look for the
             * offset literal in any function. */
            if (c.toLowerCase().contains(needle)) {
                hits.add(f);
            }
        }
        println("scanned " + n + " functions, " + hits.size() + " contain " + needle);

        for (Function f : hits) {
            DecompileResults r = decomp.decompileFunction(f, 60,
                                    new ConsoleTaskMonitor());
            if (r.getDecompiledFunction() == null) continue;
            String c = r.getDecompiledFunction().getC();
            File outF = new File(outDir,
                f.getName().replaceAll("[^A-Za-z0-9_.-]", "_") + "_"
                + f.getEntryPoint() + ".c");
            try (PrintWriter pw = new PrintWriter(outF)) {
                pw.println("// " + f.getName() + " @ " + f.getEntryPoint());
                pw.print(c);
            }
        }
        println("wrote " + hits.size() + " files to " + outDir);
    }
}
