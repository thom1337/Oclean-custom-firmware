#!/usr/bin/env python3
"""Name functions/globals in the stock Oclean image by matching against unlinked objects
of a reference ESP-IDF v5.1.1 -Os build (plus IDF's prebuilt archives).

Instruction streams are tokenised so link-time differences vanish (literal slots, call
targets, branch offsets, longcall relaxation, nops). Candidates are confirmed by the
strings their literals point at, then names are propagated along calls / literals.

usage (IDF python env): match2.py <dir-or-.a> ... ; writes names.txt, data_names.txt
"""
import sys, os, struct, subprocess, tempfile, collections, glob, hashlib, pickle
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

AR = os.environ.get('XTENSA_AR', os.path.expanduser('~/.espressif/tools/xtensa-esp32s3-elf/esp-12.2.0_20230208/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-ar'))
RE = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'
SEGS = [(0x3c110020, open(RE + 'seg0_3c110020.bin', 'rb').read()),
        (0x3fc99e00, open(RE + 'seg1_3fc99e00.bin', 'rb').read()),
        (0x40374000, open(RE + 'seg2_40374000.bin', 'rb').read()),
        (0x42000020, open(RE + 'seg3_42000020.bin', 'rb').read()),
        (0x4037aa08, open(RE + 'seg4_4037aa08.bin', 'rb').read())]
CODE = [SEGS[2], SEGS[3], SEGS[4]]

def seg(a):
    for b, d in SEGS:
        if b <= a < b + len(d):
            return b, d
    return None, None
def rd32(a):
    b, d = seg(a)
    if d is None or a - b + 4 > len(d): return None
    return struct.unpack_from('<I', d, a - b)[0]
def cstr(a, n=200):
    b, d = seg(a)
    if d is None: return None
    o = a - b
    e = d.find(b'\0', o, o + n)
    if e < 0: return None
    return d[o:e]

def tokenize(code, base=0, lit=None, maxtok=100000):
    """-> list of (token, kind, info). kind: None | 'L' (l32r; info = literal value/addr or code
    offset) | 'C' (call; info = target or code offset of the call site)."""
    toks = []
    i = 0
    n = len(code)
    while i < n and len(toks) < maxtok:
        b0 = code[i]
        op0 = b0 & 0xF
        ln = 2 if 8 <= op0 <= 13 else 3
        if i + ln > n:
            break
        b1 = code[i + 1]
        b2 = code[i + 2] if ln == 3 else 0
        if ln == 2:
            # canonicalise density (narrow) instructions to their wide equivalents: the
            # linker narrows/widens them to keep alignment after relaxation
            t = b0 >> 4; s_ = b1 & 0xF; r = b1 >> 4
            if op0 == 8:   toks.append((bytes([0x02 | (t << 4), s_ | 0x20, r]), None, None))
            elif op0 == 9: toks.append((bytes([0x02 | (t << 4), s_ | 0x60, r]), None, None))
            elif op0 == 0xA: toks.append((bytes([t << 4, b1, 0x80]), None, None))
            elif op0 == 0xB:
                imm = 0xFF if t == 0 else t
                toks.append((bytes([0x02 | (r << 4), s_ | 0xC0, imm]), None, None))
            elif op0 == 0xC:
                if t & 8:
                    toks.append((bytes([0x56 if t & 4 else 0x16, s_]), None, None))
                else:
                    imm = ((t & 7) << 4) | r
                    if imm >= 96: imm -= 128
                    imm &= 0xFFF
                    toks.append((bytes([0x02 | (s_ << 4), 0xA0 | (imm >> 8), imm & 0xFF]), None, None))
            else:   # 0xD
                if r == 0:
                    toks.append((bytes([s_ << 4, s_ | (t << 4), 0x20]), None, None))
                elif r == 0xF and t == 0: toks.append((b'\x80\x00\x00', None, None))
                elif r == 0xF and t == 1: toks.append((b'\x90\x00\x00', None, None))
                elif r == 0xF and t == 3: pass
                else: toks.append((code[i:i + 2], None, None))
        elif op0 == 1:                                           # l32r
            imm = b1 | (b2 << 8)
            toks.append((bytes([0x01, b0 >> 4]), 'L', (i, imm)))
        elif op0 == 5:                                           # callN
            off = (b0 >> 6) | (b1 << 2) | (b2 << 10)
            if off & 0x20000: off -= 0x40000
            toks.append((bytes([0x05, (b0 >> 4) & 3]), 'C', (i, off)))
        elif op0 == 0 and b2 == 0 and (b0 & 0xC0) == 0xC0 and (b0 & 0x30) != 0 or \
                (op0 == 0 and b2 == 0 and b0 == 0xC0):           # callxN aS
            nn = (b0 >> 4) & 3
            s = b1 & 0xF
            if toks and toks[-1][1] == 'L' and toks[-1][0][1] == s and (b1 >> 4) == 0:
                li = toks[-1][2]
                toks[-1] = (bytes([0x05, nn]), 'C', ('lit', li))
            else:
                toks.append((code[i:i + 3], None, None))
        elif op0 == 0 and b0 == 0xF0 and b1 == 0x20 and b2 == 0:   # nop
            i += 3; continue
        elif op0 == 6:
            nfld = (b0 >> 4) & 3
            m = b0 >> 6
            if nfld == 0:                                        # j
                toks.append((b'\x06', None, None))
            elif nfld == 1:                                      # beqz/bnez/bltz/bgez
                toks.append((bytes([b0, b1 & 0x0F]), None, None))
            elif nfld == 3 and m == 0:                           # entry
                toks.append((code[i:i + 3], None, None))
            else:                                                # bi / loop / bt / bf
                toks.append((bytes([b0, b1]), None, None))
        elif op0 == 7:                                           # bcc
            toks.append((bytes([b0, b1]), None, None))
        else:
            toks.append((code[i:i + 3], None, None))
        i += ln
        # after an unconditional transfer the assembler/linker may pad with zero bytes
        uncond = (ln == 3 and ((op0 == 6 and (b0 >> 4) & 3 == 0) or (op0 == 0 and b2 == 0 and b0 in (0x80, 0x90)) or
                               (op0 == 0 and b2 == 0 and b0 == 0xA0 and (b1 >> 4) == 0))) or \
                 (ln == 2 and op0 == 0xD and (b1 >> 4) == 0xF and (b0 >> 4) in (0, 1))
        if uncond:
            while i < n and code[i] == 0:
                i += 1
    return toks

