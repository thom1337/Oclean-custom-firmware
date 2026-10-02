#!/usr/bin/env python3
"""fd.py ADDR [END|+LEN] : disassemble from exact ADDR (resync-safe) with l32r/call annotation.
If END omitted, stops at next function entry (from entries list) or +0x600."""
import sys, os, re, struct, subprocess, bisect, pickle

HERE = os.path.dirname(os.path.abspath(__file__))
RE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBJ = os.environ.get('XTENSA_OBJDUMP', os.path.expanduser('~/.espressif/tools/xtensa-esp32s3-elf/esp-12.2.0_20230208/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-objdump'))
segs = [
    ('drom', 0x3c110020, f'{RE}/seg0_3c110020.bin'),
    ('dram', 0x3fc99e00, f'{RE}/seg1_3fc99e00.bin'),
    ('iram0', 0x40374000, f'{RE}/seg2_40374000.bin'),
    ('irom', 0x42000020, f'{RE}/seg3_42000020.bin'),
    ('iram1', 0x4037aa08, f'{RE}/seg4_4037aa08.bin'),
]
data = {n: (b, open(p, 'rb').read(), p) for n, b, p in segs}

def seg_of(v):
    for n, (b, d, p) in data.items():
        if b <= v < b + len(d):
            return n
    return None

def rd32(a):
    n = seg_of(a)
    if n is None: return None
    b, d, p = data[n]
    o = a - b
    if o + 4 > len(d): return None
    return struct.unpack('<I', d[o:o+4])[0]

def cstr(a, maxlen=90):
    n = seg_of(a)
    if n is None: return None
    b, d, p = data[n]
    o = a - b; s = bytearray()
    while o < len(d) and len(s) < maxlen:
        c = d[o]
        if c == 0: break
        if c < 9 or (13 < c < 32) or c > 126: return None
        s.append(c); o += 1
    if len(s) >= 2:
        return s.decode('latin1').replace('\n', '\\n').replace('\r', '\\r')
    return None

names = {}
nf = os.path.join(HERE, 'names.txt')
if os.path.exists(nf):
    for line in open(nf):
        line = line.strip()
        if not line or line.startswith('#'): continue
        a, nm = line.split(None, 1)
        names[int(a, 16)] = nm
romf = os.path.join(HERE, 'rom.pkl')
if os.path.exists(romf):
    names.update({k: 'ROM:' + v for k, v in pickle.load(open(romf, 'rb')).items() if k not in names})

entries = []
ef = os.path.join(HERE, 'entries.txt')
if os.path.exists(ef):
    entries = sorted(int(x, 16) for x in open(ef).read().split())

def describe(v):
    if v is None: return ''
    if v in names: return names[v]
    s = seg_of(v)
    if s in ('drom', 'dram'):
        st = cstr(v)
        return f'{s} "{st}"' if st else s
    if s in ('irom', 'iram0', 'iram1'):
        return f'code'
    if 0x3fc88000 <= v < 0x3fd00000: return 'bss'
    if 0x60000000 <= v < 0x60100000: return 'periph'
    if 0x50000000 <= v < 0x50002000: return 'rtcmem'
    return ''

def dis(start, stop):
    n = seg_of(start)
    b, d, p = data[n]
    out = subprocess.run([OBJ, '-D', '-b', 'binary', '-m', 'xtensa', f'--adjust-vma={b:#x}',
                          f'--start-address={start:#x}', f'--stop-address={stop:#x}', p],
                         capture_output=True, text=True).stdout.splitlines()[7:]
    res = []
    for line in out:
        m = re.match(r'^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(\S+)\s*(.*)$', line)
        if not m:
            res.append(line); continue
        addr, raw, mn, ops = m.groups()
        ann = ''
        if mn == 'l32r':
            la = int(re.search(r'0x([0-9a-f]+)', ops).group(1), 16)
            v = rd32(la)
            if v is not None:
                ann = f'; ={v:#x} {describe(v)}'
        elif mn.startswith('call') and not mn.startswith('callx') and '0x' in ops:
            t = int(re.search(r'0x([0-9a-f]+)', ops).group(1), 16)
            ann = f'; {names.get(t, "")}'
        elif mn == 'j' and '0x' in ops:
            pass
        res.append(f'{addr}: {raw:<8} {mn:<8} {ops} {ann}'.rstrip())
    return res

if __name__ == '__main__':
    a = int(sys.argv[1], 16)
    if len(sys.argv) > 2:
        e = sys.argv[2]
        stop = a + int(e[1:], 16) if e.startswith('+') else int(e, 16)
    else:
        i = bisect.bisect_right(entries, a)
        stop = entries[i] if i < len(entries) else a + 0x600
        if stop - a > 0x1800: stop = a + 0x1800
    print(f'== {a:#x} .. {stop:#x}  {names.get(a, "")}')
    print('\n'.join(dis(a, stop)))
