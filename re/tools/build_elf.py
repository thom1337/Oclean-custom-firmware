#!/usr/bin/env python3
"""Wrap the stock ota.bin segments in a minimal ELF32 (Xtensa LE) so Ghidra can load it."""
import struct, sys

src = sys.argv[1] if len(sys.argv) > 1 else 'ota.bin'
out = sys.argv[2] if len(sys.argv) > 2 else 'stock.elf'
d = open(src, 'rb').read()
nseg = d[1]
entry = struct.unpack_from('<I', d, 4)[0]
off = 24
segs = []
for i in range(nseg):
    la, ln = struct.unpack_from('<II', d, off)
    segs.append((la, d[off + 8: off + 8 + ln]))
    off += 8 + ln

def kind(a):
    if 0x3c000000 <= a < 0x3e000000: return ('.flash.rodata', 0x2)          # A
    if 0x3fc80000 <= a < 0x3fd00000: return ('.dram0.data', 0x3)            # WA
    if 0x40370000 <= a < 0x40380000: return ('.iram0.text', 0x6)            # AX
    if 0x42000000 <= a < 0x44000000: return ('.flash.text', 0x6)
    if 0x50000000 <= a < 0x50002000: return ('.rtc.data', 0x3)
    if 0x600fe000 <= a < 0x60100000: return ('.rtc.fast', 0x3)
    return ('.seg', 0x3)

names = []
seen = {}
for la, b in segs:
    n, fl = kind(la)
    seen[n] = seen.get(n, 0) + 1
    if seen[n] > 1: n = '%s%d' % (n, seen[n])
    names.append((n, fl))

shstr = b'\0'
name_off = []
for n, _ in names:
    name_off.append(len(shstr)); shstr += n.encode() + b'\0'
shstr_name = len(shstr); shstr += b'.shstrtab\0'

EH, PH, SH = 52, 32, 40
phoff = EH
data_off = EH + PH * len(segs)
blobs = b''
offs = []
for la, b in segs:
    pad = (-(data_off + len(blobs))) % 16
    blobs += b'\0' * pad
    offs.append(data_off + len(blobs))
    blobs += b
shstr_off = data_off + len(blobs)
blobs += shstr
pad = (-(data_off + len(blobs))) % 4
blobs += b'\0' * pad
shoff = data_off + len(blobs)
shnum = len(segs) + 2

eh = b'\x7fELF' + bytes([1, 1, 1, 0]) + b'\0' * 8
eh += struct.pack('<HHIIIIIHHHHHH', 2, 94, 1, entry, phoff, shoff, 0x300, EH, PH, len(segs), SH, shnum, shnum - 1)
ph = b''
for (la, b), o, (n, fl) in zip(segs, offs, names):
    pf = 4 | (2 if fl & 1 else 0) | (1 if fl & 4 else 0)
    ph += struct.pack('<IIIIIIII', 1, o, la, la, len(b), len(b), pf, 4)
sh = b'\0' * SH
for (la, b), o, (n, fl), no in zip(segs, offs, names, name_off):
    sh += struct.pack('<IIIIIIIIII', no, 1, fl, la, o, len(b), 0, 0, 4, 0)
sh += struct.pack('<IIIIIIIIII', shstr_name, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
open(out, 'wb').write(eh + ph + blobs + sh)
print('wrote', out, 'entry', hex(entry), [(n, hex(la), hex(len(b))) for (la, b), (n, _) in zip(segs, names)])