# ---------------------------------------------------------------- reference objects
class RefFn:
    __slots__ = ('name', 'obj', 'toks', 'ann', 'size')

def load_obj(path, tag, out):
    try:
        f = ELFFile(open(path, 'rb'))
    except Exception:
        return
    symtab = f.get_section_by_name('.symtab')
    if symtab is None: return
    syms = list(symtab.iter_symbols())
    secs = list(f.iter_sections())
    relocs = {}
    for s in secs:
        if isinstance(s, RelocationSection):
            relocs[s['sh_info']] = s
    sec_data = {}
    def data(idx):
        if idx not in sec_data:
            sec_data[idx] = secs[idx].data() if secs[idx]['sh_type'] != 'SHT_NOBITS' else b''
        return sec_data[idx]
    def sym_target(sym_idx, addend):
        s = syms[sym_idx]
        if s['st_info']['type'] == 'STT_SECTION':
            shndx = s['st_shndx']
            if isinstance(shndx, int):
                sec = secs[shndx]
                if sec.name.startswith('.rodata') and 'str' in sec.name:
                    d = data(shndx)
                    e = d.find(b'\0', addend)
                    if e >= 0: return ('str', d[addend:e])
                # static function / data inside a section: find a symbol at that offset
                for t in syms:
                    if t['st_shndx'] == shndx and t['st_value'] == addend and t.name and t['st_info']['type'] in ('STT_FUNC', 'STT_OBJECT'):
                        return ('sym', t.name)
                return ('sec', sec.name + '+%x' % addend)
            return None
        if s.name:
            return ('sym', s.name) if addend == 0 else ('symoff', '%s+%x' % (s.name, addend))
        return None
    for s in syms:
        if s['st_info']['type'] != 'STT_FUNC' or s['st_size'] < 9: continue
        shndx = s['st_shndx']
        if not isinstance(shndx, int): continue
        d = data(shndx)
        code = d[s['st_value']: s['st_value'] + s['st_size']]
        if len(code) != s['st_size']: continue
        toks = tokenize(code)
        if len(toks) < 3: continue
        # relocations for this text section
        rel = relocs.get(shndx)
        lit_of = {}     # code offset -> ('lit', sec idx, off) / callee
        if rel is not None:
            for r in rel.iter_relocations():
                off = r['r_offset'] - s['st_value']
                if off < 0 or off >= s['st_size']: continue
                t = r['r_info_type']
                if t == 20:      # SLOT0_OP
                    lit_of.setdefault(off, []).append(('slot', r['r_info_sym'], r['r_addend']))
                elif t == 11:    # ASM_EXPAND -> callee
                    lit_of.setdefault(off, []).append(('expand', r['r_info_sym'], r['r_addend']))
        ann = []
        for tok, kind, info in toks:
            a = None
            if kind == 'C':
                off = info[1][0] if info[0] == 'lit' else info[0]
                for k, si, ad in lit_of.get(off, []):
                    if k == 'expand':
                        a = sym_target(si, ad)
                    elif k == 'slot' and a is None and info[0] != 'lit':
                        a = sym_target(si, ad)         # direct call to a symbol
                if a is None and info[0] == 'lit':
                    kind = 'Lc'
            if kind in ('L', 'Lc'):
                off = info[1][0] if kind == 'Lc' else info[0]
                for k, si, ad in lit_of.get(off, []):
                    if k != 'slot': continue
                    ls = syms[si]
                    lsec = ls['st_shndx']
                    if not isinstance(lsec, int): continue
                    loff = ls['st_value'] + ad
                    lrel = relocs.get(lsec)
                    if lrel is None: continue
                    for r2 in lrel.iter_relocations():
                        if r2['r_offset'] == loff and r2['r_info_type'] == 1:
                            base = struct.unpack_from('<I', data(lsec), loff)[0] if loff + 4 <= len(data(lsec)) else 0
                            a = sym_target(r2['r_info_sym'], r2['r_addend'] + base)
                            break
            ann.append(a)
        fn = RefFn()
        fn.name = s.name; fn.obj = tag; fn.toks = tuple(t[0] for t in toks); fn.ann = ann; fn.size = s['st_size']
        out.append(fn)

