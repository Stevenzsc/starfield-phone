// Find Papyrus native implementations by name: for each string arg, every reference to that exact C string is
// printed with the strings and code addresses loaded by LEA in the 12 instructions around it (the registration
// call passes the native name, the script class name and the implementation, usually in R9).
// usage: run.cmd NativeFind.java <out> GetValue GetLevel ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

public class NativeFind extends GhidraScript {
    String describe(Address a) {
        Data d = getDataAt(a);
        if (d != null && d.hasStringValue()) return "\"" + d.getValue() + "\"";
        Function f = getFunctionAt(a);
        if (f != null) return "FN " + f.getEntryPoint();
        try {
            byte[] b = new byte[48];
            currentProgram.getMemory().getBytes(a, b);
            StringBuilder sb = new StringBuilder();
            for (byte x : b) { if (x == 0) break; if (x < 32 || x > 126) return a.toString(); sb.append((char) x); }
            if (sb.length() > 2) return "\"" + sb + "\"";
        } catch (Exception e) { }
        return a.toString();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        for (int i = 1; i < args.length; i++) {
            byte[] s = args[i].getBytes("US-ASCII");
            byte[] pat = new byte[s.length + 2];
            System.arraycopy(s, 0, pat, 1, s.length);
            out.println("==== " + args[i]);
            Address cur = currentProgram.getMinAddress();
            for (int n = 0; n < 10; n++) {
                Address h = currentProgram.getMemory().findBytes(cur, pat, null, true, monitor);
                if (h == null) break;
                cur = h.add(1);
                Address a = h.add(1);
                for (Reference r : getReferencesTo(a)) {
                    Instruction ins = getInstructionAt(r.getFromAddress());
                    if (ins == null) continue;
                    StringBuilder sb = new StringBuilder();
                    Instruction p = ins;
                    for (int k = 0; k < 8 && p.getPrevious() != null; k++) p = p.getPrevious();
                    for (int k = 0; k < 16 && p != null; k++, p = p.getNext()) {
                        if (!p.getMnemonicString().startsWith("LEA")) continue;
                        for (Reference rr : p.getReferencesFrom()) {
                            sb.append(" ").append(p.getDefaultOperandRepresentation(0)).append("=").append(describe(rr.getToAddress()));
                        }
                    }
                    out.println("  @" + r.getFromAddress() + sb);
                }
            }
        }
        out.close();
    }
}
