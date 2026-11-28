// For each ASCII string arg: find exact null-terminated occurrences in memory, then list references to each.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
public class StrXrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        for (int i = 1; i < args.length; i++) {
            byte[] s = args[i].getBytes("US-ASCII");
            byte[] pat = new byte[s.length + 2];
            pat[0] = 0; System.arraycopy(s, 0, pat, 1, s.length); pat[s.length + 1] = 0;
            out.println("==== \"" + args[i] + "\"");
            java.util.List<Address> hits = new java.util.ArrayList<>();
            Address cur = currentProgram.getMinAddress();
            while (hits.size() < 20) {
                Address h = currentProgram.getMemory().findBytes(cur, pat, null, true, monitor);
                if (h == null) break;
                hits.add(h); cur = h.add(1);
            }
            for (Address h : hits) {
                Address a = h.add(1);
                out.println("  str @ " + a);
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.println("    from " + r.getFromAddress() + " in " +
                        (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                }
            }
        }
        out.close();
    }
}
