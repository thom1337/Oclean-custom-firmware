// Pre-analysis: memory map, ROM labels, function entries.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class OcleanSetup extends GhidraScript {
    private void block(String name, long start, long len, boolean x, boolean w) throws Exception {
        Memory mem = currentProgram.getMemory();
        Address a = toAddr(start);
        try {
            MemoryBlock b = mem.createUninitializedBlock(name, a, len, false);
            b.setRead(true); b.setWrite(w); b.setExecute(x);
        } catch (Exception e) { println("block " + name + ": " + e.getMessage()); }
    }
    @Override
    public void run() throws Exception {
        String dir = getScriptArgs()[0];
        Memory mem = currentProgram.getMemory();
        // RAM around the initialised .data (0x3fc99e00..0x3fca1d70): bss/heap
        block("dram_lo", 0x3fc88000L, 0x3fc99e00L - 0x3fc88000L, false, true);
        block("dram_hi", 0x3fca1d70L, 0x3fcf0000L - 0x3fca1d70L, false, true);
        block("rom", 0x40000000L, 0x60000L, true, false);
        block("romdata", 0x3ff00000L, 0x20000L, false, false);
        block("periph", 0x60000000L, 0xfe000L, false, true);
        block("rtcslow", 0x50000000L, 0x1000L, false, true);
        block("rtcslow2", 0x50001064L, 0x2000L - 0x1064L, false, true);
        SymbolTable st = currentProgram.getSymbolTable();
        int n = 0;
        for (String f : new String[]{"rom_syms.txt", "rom_ld.txt"}) {
            for (String line : Files.readAllLines(Paths.get(dir, f))) {
                String[] p = line.trim().split("\\s+");
                if (p.length < 2) continue;
                long a = Long.parseLong(p[0], 16);
                Address ad = toAddr(a);
                if (!mem.contains(ad)) continue;
                try { st.createLabel(ad, p[1], SourceType.IMPORTED); n++; } catch (Exception e) {}
            }
        }
        println("rom labels: " + n);
        int fn = 0;
        for (String line : Files.readAllLines(Paths.get(dir, "entries.txt"))) {
            line = line.trim(); if (line.isEmpty()) continue;
            Address ad = toAddr(Long.parseLong(line, 16));
            if (!mem.contains(ad)) continue;
            if (getFunctionAt(ad) == null) {
                disassemble(ad);
                if (createFunction(ad, null) != null) fn++;
            }
        }
        println("functions from entries: " + fn);
    }
}