def load_archive(path, out):
    tmp = tempfile.mkdtemp(prefix='ar_')
    subprocess.run([AR, 'x', os.path.abspath(path)], cwd=tmp, stderr=subprocess.DEVNULL)
    for root, _, files in os.walk(tmp):
        for fn in files:
            load_obj(os.path.join(root, fn), os.path.basename(path) + ':' + fn, out)
    subprocess.run(['rm', '-rf', tmp])

cache = 'ref_fns.pkl'
if os.path.exists(cache) and '--rebuild' not in sys.argv:
    ref = pickle.load(open(cache, 'rb'))
else:
    ref = []
    for p in sys.argv[1:]:
        if p.startswith('--'): continue
        paths = [p] if p.endswith('.a') else sorted(glob.glob(os.path.join(p, '**', '*.a'), recursive=True))
        for a in paths:
            if '/bootloader/' in a: continue
            n0 = len(ref)
            load_archive(a, ref)
            sys.stderr.write('%s: %d fns\n' % (os.path.basename(a), len(ref) - n0))
    pickle.dump([(r.name, r.obj, r.toks, r.ann, r.size) for r in ref], open(cache, 'wb'))
    ref = pickle.load(open(cache, 'rb'))
print('reference functions:', len(ref))

# ---------------------------------------------------------------- stock side
entries = set()
for base, d in CODE:
    i = d.find(b'\x36')
    while i >= 0:
        if i + 3 <= len(d) and (d[i + 1] & 0x0F) == 1 and d[i + 2] < 0x10:
            entries.add(base + i)
        i = d.find(b'\x36', i + 1)
print('stock entry candidates:', len(entries))

stock_tok = {}
def stoks(addr, need):
    cur = stock_tok.get(addr)
    if cur is not None and len(cur) >= need:
        return cur
    b, d = seg(addr)
    o = addr - b
    toks = tokenize(d[o:o + need * 4 + 16], maxtok=need + 2)
    res = []
    for tok, kind, info in toks:
        if kind == 'L':
            pc = addr + info[0]
            la = ((pc + 3) & ~3) + ((info[1] | ~0xFFFF) << 2)
            res.append((tok, 'L', rd32(la & 0xFFFFFFFF)))
        elif kind == 'C':
            if info[0] == 'lit':
                pc = addr + info[1][0]
                la = ((pc + 3) & ~3) + ((info[1][1] | ~0xFFFF) << 2)
                res.append((tok, 'C', rd32(la & 0xFFFFFFFF)))
            else:
                pc = addr + info[0]
                res.append((tok, 'C', ((pc & ~3) + (info[1] << 2) + 4) & 0xFFFFFFFF))
        else:
            res.append((tok, None, None))
    stock_tok[addr] = res
    return res

KEY = 5
index = collections.defaultdict(list)
for r in ref:
    name, obj, toks, ann, size = r
    if len(toks) >= KEY and size >= 14:
        index[toks[:KEY]].append(r)

