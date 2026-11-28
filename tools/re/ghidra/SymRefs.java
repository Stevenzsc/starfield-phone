// args: out <substring>... -> every symbol whose full name contains a substring, and the functions referencing it.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
public class SymRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(a[0], "UTF-8");
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        int n = 0;
        while (it.hasNext() && n < 2000) {
            Symbol s = it.next();
            String name = s.getName(true);
            boolean hit = false;
            for (int i = 1; i < a.length; i++) if (name.contains(a[i])) hit = true;
            if (!hit) continue;
            n++;
            out.println("==== " + name + " @ " + s.getAddress());
            int k = 0;
            for (Reference r : getReferencesTo(s.getAddress())) {
                if (k++ > 12) break;
                Function f = getFunctionContaining(r.getFromAddress());
                out.println("    from " + r.getFromAddress() + " in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
            }
        }
        out.close();
    }
}
