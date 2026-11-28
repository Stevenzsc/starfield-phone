import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;
import java.io.PrintWriter;
public class Disasm extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs(); PrintWriter out = new PrintWriter(a[0], "UTF-8");
        Instruction ins = getInstructionAt(toAddr(a[1])).getPrevious().getPrevious().getPrevious().getPrevious();
        for (int i = 0; i < 9 && ins != null; i++, ins = ins.getNext()) out.println(ins.getAddress() + "  " + ins);
        out.close();
    }
}
