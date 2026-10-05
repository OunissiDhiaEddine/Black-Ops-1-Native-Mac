// T-009a: disassemble + create functions at the addresses in a seeds CSV (addr,reason,refs)
// produced by tools/seed_functions.py. Run as a headless -preScript so auto-analysis then
// follows the new code. Usage: -preScript SeedFunctions.java <seeds.csv>
//@category BO1
import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class SeedFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int created = 0, existed = 0, failed = 0, total = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(args[0]))) {
            String line = r.readLine(); // header
            while ((line = r.readLine()) != null) {
                monitor.checkCancelled();
                String[] f = line.split(",");
                if (f.length < 2) continue;
                total++;
                Address a = toAddr(Long.decode(f[0]));
                if (getFunctionAt(a) != null) { existed++; continue; }
                if (getInstructionAt(a) == null) disassemble(a);
                if (getInstructionAt(a) == null) { failed++; continue; }
                Function fn = createFunction(a, null);
                if (fn == null) { failed++; continue; }
                fn.setComment("seed: " + f[1]);
                created++;
            }
        }
        println("SeedFunctions: seeds=" + total + " created=" + created + " existed=" + existed + " failed=" + failed);
    }
}
