import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
public class DisFunc extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs(); PrintWriter out = new PrintWriter(a[0], "UTF-8");
        Function f = getFunctionAt(toAddr(a[1]));
        for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true)) out.println(i.getAddress() + "  " + i);
        out.close();
    }
}
