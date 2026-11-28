// Disassemble code Ghidra did not mark as a function (read-only project): N instructions from each address,
// following unconditional jumps once.
// usage: run.cmd PseudoDis.java <out> <count> <hexaddr...>
import ghidra.app.script.GhidraScript;
import ghidra.app.util.PseudoDisassembler;
import ghidra.app.util.PseudoInstruction;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;

public class PseudoDis extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(a[0], "UTF-8");
        int count = Integer.parseInt(a[1]);
        PseudoDisassembler pd = new PseudoDisassembler(currentProgram);
        for (int k = 2; k < a.length; k++) {
            out.println("==== " + a[k]);
            Address at = toAddr(a[k]);
            for (int i = 0; i < count && at != null; i++) {
                PseudoInstruction ins = pd.disassemble(at);
                if (ins == null) break;
                out.println(at + "  " + ins);
                at = at.add(ins.getLength());
            }
        }
        out.close();
    }
}
