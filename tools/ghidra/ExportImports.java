// Export the program's imported functions (module, ordinal, name) as
// analysis/imports.csv. Ghidra resolves Win16 ordinals to API names.
// @category Myulk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.ExternalLocation;
import ghidra.program.model.symbol.ExternalLocationIterator;
import ghidra.program.model.symbol.ExternalManager;

import java.io.File;
import java.io.PrintWriter;

public class ExportImports extends GhidraScript {
    @Override
    public void run() throws Exception {
        String root = System.getProperty("myulk.root");
        ExternalManager em = currentProgram.getExternalManager();
        int n = 0;
        try (PrintWriter w = new PrintWriter(new File(root, "analysis/imports.csv"), "UTF-8")) {
            w.println("module,ordinal,name");
            for (String lib : em.getExternalLibraryNames()) {
                ExternalLocationIterator it = em.getExternalLocations(lib);
                while (it.hasNext()) {
                    ExternalLocation loc = it.next();
                    String label = loc.getLabel();
                    String orig = loc.getOriginalImportedName();
                    String ord = orig != null && orig.startsWith("Ordinal_") ? orig.substring(8) : "";
                    if (ord.isEmpty() && label.matches(".*_\\d+$")) ord = label.replaceAll(".*_", "");
                    w.printf("%s,%s,%s%n", lib, ord, label);
                    n++;
                }
            }
        }
        println("imports: " + n);
    }
}
