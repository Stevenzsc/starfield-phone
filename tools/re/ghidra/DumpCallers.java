// For each address: every reference to it, with the containing function and its entry.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
public class DumpCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(args[i]);
            out.println("==== refs to " + args[i]);
            for (Reference r : getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                out.println("  from " + r.getFromAddress() + " " + r.getReferenceType() + " in " +
                    (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint() + " +0x" + Long.toHexString(r.getFromAddress().subtract(f.getEntryPoint()))));
            }
        }
        out.close();
    }
}