cands = collections.defaultdict(list)    # stock addr -> [(ref, strscore, strbad)]
for a in sorted(entries):
    st = stoks(a, KEY)
    if len(st) < KEY: continue
    key = tuple(t[0] for t in st[:KEY])
    for r in index.get(key, ()):
        name, obj, toks, ann, size = r
        st = stoks(a, len(toks))
        if len(st) < len(toks): continue
        if any(st[i][0] != toks[i] for i in range(len(toks))): continue
        good = bad = 0
        for i, an in enumerate(ann):
            if an and an[0] == 'str' and st[i][1] in ('L', 'C') and st[i][2] is not None:
                s = cstr(st[i][2])
                if s == an[1]: good += 1
                else: bad += 1
        cands[a].append((r, good, bad))

# ---------------------------------------------------------------- resolve
names = {}          # stock addr -> name
votes = collections.defaultdict(collections.Counter)    # addr -> name votes (from calls/literals)
def accept(a, r):
    names[a] = r[0]
by_name = collections.defaultdict(list)
for a, cl in cands.items():
    cl = [c for c in cl if c[2] == 0]
    if not cl: continue
    best = max(cl, key=lambda c: (c[1], len(c[0][2])))
    top = [c for c in cl if (c[1], len(c[0][2])) == (best[1], len(best[0][2]))]
    nm = set(c[0][0] for c in top)
    cands[a] = top
    for c in top: by_name[c[0][0]].append(a)

for rnd in range(6):
    changed = 0
    for a, top in cands.items():
        if a in names: continue
        nm = sorted(set(c[0][0] for c in top))
        pick = None
        if len(nm) == 1 and len(set(by_name[nm[0]])) == 1 and (top[0][1] > 0 or len(top[0][0][2]) >= 12):
            pick = top[0]
        else:
            v = votes.get(a)
            if v:
                vn, vc = v.most_common(1)[0]
                for c in top:
                    if c[0][0] == vn: pick = c; break
        if pick is None: continue
        accept(a, pick[0]); changed += 1
        name, obj, toks, ann, size = pick[0]
        st = stoks(a, len(toks))
        for i, an in enumerate(ann):
            if an and an[0] == 'sym' and st[i][2] is not None and st[i][1] in ('L', 'C'):
                votes[st[i][2]][an[1]] += 1
    print('round', rnd, 'accepted', changed, 'total', len(names))
    if not changed: break

fn_names = dict(names)
data_names = {}
for a, v in votes.items():
    vn, vc = v.most_common(1)[0]
    tot = sum(v.values())
    if vc * 3 < tot * 2: continue          # need a clear majority
    if a in fn_names: continue
    b, d = seg(a)
    if any(base <= a < base + len(dd) for base, dd in CODE):
        fn_names[a] = vn
    elif 0x3c000000 <= a < 0x3fd00000 or 0x60000000 <= a < 0x60100000 or 0x40000000 <= a < 0x40060000 or 0x50000000 <= a < 0x50002000:
        data_names[a] = vn
with open('names.txt', 'w') as o:
    for a in sorted(fn_names): o.write('%08x %s\n' % (a, fn_names[a]))
with open('data_names.txt', 'w') as o:
    for a in sorted(data_names): o.write('%08x %s\n' % (a, data_names[a]))
print('function names', len(fn_names), '(body-matched %d)' % len(names), 'data names', len(data_names))

# ---------------------------------------------------------------- __FUNCTION__ string pass
import re as _re
ref_names = set(r[0] for r in ref)
used = set(fn_names.values())
sorted_entries = sorted(entries)
extra = {}
ident = _re.compile(rb'^[A-Za-z_][A-Za-z0-9_]{5,}$')
for k, a in enumerate(sorted_entries):
    if a in fn_names: continue
    end = sorted_entries[k + 1] if k + 1 < len(sorted_entries) else a + 0x400
    if end - a > 0x1000: end = a + 0x1000
    b, d = seg(a)
    if seg(end - 1)[0] != b: continue
    cnt = collections.Counter()
    for tok, kind, info in tokenize(d[a - b:end - b]):
        if kind != 'L': continue
        pc = a + info[0]
        la = ((pc + 3) & ~3) + ((info[1] | ~0xFFFF) << 2)
        v = rd32(la & 0xFFFFFFFF)
        if v is None: continue
        s = cstr(v, 80)
        if s and ident.match(s) and s.decode() in ref_names:
            cnt[s.decode()] += 1
    if len(cnt) == 1:
        nm = next(iter(cnt))
        if nm not in used:
            extra.setdefault(nm, []).append(a)
n_extra = 0
with open('names.txt', 'a') as o:
    for nm, al in extra.items():
        if len(al) == 1:
            o.write('%08x %s\n' % (al[0], nm)); n_extra += 1
print('named via __FUNCTION__ strings:', n_extra)
