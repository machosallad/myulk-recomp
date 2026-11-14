// Build a struct for every Delphi class and type Self parameters and globals.
//
// Reads analysis/class_layouts.csv (from tools/delphi_map.py):
//   C,class,size,parent        -> struct /Delphi/<class> of instance size, vmt at 0
//   F,class,offset,name,type   -> far pointer field to the component's struct
//   M,seg,offset,class,name    -> first parameter of that function becomes "Self"
// and analysis/globals.csv      -> form globals typed as far pointers.
//
// Run after ApplySymbols and auto-analysis.
// @category Myulk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.data.DataUtilities.ClearDataMode;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.*;

public class ApplyClassTypes extends GhidraScript {
    private final CategoryPath CAT = new CategoryPath("/Delphi");
    private DataTypeManager dtm;
    private final Map<String, Structure> structs = new HashMap<>();

    private DataType farPtr(DataType to) {
        return new PointerDataType(to == null ? VoidDataType.dataType : to, 4, dtm);
    }

    /** ptr:Class -> far pointer to struct; word:T / dword:T -> typedef T. */
    private DataType paramType(String kind, String type) throws Exception {
        if (kind.equals("ptr")) {
            Structure s = structs.get(type);
            return farPtr(s != null ? s : named(type, Undefined1DataType.dataType));
        }
        return named(type, kind.equals("dword") ? DWordDataType.dataType : WordDataType.dataType);
    }

    private DataType named(String name, DataType base) {
        DataType existing = dtm.getDataType(CAT, name);
        if (existing != null) return existing;
        return dtm.addDataType(new TypedefDataType(CAT, name, base, dtm),
            DataTypeConflictHandler.KEEP_HANDLER);
    }

    private MemoryBlock block(String name) {
        return currentProgram.getMemory().getBlock(name);
    }

    @Override
    public void run() throws Exception {
        String root = System.getProperty("myulk.root");
        dtm = currentProgram.getDataTypeManager();
        List<String[]> rows = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(new File(root, "analysis/class_layouts.csv")))) {
            r.readLine();
            String line;
            while ((line = r.readLine()) != null) rows.add(line.split(",", -1));
        }

        // pass 1: empty structs of the right size, so fields can point at any class
        for (String[] f : rows) {
            if (!f[0].equals("C")) continue;
            int size = Math.max(Integer.parseInt(f[2]), 4);
            StructureDataType s = new StructureDataType(CAT, f[1], size, dtm);
            Structure res = (Structure) dtm.addDataType(s, DataTypeConflictHandler.REPLACE_HANDLER);
            structs.put(f[1], res);
        }
        // pass 2: vmt + published component fields
        int nf = 0;
        for (Structure s : structs.values()) {
            s.replaceAtOffset(0, farPtr(null), 4, "vmt", "far pointer to the class VMT");
        }
        for (String[] f : rows) {
            if (!f[0].equals("F")) continue;
            Structure s = structs.get(f[1]);
            int off = Integer.parseInt(f[2]);
            if (s == null || off + 4 > s.getLength()) continue;
            s.replaceAtOffset(off, farPtr(structs.get(f[4])), 4, f[3], null);
            nf++;
        }

        // pass 3: Self parameters
        int typed = 0, added = 0, skipped = 0;
        for (String[] f : rows) {
            if (!f[0].equals("M")) continue;
            MemoryBlock b = block("Code" + f[1]);
            Structure s = structs.get(f[3]);
            if (b == null || s == null) { skipped++; continue; }
            Address a = b.getStart().add(Long.parseLong(f[2], 16));
            Function fn = getFunctionAt(a);
            if (fn == null) { skipped++; continue; }
            DataType selfType = farPtr(s);
            try {
                if (fn.getParameterCount() > 0) {
                    Parameter p = fn.getParameter(0);
                    if (p.getLength() == 4) {
                        p.setDataType(selfType, SourceType.USER_DEFINED);
                        p.setName("Self", SourceType.USER_DEFINED);
                        typed++;
                    } else {
                        skipped++;
                    }
                } else {
                    fn.replaceParameters(
                        List.of(new ParameterImpl("Self", selfType, currentProgram)),
                        FunctionUpdateType.DYNAMIC_STORAGE_FORMAL_PARAMS, true, SourceType.USER_DEFINED);
                    added++;
                }
            } catch (Exception e) {
                skipped++;
            }
        }

        // pass 3b: full event-handler signatures (S rows), already in
        // Ghidra's stdcall16far parameter order
        int sigs = 0;
        for (String[] f : rows) {
            if (!f[0].equals("S")) continue;
            MemoryBlock b = block("Code" + f[1]);
            if (b == null) continue;
            Function fn = getFunctionAt(b.getStart().add(Long.parseLong(f[2], 16)));
            if (fn == null) continue;
            List<Parameter> params = new ArrayList<>();
            for (String p : f[4].split(";")) {
                String[] parts = p.split(":", 3);   // name, kind, type
                params.add(new ParameterImpl(parts[0], paramType(parts[1], parts[2]), currentProgram));
            }
            try {
                fn.updateFunction(null, new ReturnParameterImpl(VoidDataType.dataType, currentProgram),
                    params, FunctionUpdateType.DYNAMIC_STORAGE_FORMAL_PARAMS, true, SourceType.USER_DEFINED);
                sigs++;
            } catch (Exception e) {
                println("signature failed at " + fn.getEntryPoint() + ": " + e.getMessage());
            }
        }
        println("event handler signatures: " + sigs);

        // pass 4: form globals
        int ng = 0;
        MemoryBlock data = null;
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.getName().startsWith("Data")) data = b;
        }
        File g = new File(root, "analysis/globals.csv");
        if (data != null && g.isFile()) {
            try (BufferedReader r = new BufferedReader(new FileReader(g))) {
                r.readLine();
                String line;
                while ((line = r.readLine()) != null) {
                    String[] f = line.split(",", 3);
                    Structure s = structs.get(f[2].trim());
                    Address a = data.getStart().add(Long.parseLong(f[0], 16));
                    DataUtilities.createData(currentProgram, a, farPtr(s), 4,
                        ClearDataMode.CLEAR_ALL_CONFLICT_DATA);
                    ng++;
                }
            }
        }
        println(String.format("structs=%d fields=%d self_typed=%d self_added=%d skipped=%d globals=%d",
            structs.size(), nf, typed, added, skipped, ng));
    }
}
