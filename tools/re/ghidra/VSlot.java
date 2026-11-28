import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
// args: out vtable slotOffsetHex... -> function at each vtable slot
public class VSlot extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs(); PrintWriter out = new PrintWriter(a[0], "UTF-8");
        Address vt = toAddr(a[1]);
        for (int i = 2; i < a.length; i++) {
            long off = Long.parseLong(a[i], 16);
            long p = getLong(vt.add(off));
            Function f = getFunctionAt(toAddr(p));
            out.println(String.format("slot +0x%X -> %X %s", off, p, f == null ? "?" : f.getName()));
        }
        out.close();
    }
}
