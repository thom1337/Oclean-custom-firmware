#!/usr/bin/env python3
"""Make the Ghidra export readable: resolve literal-pool pointers to names/strings/globals."""
import re, struct, os, sys
# usage: postproc.py WORKDIR   (WORKDIR holds decomp/all.c, names.txt, ...; segments are in re/)
S = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()
RE = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'
SEGS = [(0x3c110020, open(RE + 'seg0_3c110020.bin', 'rb').read()),
        (0x3fc99e00, open(RE + 'seg1_3fc99e00.bin', 'rb').read()),
        (0x40374000, open(RE + 'seg2_40374000.bin', 'rb').read()),
        (0x42000020, open(RE + 'seg3_42000020.bin', 'rb').read()),
        (0x4037aa08, open(RE + 'seg4_4037aa08.bin', 'rb').read())]
def seg(a):
    for b, d in SEGS:
        if b <= a < b + len(d): return b, d
    return None, None
def rd32(a):
    b, d = seg(a)
    if d is None or a - b + 4 > len(d): return None
    return struct.unpack_from('<I', d, a - b)[0]
def cstr(a, n=300):
    b, d = seg(a)
    if d is None: return None
    o = a - b; e = d.find(b'\0', o, o + n)
    if e <= o: return None
    s = d[o:e]
    if any(c < 9 or (13 < c < 32) or c > 126 for c in s) or len(s) < 2: return None
    return s.decode('latin1')
names = {}
for f in ('names.txt', 'names_manual.txt', 'app_names.txt', 'rom_ld.txt', 'rom_syms.txt'):
    p = os.path.join(S, f)
    if os.path.exists(p):
        for line in open(p):
            q = line.split()
            if len(q) >= 2 and not q[0].startswith('#'): names.setdefault(int(q[0], 16), q[1])
dnames = {}
for f in ('data_names.txt', 'app_data_names.txt'):
    p = os.path.join(S, f)
    if os.path.exists(p):
        for line in open(p):
            q = line.split()
            if len(q) >= 2 and not q[0].startswith('#'): dnames[int(q[0], 16)] = q[1]

def resolve(m):
    la = int(m.group(2), 16)
    v = rd32(la)
    if v is None: return m.group(0)
    if v in names: return names[v]
    if v in dnames: return '&' + dnames[v]
    if 0x3c000000 <= v < 0x3c200000 or 0x3fc99e00 <= v < 0x3fca1d70:
        s = cstr(v)
        if s is not None and m.group(1).startswith('s_'):
            return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\r', '\\r') + '"'
    if 0x40370000 <= v < 0x40390000 or 0x42000000 <= v < 0x42200000:
        return 'FUN_%08x' % v
    if 0x3c000000 <= v < 0x3c200000: return '&ro_%08x' % v
    if 0x3fc80000 <= v < 0x3fd00000: return '&g_%08x' % v
    if 0x50000000 <= v < 0x50002000: return '&rtc_%08x' % v
    if 0x60000000 <= v < 0x60100000: return '&periph_%08x' % v
    return '0x%x' % v

src = open(os.path.join(S, 'decomp', 'all.c')).read()
src = re.sub(r'\bPTR_(\w+?)_([0-9a-f]{8})\b', resolve, src)
src = re.sub(r'\bDAT_([0-9a-f]{8})\b', lambda m: (lambda v: (lambda n: n if n else 'lit_%s{=0x%x}' % (m.group(1), rd32(v) or 0))(None))(int(m.group(1), 16))
             if 0x42000000 <= int(m.group(1), 16) < 0x42200000 or 0x40370000 <= int(m.group(1), 16) < 0x40390000 else
             ('g_' + m.group(1) if int(m.group(1), 16) >= 0x3fc80000 and int(m.group(1), 16) < 0x3fd00000 else m.group(0)), src)
src = re.sub(r'\(\*\(code \*\)([A-Za-z_][\w$]*)\)\(', r'\1(', src)
# rename FUN_ with known names (app_names)
src = re.sub(r'\bFUN_([0-9a-f]{8})\b', lambda m: names.get(int(m.group(1), 16), m.group(0)), src)
open(os.path.join(S, 'decomp', 'all2.c'), 'w').write(src)
os.makedirs(os.path.join(S, 'decomp', 'f2'), exist_ok=True)
n = 0
for p in re.split(r'\n(?=// ===== [0-9a-f]{8} )', src):
    m = re.match(r'// ===== ([0-9a-f]{8}) (\S+)', p.strip())
    if not m: continue
    open(os.path.join(S, 'decomp', 'f2', m.group(1) + '.c'), 'w').write(p.strip() + '\n'); n += 1
print('functions', n)
