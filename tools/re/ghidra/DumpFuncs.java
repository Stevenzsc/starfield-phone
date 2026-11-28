// Decompile selected functions of Starfield.exe 1.16.244 and write them to a text file.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
public class DumpFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            Function f = getFunctionAt(toAddr(args[i]));
            if (f == null) f = getFunctionContaining(toAddr(args[i]));
            out.println("==== " + args[i] + " " + (f == null ? "NO FUNCTION" : f.getName() + " @ " + f.getEntryPoint()));
            if (f == null) continue;
            DecompileResults r = d.decompileFunction(f, 120, monitor);
            out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "DECOMPILE FAILED: " + r.getErrorMessage());
        }
        out.close();
    }
}
