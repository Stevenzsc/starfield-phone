// For each arg: symbols whose full name contains it; for vftable symbols also print the first 12 slots.
// usage: run.cmd SymVtbl.java <out> MoveToFunctor ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import java.io.PrintWriter;

public class SymVtbl extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext()) {
            Symbol s = it.next();
            String n = s.getName(true);
            for (int i = 1; i < args.length; i++) {
                if (!n.contains(args[i])) continue;
                out.println(s.getAddress() + "  " + n);
                if (n.endsWith("vftable")) {
                    for (int k = 0; k < 12; k++) {
                        long p = getLong(s.getAddress().add(k * 8));
                        Address a = toAddr(p);
                        Function f = getFunctionAt(a);
                        out.println("   [" + k + "] " + a + (f != null ? " " + f.getName() : ""));
                    }
                }
            }
        }
        out.close();
    }
}
