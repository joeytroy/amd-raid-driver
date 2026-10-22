// Headless postScript: decompile every function and dump it together with
// the strings it references, so functions can be matched across builds
// (see match_builds.py).
//
// Usage: analyzeHeadless <proj> <name> -import <file.sys> -overwrite \
//          -scriptPath <repo>/scripts/ghidra -postScript DumpAll.java <outfile>
//@category AMD-RAID
//@runtime Java

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.address.Address;

import java.io.PrintWriter;
import java.util.TreeSet;

public class DumpAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing listing = currentProgram.getListing();
        try (PrintWriter pw = new PrintWriter(out, "UTF-8")) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                TreeSet<String> strs = new TreeSet<>();
                for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
                    for (Reference r : ins.getReferencesFrom()) {
                        Data d = listing.getDataAt(r.getToAddress());
                        if (d != null && d.hasStringValue()) {
                            Object v = d.getValue();
                            if (v != null) strs.add(v.toString().replace("\n", "\\n"));
                        }
                    }
                }
                pw.println("@@FUNC " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
                for (String s : strs) pw.println("@@STR " + s);
                DecompileResults res = dec.decompileFunction(f, 120, monitor);
                if (res != null && res.decompileCompleted())
                    pw.println(res.getDecompiledFunction().getC());
                else
                    pw.println("/* decompile failed */");
            }
        }
    }
}
