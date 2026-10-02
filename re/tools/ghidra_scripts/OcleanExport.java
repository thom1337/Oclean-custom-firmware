// Decompile every function in [lo,hi) ranges and write C to outdir.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class OcleanExport extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args[0];
        new File(out).mkdirs();
        List<long[]> ranges = new ArrayList<>();
        for (int i = 1; i + 1 < args.length; i += 2)
            ranges.add(new long[]{Long.parseLong(args[i], 16), Long.parseLong(args[i + 1], 16)});
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true); di.toggleSyntaxTree(false);
        di.openProgram(currentProgram);
        PrintWriter all = new PrintWriter(new FileWriter(out + "/all.c"));
        PrintWriter idx = new PrintWriter(new FileWriter(out + "/index.txt"));
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0, fail = 0;
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            long a = f.getEntryPoint().getOffset();
            boolean in = false;
            for (long[] r : ranges) if (a >= r[0] && a < r[1]) in = true;
            if (!in) continue;
            StringBuilder callers = new StringBuilder();
            int nc = 0;
            for (Reference ref : getReferencesTo(f.getEntryPoint())) {
                Function cf = getFunctionContaining(ref.getFromAddress());
                if (nc++ < 12) callers.append(cf != null ? cf.getName() : ref.getFromAddress().toString()).append(' ');
            }
            idx.println(String.format("%08x %s size=%d callers(%d): %s", a, f.getName(), f.getBody().getNumAddresses(), nc, callers));
            DecompileResults res = di.decompileFunction(f, 120, monitor);
            String c = (res != null && res.decompileCompleted()) ? res.getDecompiledFunction().getC() : null;
            if (c == null) { fail++; c = "/* decompile failed: " + (res != null ? res.getErrorMessage() : "null") + " */\n"; }
            String hdr = String.format("\n// ===== %08x %s  (callers %d: %s)\n", a, f.getName(), nc, callers);
            all.print(hdr); all.print(c);
            n++;
        }
        all.close(); idx.close();
        println("decompiled " + n + " functions, failed " + fail);
    }
}
