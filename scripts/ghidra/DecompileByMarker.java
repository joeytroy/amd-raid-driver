// Find string by exact contains-match, then decompile every function that
// references it.
//
// Args: <outdir> <marker_string>
//
//@category AMD-RAID
//@runtime Java

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;

public class DecompileByMarker extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args == null || args.length < 2) {
            println("usage: <outdir> <marker>");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();
        String marker = args[1];

        Address strAddr = null;
        DataIterator di = currentProgram.getListing().getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            Object v = d.getValue();
            if (v instanceof String && ((String) v).contains(marker)) {
                strAddr = d.getMinAddress();
                println("found '" + marker + "' at " + strAddr);
                break;
            }
        }
        if (strAddr == null) {
            println("marker not found: " + marker);
            return;
        }

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        LinkedHashSet<Function> seen = new LinkedHashSet<>();
        ReferenceIterator refs =
            currentProgram.getReferenceManager().getReferencesTo(strAddr);
        while (refs.hasNext()) {
            Reference ref = refs.next();
            Function f = getFunctionContaining(ref.getFromAddress());
            if (f != null) seen.add(f);
        }

        for (Function f : seen) {
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
