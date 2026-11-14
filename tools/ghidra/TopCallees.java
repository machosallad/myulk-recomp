// Rank functions in the library segments by how often game code calls them,
// and dump the decompiled body of the top unnamed ones for identification.
//
// Script args: [count] [gameLastSeg]   (default 150 22)
// Output: decomp/top_callees.txt
// @category Myulk
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.util.*;

public class TopCallees extends GhidraScript {
    private int segOf(ghidra.program.model.address.Address a) {
        MemoryBlock b = currentProgram.getMemory().getBlock(a);
        if (b == null || !b.getName().startsWith("Code")) return -1;
        return Integer.parseInt(b.getName().substring(4));
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int count = args.length > 0 ? Integer.parseInt(args[0]) : 150;
        int gameLast = args.length > 1 ? Integer.parseInt(args[1]) : 22;
        String root = System.getProperty("myulk.root");

        Map<Function, Integer> calls = new HashMap<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            int s = segOf(f.getEntryPoint());
            if (s <= gameLast) continue;
            int n = 0;
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                if (r.getReferenceType().isCall()) {
                    int from = segOf(r.getFromAddress());
                    if (from >= 1 && from <= gameLast) n++;
                }
            }
            if (n > 0) calls.put(f, n);
        }
        List<Map.Entry<Function, Integer>> list = new ArrayList<>(calls.entrySet());
        list.sort((a, b) -> b.getValue() - a.getValue());

        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.openProgram(currentProgram);
        File out = new File(root, "decomp/top_callees.txt");
        int shown = 0, totalCalls = 0, unnamedCalls = 0;
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            for (Map.Entry<Function, Integer> e : list) {
                Function f = e.getKey();
                boolean unnamed = f.getName().startsWith("FUN_");
                totalCalls += e.getValue();
                if (unnamed) unnamedCalls += e.getValue();
                if (!unnamed || shown >= count) continue;
                MemoryBlock b = currentProgram.getMemory().getBlock(f.getEntryPoint());
                long off = f.getEntryPoint().subtract(b.getStart());
                w.printf("==== %s:%04X %s  calls=%d size=%d%n", b.getName().substring(4), off,
                    f.getName(), e.getValue(), f.getBody().getNumAddresses());
                DecompileResults res = di.decompileFunction(f, 30, monitor);
                if (res != null && res.decompileCompleted()) {
                    String[] lines = res.getDecompiledFunction().getC().split("\n");
                    for (int i = 0; i < Math.min(lines.length, 45); i++) w.println(lines[i]);
                    if (lines.length > 45) w.println("  ... (" + lines.length + " lines)");
                }
                shown++;
            }
        }
        di.dispose();
        println(String.format("library functions called from game: %d, calls total %d, to unnamed %d; dumped %d",
            list.size(), totalCalls, unnamedCalls, shown));
    }
}
