// args: out start end  -> every reference to any address in [start,end)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
public class RangeRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(a[0], "UTF-8");
        Address s = toAddr(a[1]), e = toAddr(a[2]);
        for (Address x = s; x.compareTo(e) < 0; x = x.add(1)) {
            for (Reference r : getReferencesTo(x)) {
                Function f = getFunctionContaining(r.getFromAddress());
                out.println(x + " <- " + r.getFromAddress() + " " + r.getReferenceType() + " in " +
                    (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
            }
        }
        out.close();
    }
}
