import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.io.PrintWriter;
// args: out hexScalar... -> instructions using that immediate, with containing function
public class ScalarRefs extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs(); PrintWriter out = new PrintWriter(a[0], "UTF-8");
        long[] want = new long[a.length - 1];
        for (int i = 1; i < a.length; i++) want[i-1] = Long.parseLong(a[i], 16);
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        int n = 0;
        while (it.hasNext() && n < 400) {
            Instruction ins = it.next();
            for (int op = 0; op < ins.getNumOperands(); op++) {
                for (Object o : ins.getOpObjects(op)) {
                    if (o instanceof Scalar) {
                        long v = ((Scalar) o).getUnsignedValue();
                        for (long w : want) if (v == w) {
                            Function f = getFunctionContaining(ins.getAddress());
                            out.println(String.format("%X %s  %s", w, ins.getAddress(), f == null ? "?" : f.getName()));
                            n++;
                        }
                    }
                }
            }
        }
        out.close();
    }
}
