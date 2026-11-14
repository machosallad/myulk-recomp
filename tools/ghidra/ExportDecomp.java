// Decompile all functions in the game's own code segments and write
// them to decomp/segNN.c (one file per NE segment), plus
// decomp/functions.csv with an index of every function.
//
// Script args: [firstSeg] [lastSeg]   (default 1 22 — the game code;
// segments 23-33 are the Delphi VCL/RTL)
// @category Myulk
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;

import java.io.File;
import java.io.PrintWriter;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String root = System.getProperty("myulk.root");
        String[] args = getScriptArgs();
        int first = args.length > 0 ? Integer.parseInt(args[0]) : 1;
        int last = args.length > 1 ? Integer.parseInt(args[1]) : 22;

        File out = new File(root, "decomp");
        out.mkdirs();
        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.openProgram(currentProgram);

        int total = 0, failed = 0;
        try (PrintWriter idx = new PrintWriter(new File(out, "functions.csv"), "UTF-8")) {
            idx.println("seg,offset,size,name,callers,callees,decompiled");
            for (int seg = first; seg <= last; seg++) {
                MemoryBlock b = currentProgram.getMemory().getBlock("Code" + seg);
                if (b == null) continue;
                File f = new File(out, String.format("seg%02d.c", seg));
                try (PrintWriter w = new PrintWriter(f, "UTF-8")) {
                    w.printf("// NE segment %d (%s)%n%n", seg, b.getStart());
                    FunctionIterator it = currentProgram.getFunctionManager()
                        .getFunctions(new ghidra.program.model.address.AddressSet(b.getStart(), b.getEnd()), true);
                    while (it.hasNext() && !monitor.isCancelled()) {
                        Function fn = it.next();
                        long off = fn.getEntryPoint().subtract(b.getStart());
                        String name = fn.getName(true);
                        DecompileResults res = di.decompileFunction(fn, 60, monitor);
                        boolean ok = res != null && res.decompileCompleted();
                        w.printf("// ---- %d:%04X  %s ----%n", seg, off, name);
                        if (ok) {
                            w.println(res.getDecompiledFunction().getC());
                        } else {
                            w.println("// decompile failed: " + (res == null ? "null" : res.getErrorMessage()));
                            w.println();
                            failed++;
                        }
                        idx.printf("%d,%04X,%d,%s,%d,%d,%s%n", seg, off,
                            fn.getBody().getNumAddresses(), name,
                            fn.getCallingFunctions(monitor).size(),
                            fn.getCalledFunctions(monitor).size(), ok);
                        total++;
                    }
                }
            }
        }
        di.dispose();
        println(String.format("exported %d functions (%d failed) to %s", total, failed, out));
    }
}
