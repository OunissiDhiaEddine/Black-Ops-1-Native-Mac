// T-006: export structural facts from the analysed program (no bytes, no decompiler output).
// Usage (headless): -postScript ExportProgramFacts.java <out_dir>
// Writes: functions.csv blocks.csv edges.csv jumptables.csv calls.csv fnptr_refs.csv
//         strings.csv summary.json
//@category BO1
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.block.BasicBlockModel;
import ghidra.program.model.block.CodeBlock;
import ghidra.program.model.block.CodeBlockIterator;
import ghidra.program.model.block.CodeBlockReference;
import ghidra.program.model.block.CodeBlockReferenceIterator;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

public class ExportProgramFacts extends GhidraScript {

    private static String hex(Address a) {
        return a == null ? "" : String.format("0x%08x", a.getOffset());
    }

    private static String csv(String s) {
        if (s == null) return "";
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"') b.append("\"\"");
            else if (c == '\n') b.append("\\n");
            else if (c == '\r') b.append("\\r");
            else if (c == '\t') b.append("\\t");
            else if (c < 0x20) b.append(String.format("\\x%02x", (int) c));
            else b.append(c);
        }
        return b.append('"').toString();
    }

    private String blockName(Address a) {
        MemoryBlock mb = currentProgram.getMemory().getBlock(a);
        return mb == null ? "" : mb.getName();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : "ghidra_export");
        out.mkdirs();

        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();

        long nFunc = 0, nThunk = 0, nBlocks = 0, nEdges = 0, nInsn = 0, codeBytes = 0;
        long nJumpTables = 0, nJumpUnresolved = 0, nCallComputed = 0, nCallComputedResolved = 0;
        long nCalls = 0, nFnPtr = 0, nStrings = 0, nFuncWithFnPtr = 0;

        // ---- functions + data references to function entries (address-taken functions)
        try (PrintWriter w = new PrintWriter(new File(out, "functions.csv"));
             PrintWriter wp = new PrintWriter(new File(out, "fnptr_refs.csv"))) {
            w.println("addr,name,size,calling_convention,is_thunk,thunk_target,stack_purge,param_count,no_return,num_ranges,source");
            wp.println("func_addr,ref_from,ref_from_block,ref_type");
            for (Function f : fm.getFunctions(true)) {
                if (f.isExternal()) continue;
                nFunc++;
                if (f.isThunk()) nThunk++;
                Function tt = f.isThunk() ? f.getThunkedFunction(false) : null;
                String thunkTarget = tt == null ? "" : (tt.isExternal() ? tt.getName(true) : hex(tt.getEntryPoint()));
                String purge;
                try {
                    purge = Integer.toString(f.getStackPurgeSize());
                } catch (Exception e) {
                    purge = "";
                }
                w.println(String.join(",", hex(f.getEntryPoint()), csv(f.getName()),
                    Long.toString(f.getBody().getNumAddresses()), csv(f.getCallingConventionName()),
                    f.isThunk() ? "1" : "0", csv(thunkTarget), purge,
                    Integer.toString(f.getParameterCount()), f.hasNoReturn() ? "1" : "0",
                    Integer.toString(f.getBody().getNumAddressRanges()),
                    f.getSymbol().getSource().toString()));
                boolean taken = false;
                ReferenceIterator ri = rm.getReferencesTo(f.getEntryPoint());
                while (ri.hasNext()) {
                    Reference r = ri.next();
                    RefType t = r.getReferenceType();
                    if (t.isCall() || t.isJump() || t.isFallthrough()) continue;
                    wp.println(String.join(",", hex(f.getEntryPoint()), hex(r.getFromAddress()),
                        blockName(r.getFromAddress()), t.getName()));
                    nFnPtr++;
                    taken = true;
                }
                if (taken) nFuncWithFnPtr++;
            }
        }

        // ---- basic blocks + edges
        BasicBlockModel bbm = new BasicBlockModel(currentProgram);
        try (PrintWriter wb = new PrintWriter(new File(out, "blocks.csv"));
             PrintWriter we = new PrintWriter(new File(out, "edges.csv"))) {
            wb.println("start,end,func,flow");
            we.println("src_block,dst,flow");
            CodeBlockIterator it = bbm.getCodeBlocks(monitor);
            while (it.hasNext()) {
                monitor.checkCancelled();
                CodeBlock b = it.next();
                Address s = b.getFirstStartAddress();
                Function f = fm.getFunctionContaining(s);
                wb.println(String.join(",", hex(s), hex(b.getMaxAddress()),
                    f == null ? "" : hex(f.getEntryPoint()), b.getFlowType().getName()));
                nBlocks++;
                CodeBlockReferenceIterator di = b.getDestinations(monitor);
                while (di.hasNext()) {
                    CodeBlockReference d = di.next();
                    we.println(String.join(",", hex(s), hex(d.getDestinationAddress()), d.getFlowType().getName()));
                    nEdges++;
                }
            }
        }

        // ---- per-instruction: computed jumps (jump tables), computed calls, call graph
        try (PrintWriter wj = new PrintWriter(new File(out, "jumptables.csv"));
             PrintWriter wc = new PrintWriter(new File(out, "calls.csv"))) {
            wj.println("insn,func,kind,num_targets,targets");
            wc.println("from,from_func,to,kind");
            InstructionIterator ii = listing.getInstructions(true);
            while (ii.hasNext()) {
                monitor.checkCancelled();
                Instruction in = ii.next();
                nInsn++;
                codeBytes += in.getLength();
                FlowType ft = in.getFlowType();
                if (!ft.isCall() && !ft.isJump()) continue;
                Address a = in.getAddress();
                Function f = fm.getFunctionContaining(a);
                String fs = f == null ? "" : hex(f.getEntryPoint());
                if (ft.isComputed()) {
                    TreeSet<Address> targets = new TreeSet<>();
                    for (Reference r : in.getReferencesFrom()) {
                        RefType t = r.getReferenceType();
                        if (t.isFlow() && !t.isFallthrough() && r.getToAddress().isMemoryAddress())
                            targets.add(r.getToAddress());
                    }
                    List<String> ts = new ArrayList<>();
                    for (Address t : targets) ts.add(hex(t));
                    String kind = ft.isCall() ? "call" : "jump";
                    wj.println(String.join(",", hex(a), fs, kind, Integer.toString(targets.size()),
                        csv(String.join(";", ts))));
                    if (ft.isJump()) {
                        nJumpTables++;
                        if (targets.isEmpty()) nJumpUnresolved++;
                    } else {
                        nCallComputed++;
                        if (!targets.isEmpty()) nCallComputedResolved++;
                    }
                }
                if (ft.isCall()) {
                    for (Reference r : in.getReferencesFrom()) {
                        if (!r.getReferenceType().isCall()) continue;
                        wc.println(String.join(",", hex(a), fs, hex(r.getToAddress()),
                            r.getReferenceType().getName()));
                        nCalls++;
                    }
                }
            }
        }

        // ---- strings (address, length, xref count, text)
        try (PrintWriter ws = new PrintWriter(new File(out, "strings.csv"))) {
            ws.println("addr,block,length,xrefs,value");
            DataIterator di = listing.getDefinedData(true);
            while (di.hasNext()) {
                monitor.checkCancelled();
                Data d = di.next();
                if (!StringDataInstance.isString(d)) continue;
                String v = StringDataInstance.getStringDataInstance(d).getStringValue();
                if (v == null) continue;
                ws.println(String.join(",", hex(d.getAddress()), blockName(d.getAddress()),
                    Integer.toString(d.getLength()), Integer.toString(rm.getReferenceCountTo(d.getAddress())),
                    csv(v)));
                nStrings++;
            }
        }

        try (PrintWriter w = new PrintWriter(new File(out, "summary.json"))) {
            w.println("{");
            w.println(" \"program\": \"" + currentProgram.getName() + "\",");
            w.println(" \"ghidra_version\": \"" + ghidra.framework.Application.getApplicationVersion() + "\",");
            w.println(" \"language\": \"" + currentProgram.getLanguageID() + "\",");
            w.println(" \"compiler_spec\": \"" + currentProgram.getCompilerSpec().getCompilerSpecID() + "\",");
            w.println(" \"functions\": " + nFunc + ",");
            w.println(" \"thunks\": " + nThunk + ",");
            w.println(" \"instructions\": " + nInsn + ",");
            w.println(" \"code_bytes\": " + codeBytes + ",");
            w.println(" \"basic_blocks\": " + nBlocks + ",");
            w.println(" \"block_edges\": " + nEdges + ",");
            w.println(" \"computed_jumps\": " + nJumpTables + ",");
            w.println(" \"computed_jumps_unresolved\": " + nJumpUnresolved + ",");
            w.println(" \"computed_calls\": " + nCallComputed + ",");
            w.println(" \"computed_calls_with_known_target\": " + nCallComputedResolved + ",");
            w.println(" \"call_refs\": " + nCalls + ",");
            w.println(" \"address_taken_functions\": " + nFuncWithFnPtr + ",");
            w.println(" \"function_pointer_refs\": " + nFnPtr + ",");
            w.println(" \"strings\": " + nStrings);
            w.println("}");
        }
        println("ExportProgramFacts: wrote " + out.getAbsolutePath());
    }
}
