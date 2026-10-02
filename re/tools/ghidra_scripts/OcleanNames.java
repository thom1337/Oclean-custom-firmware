// Apply 'addr name' labels (function names) from a file.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.nio.file.*;

public class OcleanNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        int n = 0;
        for (String file : getScriptArgs()) {
            for (String line : Files.readAllLines(Paths.get(file))) {
                String[] p = line.trim().split("\\s+");
                if (p.length < 2 || p[0].startsWith("#")) continue;
                Address ad = toAddr(Long.parseLong(p[0], 16));
                if (!currentProgram.getMemory().contains(ad)) continue;
                Function f = getFunctionAt(ad);
                if (f == null) { disassemble(ad); f = createFunction(ad, p[1]); }
                try {
                    if (f != null) f.setName(p[1], SourceType.USER_DEFINED);
                    else currentProgram.getSymbolTable().createLabel(ad, p[1], SourceType.USER_DEFINED);
                    n++;
                } catch (Exception e) { println("name " + p[1] + ": " + e.getMessage()); }
            }
        }
        println("names applied: " + n);
    }
}
