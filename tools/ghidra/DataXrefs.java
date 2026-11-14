// List every data-segment address referenced from code, with the
// referencing functions. Output: decomp/data_xrefs.csv
//   ds_offset,refs,reads,writes,functions(;-separated)
// @category Myulk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.File;
import java.io.PrintWriter;
import java.util.TreeSet;

public class DataXrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String root = System.getProperty("myulk.root");
        MemoryBlock data = null;
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.getName().startsWith("Data")) data = b;
        }
        ReferenceManager rm = currentProgram.getReferenceManager();
        int n = 0;
        try (PrintWriter w = new PrintWriter(new File(root, "decomp/data_xrefs.csv"), "UTF-8")) {
            w.println("ds_offset,refs,reads,writes,functions");
            AddressIterator it = rm.getReferenceDestinationIterator(
                new ghidra.program.model.address.AddressSet(data.getStart(), data.getEnd()), true);
            while (it.hasNext()) {
                Address a = it.next();
                int refs = 0, reads = 0, writes = 0;
                TreeSet<String> fns = new TreeSet<>();
                for (Reference r : getReferencesTo(a)) {
                    refs++;
                    if (r.getReferenceType().isRead()) reads++;
                    if (r.getReferenceType().isWrite()) writes++;
                    Function f = getFunctionContaining(r.getFromAddress());
                    MemoryBlock fb = currentProgram.getMemory().getBlock(r.getFromAddress());
                    String where = fb == null ? "?" : fb.getName().replace("Code", "");
                    fns.add(f == null ? where + ":?" : f.getName(true));
                }
                w.printf("%04X,%d,%d,%d,%s%n", a.subtract(data.getStart()), refs, reads, writes,
                    String.join(";", fns));
                n++;
            }
        }
        println("data addresses referenced: " + n);
    }
}
