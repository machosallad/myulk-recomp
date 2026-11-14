// Print the memory blocks Ghidra created for the loaded program.
// @category Myulk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;

public class ListBlocks extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            println("BLOCK " + b.getName() + " " + b.getStart() + " " + b.getEnd()
                + " size=" + b.getSize() + " x=" + b.isExecute());
        }
    }
}
