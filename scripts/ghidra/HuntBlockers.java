// Ghidra headless post-script (Java)
// Hunts the specific blockers documented in docs/GHIDRA_ANALYSIS_NEEDED.md:
//   1) Device 0xb000 register-offset code paths
//   2) StorPort service slot indirect calls (call [reg+0xNNN]) bucketed by offset
//   3) devExt offset writes (descriptor accessor install, etc)
//   4) Decompile key documented functions + every function that references 0xb000
//
// Run via:
//   analyzeHeadless <proj-dir> <proj-name> -process rcbottom.sys -noanalysis \
//       -scriptPath <repo>/scripts/ghidra -postScript HuntBlockers.java <out_dir>
//
//@category AMD-RAID
//@runtime Java

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressFactory;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.scalar.Scalar;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

public class HuntBlockers extends GhidraScript {

    private File outDir;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("Usage: HuntBlockers <out_dir>");
            return;
        }
        outDir = new File(args[0]);
        if (!outDir.exists()) outDir.mkdirs();
        println("Output dir: " + outDir.getAbsolutePath());
        println("Program: " + currentProgram.getName());
        println("Image base: " + currentProgram.getImageBase());

        // 1) Device ID constant references
        LinkedHashMap<String, Long> deviceIds = new LinkedHashMap<>();
        deviceIds.put("B000", 0xb000L);
        deviceIds.put("43BD", 0x43bdL);
        deviceIds.put("7905", 0x7905L);
        deviceIds.put("7916", 0x7916L);
        deviceIds.put("7917", 0x7917L);

        Set<Function> devIdFuncs = new HashSet<>();
        for (Map.Entry<String, Long> e : deviceIds.entrySet()) {
            println("Hunting device id " + e.getKey());
            devIdFuncs.addAll(findConstantRefs(e.getValue(), e.getKey()));
        }

        // 2) Indirect call offsets (call [reg+0xNNN])
        println("Bucketing indirect calls...");
        bucketIndirectCalls();

        // 3) devExt offset writes
        println("Tracking devExt offset writes...");
        long[][] offs = {
            {0x1c2d0L, 0}, {0x1c2a0L, 0}, {0x1c2d8L, 0},
            {0x16020L, 0}, {0x16018L, 0}, {0x15920L, 0},
            {0x15928L, 0}, {0x16288L, 0}, {0x1c2e8L, 0},
            {0x16100L, 0}, {0x16168L, 0},
            {0x16056L, 0}, {0x16058L, 0}, {0x1605aL, 0}, {0x1605cL, 0},
            {0x16068L, 0}, {0x1606cL, 0},
        };
        String[] names = {
            "devExt_1c2d0_descAccessor", "devExt_1c2a0_descCtx", "devExt_1c2d8_fwCapWord",
            "devExt_16020_adapterHandle", "devExt_16018", "devExt_15920_qstate",
            "devExt_15928", "devExt_16288", "devExt_1c2e8",
            "devExt_16100_cbTableLo", "devExt_16168_cbTableHi",
            "devExt_16056", "devExt_16058", "devExt_1605a", "devExt_1605c",
            "devExt_16068_qVariant", "devExt_1606c_qVariant2",
        };
        for (int i = 0; i < offs.length; i++) {
            findStructOffsetAccess(offs[i][0], names[i]);
        }

        // 4) Decompile key documented functions
        String[] keyAddrs = {
            // AHCI-path callbacks (iVar7=1, devices 0x7905/0x7916/0x7917/0x43BD)
            "140007d40",   // firmware capability parsing + callback install
            "140008a48",   // queue init / +0x9D8 callsite
            "140007978",   // global completion register prog
            "1400021d4",   // AHCI completion register prog (+0x9D8 caller)
            "14000924c",   // doorbell activation
            "140005ff4",   // adapter init
            "1400067fc",   // adapter object & WMI reg
            "140008f34",   // BAR discovery / MmMapIoSpace
            "140001008",   // vendor mailbox builder
            "140008638",   // doorbell activation caller
            "140003048",   // port disable/quiesce (AHCI)
            "1400028f8",   // port enable/resume (AHCI)
            "140010488",   // MMIO register I/O
            // NVMe-path callbacks (iVar7=2, devices 0xb000 etc with param_3 != 0)
            "14000fafc",   // NVMe +0x16100 (primary queue dispatcher)
            "14000c0bc",   // NVMe +0x16108 (NVMe command submission cmd_type=8)
            "14000dd44",   // NVMe +0x16110 (NVMe spinlock callback init -- replaces FUN_1400021d4)
            "14000e59c",   // NVMe +0x16158 (NVMe cleanup completion)
            "14000e800",   // NVMe +0x16120 (NVMe command submission wrapper)
            "14000e494",   // NVMe +0x16130 (early init stub)
            "14000c814",   // NVMe +0x16138 (NVMe state getter)
            "14000c82c",   // NVMe +0x16140 (NVMe completion check)
            "14000d06c",   // NVMe +0x16148
            "14001023c",   // NVMe +0x16160
            "1400100c0",   // NVMe +0x16168 (NVMe status update)
            "14000f454",   // NVMe per-queue setup (where MMIO writes happen)
            "140001ed8",   // legacy queue bring-up helper (called from FUN_14000924c if iVar7=1)
            "14000be7c",   // AHCI per-port init (called from FUN_1400021d4)
            "14000b730",   // AHCI early init (called from FUN_1400021d4)
            "14000204c",   // AHCI port iteration done callback
            "140006c34",   // event handler 1 (registered by FUN_140005ff4)
            "140006da0",   // event handler 2 (registered by FUN_140005ff4)
        };
        println("Decompiling key documented functions...");
        decompileNamed(keyAddrs, "key_funcs");

        // 5b) Dump the firmware descriptor blob (DAT_140012258)
        try {
            Address blobAddr = currentProgram.getAddressFactory().getAddress("140012258");
            if (blobAddr != null) {
                int dumpLen = 0x800;  // 2 KiB should cover ~40 entries of 0x30 bytes
                List<String> blobLines = new ArrayList<>();
                blobLines.add("# Firmware descriptor blob at 0x140012258 (DAT_140012258)");
                blobLines.add("# Used by StorPort service +0x418 to set up devExt+0x1c2d0 accessor");
                blobLines.add("# Dumping " + dumpLen + " bytes as hex; trailing 00s indicate end");
                blobLines.add("");
                StringBuilder asciiCol = new StringBuilder();
                StringBuilder hexCol = new StringBuilder();
                for (int i = 0; i < dumpLen; i++) {
                    Address a = blobAddr.add(i);
                    byte b;
                    try { b = currentProgram.getMemory().getByte(a); } catch (Exception ex) { b = 0; }
                    if (i % 16 == 0) {
                        if (i > 0) {
                            blobLines.add(String.format("0x%05x: %-48s  %s",
                                    i - 16, hexCol.toString(), asciiCol.toString()));
                        }
                        hexCol.setLength(0);
                        asciiCol.setLength(0);
                    }
                    hexCol.append(String.format("%02x ", b & 0xff));
                    asciiCol.append((b >= 0x20 && b < 0x7f) ? (char) b : '.');
                }
                if (hexCol.length() > 0) {
                    blobLines.add(String.format("0x%05x: %-48s  %s",
                            dumpLen - (dumpLen % 16 == 0 ? 16 : dumpLen % 16),
                            hexCol.toString(), asciiCol.toString()));
                }
                writeLines("firmware_descriptor_blob.txt", blobLines);
            }
        } catch (Exception ex) {
            println("Could not dump descriptor blob: " + ex.getMessage());
        }

        // 5c) Dump the StorPort service table indirection target if statically known.
        // DAT_140014958 / DAT_140014980 — the dispatch table base pointers used everywhere.
        try {
            List<String> tableLines = new ArrayList<>();
            tableLines.add("# StorPort dispatch globals");
            tableLines.add("# DAT_140014958 - service table base pointer");
            tableLines.add("# DAT_140014980 - StorPort context handle (first arg to services)");
            for (String addr : new String[] {"140014958", "140014980", "140014090"}) {
                Address a = currentProgram.getAddressFactory().getAddress(addr);
                if (a == null) { tableLines.add("// " + addr + ": null"); continue; }
                try {
                    long v = currentProgram.getMemory().getLong(a);
                    tableLines.add(String.format("DAT_%s = 0x%016x", addr, v));
                } catch (Exception ex) {
                    tableLines.add("// " + addr + ": read failed: " + ex.getMessage());
                }
            }
            writeLines("storport_globals.txt", tableLines);
        } catch (Exception ex) {
            println("Could not dump storport globals: " + ex.getMessage());
        }

        // 5) Decompile every function that referenced any device ID
        println("Decompiling " + devIdFuncs.size() + " functions that reference device IDs...");
        decompileFunctions(new ArrayList<>(devIdFuncs), "decompiled_devid_funcs");

        println("Done.");
    }

    private Set<Function> findConstantRefs(long value, String name) throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();
        Set<Function> seenFn = new HashSet<>();
        List<String> hits = new ArrayList<>();
        InstructionIterator it = listing.getInstructions(true);
        long total = 0;
        while (it.hasNext()) {
            Instruction inst = it.next();
            total++;
            int n = inst.getNumOperands();
            for (int i = 0; i < n; i++) {
                Object[] objs = inst.getOpObjects(i);
                for (Object o : objs) {
                    if (o instanceof Scalar) {
                        long v = ((Scalar) o).getUnsignedValue();
                        if (v == value) {
                            Function fn = fm.getFunctionContaining(inst.getAddress());
                            String fnName = (fn == null) ? "<nofunc>" : fn.getName();
                            hits.add(String.format("0x%x  %s  in %s",
                                    inst.getAddress().getOffset(), inst.toString(), fnName));
                            if (fn != null) seenFn.add(fn);
                        }
                    }
                }
            }
        }
        List<String> lines = new ArrayList<>();
        lines.add(String.format("### Refs to constant %s (0x%x)", name, value));
        lines.add(String.format("### scanned %d instructions, %d hits, %d funcs",
                total, hits.size(), seenFn.size()));
        lines.add("");
        lines.addAll(hits);
        writeLines("refs_" + name + ".txt", lines);
        return seenFn;
    }

    private void bucketIndirectCalls() throws Exception {
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        TreeMap<Long, List<String>> buckets = new TreeMap<>();
        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction inst = it.next();
            if (!inst.getMnemonicString().equalsIgnoreCase("CALL")) continue;
            int n = inst.getNumOperands();
            for (int i = 0; i < n; i++) {
                Object[] objs = inst.getOpObjects(i);
                // Pattern: register + Scalar (deref) — Ghidra represents memory operand
                // as a list including the base register and the displacement scalar
                Scalar disp = null;
                boolean hasReg = false;
                for (Object o : objs) {
                    if (o instanceof Scalar) {
                        // pick largest scalar that looks like a structure offset
                        long v = ((Scalar) o).getUnsignedValue();
                        if (v >= 0x80 && v <= 0x10000) {
                            disp = (Scalar) o;
                        }
                    } else {
                        hasReg = true;
                    }
                }
                if (hasReg && disp != null) {
                    long off = disp.getUnsignedValue();
                    Function fn = fm.getFunctionContaining(inst.getAddress());
                    String fnName = (fn == null) ? "<nofunc>" : fn.getName();
                    buckets.computeIfAbsent(off, k -> new ArrayList<>())
                            .add(String.format("0x%x  %s  in %s",
                                    inst.getAddress().getOffset(), inst.toString(), fnName));
                }
            }
        }
        List<String> lines = new ArrayList<>();
        lines.add("### Indirect calls bucketed by displacement (StorPort service slot candidates)");
        lines.add(String.format("### %d unique offsets", buckets.size()));
        lines.add("");
        for (Map.Entry<Long, List<String>> e : buckets.entrySet()) {
            lines.add(String.format("-- offset 0x%x (%d hits) --", e.getKey(), e.getValue().size()));
            int max = Math.min(30, e.getValue().size());
            for (int i = 0; i < max; i++) lines.add("  " + e.getValue().get(i));
            lines.add("");
        }
        writeLines("indirect_call_offsets.txt", lines);
    }

    private void findStructOffsetAccess(long targetOffset, String name) throws Exception {
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        List<String> reads = new ArrayList<>();
        List<String> writes = new ArrayList<>();
        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction inst = it.next();
            String mnem = inst.getMnemonicString().toUpperCase();
            if (!mnem.equals("MOV") && !mnem.equals("LEA") && !mnem.equals("CALL")) continue;
            int n = inst.getNumOperands();
            for (int i = 0; i < n; i++) {
                Object[] objs = inst.getOpObjects(i);
                boolean hasReg = false;
                boolean matched = false;
                for (Object o : objs) {
                    if (o instanceof Scalar) {
                        if (((Scalar) o).getUnsignedValue() == targetOffset) matched = true;
                    } else {
                        hasReg = true;
                    }
                }
                if (matched && hasReg) {
                    Function fn = fm.getFunctionContaining(inst.getAddress());
                    String fnName = (fn == null) ? "<nofunc>" : fn.getName();
                    String line = String.format("0x%x op%d  %s  in %s",
                            inst.getAddress().getOffset(), i, inst.toString(), fnName);
                    // crude: operand 0 = destination for MOV; LEA is "read-of-address"; CALL is target
                    if (mnem.equals("MOV") && i == 0) writes.add(line);
                    else reads.add(line);
                }
            }
        }
        List<String> lines = new ArrayList<>();
        lines.add(String.format("### Accesses with displacement 0x%x  (%s)", targetOffset, name));
        lines.add(String.format("### %d writes, %d reads/other", writes.size(), reads.size()));
        lines.add("");
        lines.add("--- WRITES (likely install/store) ---");
        lines.addAll(writes);
        lines.add("");
        lines.add("--- READS / LEA / CALL through this offset ---");
        lines.addAll(reads);
        writeLines("offset_" + name + ".txt", lines);
    }

    private void decompileNamed(String[] addrHexes, String label) throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try {
            FunctionManager fm = currentProgram.getFunctionManager();
            AddressFactory af = currentProgram.getAddressFactory();
            List<String> lines = new ArrayList<>();
            for (String h : addrHexes) {
                Address a;
                try {
                    a = af.getAddress(h);
                } catch (Exception ex) {
                    lines.add("// " + h + ": bad addr");
                    continue;
                }
                if (a == null) {
                    lines.add("// " + h + ": null addr");
                    continue;
                }
                Function fn = fm.getFunctionAt(a);
                if (fn == null) fn = fm.getFunctionContaining(a);
                if (fn == null) {
                    lines.add("// " + h + ": no function");
                    continue;
                }
                lines.add(String.format("// ===== %s @ %s =====", fn.getName(), h));
                DecompileResults r = decomp.decompileFunction(fn, 60, new ConsoleTaskMonitor());
                if (r != null && r.decompileCompleted()) {
                    lines.add(r.getDecompiledFunction().getC());
                } else {
                    lines.add("// (decompile failed)");
                }
                lines.add("");
            }
            writeLines(label + ".c", lines);
        } finally {
            decomp.dispose();
        }
    }

    private void decompileFunctions(List<Function> fns, String label) throws Exception {
        Collections.sort(fns, (a, b) -> Long.compare(a.getEntryPoint().getOffset(), b.getEntryPoint().getOffset()));
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try {
            List<String> lines = new ArrayList<>();
            for (Function fn : fns) {
                lines.add(String.format("// ===== %s @ 0x%x =====",
                        fn.getName(), fn.getEntryPoint().getOffset()));
                DecompileResults r = decomp.decompileFunction(fn, 60, new ConsoleTaskMonitor());
                if (r != null && r.decompileCompleted()) {
                    lines.add(r.getDecompiledFunction().getC());
                } else {
                    lines.add("// (decompile failed)");
                }
                lines.add("");
            }
            writeLines(label + ".c", lines);
        } finally {
            decomp.dispose();
        }
    }

    private void writeLines(String name, List<String> lines) throws Exception {
        File f = new File(outDir, name);
        try (PrintWriter pw = new PrintWriter(f)) {
            for (String l : lines) pw.println(l);
        }
    }
}
