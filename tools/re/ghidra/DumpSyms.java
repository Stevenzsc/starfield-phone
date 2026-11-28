// Print the symbol (or containing function) at each address, plus the first 8 pointers of a vtable.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
public class DumpSyms extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(args[i]);
            Symbol s = getSymbolAt(a);
            out.println("==== " + args[i] + " symbol: " + (s == null ? "none" : s.getName(true)));
            for (int k = 0; k < 40; k++) {
                long p = getLong(a.add(k * 8L));
                Address t = toAddr(p);
                Function f = getFunctionAt(t);
                Symbol ts = getSymbolAt(t);
                out.println(String.format("  [0x%03X] %016X %s", k * 8, p, f != null ? f.getName() : (ts != null ? ts.getName(true) : "")));
            }
        }
        out.close();
    }
}
