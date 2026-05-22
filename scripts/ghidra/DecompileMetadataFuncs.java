// Ghidra headless postScript: find functions referencing key metadata
// strings, decompile them, and dump the result so we can locate the
// RCIdent magic value and the LBA RC_ReadMetaData uses.
//
// Usage: analyzeHeadless <proj> <name> -process rcraid.sys -noanalysis \
//          -scriptPath <repo>/scripts/ghidra -postScript DecompileMetadataFuncs.java <out>
//
//@category AMD-RAID
//@runtime Java

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;

public class DecompileMetadataFuncs extends GhidraScript {

    private static final String[] MARKERS = new String[] {
        "RC_CheckMetaData: not valid metatata",
        "RC_CheckMetaData: Checksum not match",
        "RC_ConfigInit: Call RC_ReadMBR",
        "RC_ConfigRescan: Call  RC_ReadMetaData",
        "RC_ConfigRescan: Call  RC_ReadMBR",
        "RC_BuildConfigMetadataFromRing",
        "RC_ClearMetaData:",
        "NVRAM Valid Cookie but invalid size",
    };

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir;
        if (args != null && args.length > 0) {
            outDir = new File(args[0]);
        } else {
            outDir = new File("/tmp/rcraid_metadata_out");
        }
        outDir.mkdirs();

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        LinkedHashSet<Function> seen = new LinkedHashSet<>();
        PrintWriter summary = new PrintWriter(new File(outDir, "summary.txt"));

        for (String marker : MARKERS) {
            Address strAddr = findString(marker);
            if (strAddr == null) {
                summary.println("MARKER NOT FOUND: " + marker);
                continue;
            }
            summary.println("MARKER: " + marker + " @ " + strAddr);

            ReferenceIterator refs =
                currentProgram.getReferenceManager().getReferencesTo(strAddr);
            while (refs.hasNext()) {
                Reference ref = refs.next();
                Function f = getFunctionContaining(ref.getFromAddress());
                if (f != null) {
                    summary.println("  referenced from: " + f.getName()
                                    + " @ " + f.getEntryPoint());
                    seen.add(f);
                }
            }
        }

        summary.println();
        summary.println("DECOMPILED FUNCTIONS:");
        for (Function f : seen) {
            summary.println("  " + f.getName() + " @ " + f.getEntryPoint());
        }
        summary.close();

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

    private Address findString(String needle) {
        DataIterator di =
            currentProgram.getListing().getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            Object v = d.getValue();
            if (v instanceof String && ((String) v).contains(needle)) {
                return d.getMinAddress();
            }
        }
        return null;
    }
}
