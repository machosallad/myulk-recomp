// Apply names recovered by tools/delphi_map.py to the Myulk program.
//
// Reads analysis/symbols.csv (seg,offset,name) and analysis/globals.csv
// (ds_offset,name,type). NE segment N is the memory block "CodeN" (or
// "DataN"); offsets are relative to the block start.
//
// Names of the form "Class.Method" go into a Ghidra class namespace.
// "VMT_*" become labels; everything else becomes a function.
// Safe to run more than once (run as pre- and post-script on import so
// names survive auto-analysis).
// @category Myulk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.HashMap;
import java.util.Map;

public class ApplySymbols extends GhidraScript {
    private final Map<Integer, MemoryBlock> segs = new HashMap<>();

    @Override
    public void run() throws Exception {
        String root = System.getProperty("myulk.root");
        if (root == null) {
            root = askDirectory("Myulk repository root", "OK").getAbsolutePath();
        }
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            String n = b.getName();
            String digits = n.replaceAll("^(Code|Data)", "");
            if (!digits.equals(n) && digits.matches("\\d+")) {
                segs.put(Integer.parseInt(digits), b);
            }
        }
        // commit decompiler-inferred parameters during auto-analysis, so
        // ApplyClassTypes can retype Self without dropping the others
        try {
            setAnalysisOption(currentProgram, "Decompiler Parameter ID", "true");
        } catch (Exception e) {
            println("could not enable Decompiler Parameter ID: " + e.getMessage());
        }
        int fn = 0, lbl = 0, glob = 0, bad = 0;
        SymbolTable st = currentProgram.getSymbolTable();

        try (BufferedReader r = new BufferedReader(new FileReader(new File(root, "analysis/symbols.csv")))) {
            r.readLine();
            String line;
            while ((line = r.readLine()) != null) {
                String[] f = line.split(",", 3);
                MemoryBlock b = segs.get(Integer.parseInt(f[0]));
                if (b == null) { bad++; continue; }
                Address a = b.getStart().add(Long.parseLong(f[1], 16));
                String name = f[2].trim();
                Namespace ns = currentProgram.getGlobalNamespace();
                String simple = name;
                int dot = name.lastIndexOf('.');
                if (dot > 0) {
                    String cls = name.substring(0, dot);
                    simple = name.substring(dot + 1);
                    Namespace existing = st.getNamespace(cls, currentProgram.getGlobalNamespace());
                    ns = existing != null ? existing
                        : st.createClass(currentProgram.getGlobalNamespace(), cls, SourceType.IMPORTED);
                }
                if (name.startsWith("VMT_")) {
                    st.createLabel(a, name, SourceType.IMPORTED).setPrimary();
                    lbl++;
                    continue;
                }
                Function func = getFunctionAt(a);
                if (func == null) {
                    disassemble(a);
                    func = createFunction(a, null);
                }
                if (func == null) { bad++; continue; }
                func.setParentNamespace(ns);
                func.setName(simple, SourceType.IMPORTED);
                fn++;
            }
        }

        // unnamed entry points found by tools/find_functions.py
        int extra = 0;
        File fs = new File(root, "analysis/function_starts.csv");
        if (fs.isFile()) {
            try (BufferedReader r = new BufferedReader(new FileReader(fs))) {
                r.readLine();
                String line;
                while ((line = r.readLine()) != null) {
                    String[] f = line.split(",", 3);
                    MemoryBlock b = segs.get(Integer.parseInt(f[0]));
                    if (b == null) continue;
                    Address a = b.getStart().add(Long.parseLong(f[1], 16));
                    if (getFunctionContaining(a) != null) continue;
                    disassemble(a);
                    if (createFunction(a, null) != null) extra++;
                }
            }
        }
        println("extra function starts created: " + extra);

        MemoryBlock data = null;
        for (MemoryBlock b : segs.values()) {
            if (b.getName().startsWith("Data")) data = b;
        }
        File g = new File(root, "analysis/globals.csv");
        if (data != null && g.isFile()) {
            try (BufferedReader r = new BufferedReader(new FileReader(g))) {
                r.readLine();
                String line;
                while ((line = r.readLine()) != null) {
                    String[] f = line.split(",", 3);
                    Address a = data.getStart().add(Long.parseLong(f[0], 16));
                    st.createLabel(a, f[1], SourceType.IMPORTED).setPrimary();
                    glob++;
                }
            }
        }
        println(String.format("functions=%d labels=%d globals=%d failed=%d", fn, lbl, glob, bad));
    }
}
