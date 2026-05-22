// Quick-and-dirty: decompile functions at user-specified addresses (and the
// functions they call, 1 level deep) to a target dir.
//
// Args: <outdir> <addr1> [<addr2> ...]
//
//@category AMD-RAID
//@runtime Java

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;

public class DecompileByAddr extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args == null || args.length < 2) {
            println("usage: <outdir> <addr1> [<addr2> ...]");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        LinkedHashSet<Function> work = new LinkedHashSet<>();
        for (int i = 1; i < args.length; i++) {
            Address a = currentProgram.getAddressFactory().getAddress(args[i]);
            Function f = getFunctionAt(a);
            if (f == null) {
                f = getFunctionContaining(a);
            }
            if (f == null) {
                println("no function at " + args[i]);
                continue;
            }
            work.add(f);
            for (Function callee : f.getCalledFunctions(new ConsoleTaskMonitor())) {
                work.add(callee);
            }
        }

        for (Function f : work) {
            DecompileResults r = decomp.decompileFunction(f, 60,
                                    new ConsoleTaskMonitor());
            String c = r.getDecompiledFunction() != null ?
                       r.getDecompiledFunction().getC() :
                       "// decompile failed";
            File outF = new File(outDir,
                f.getName().replaceAll("[^A-Za-z0-9_.-]", "_") + "_"
                + f.getEntryPoint() + ".c");
            try (PrintWriter pw = new PrintWriter(outF)) {
                pw.println("// " + f.getName() + " @ " + f.getEntryPoint());
                pw.print(c);
            }
            println("wrote " + outF);
        }
    }
}
