#!/usr/bin/env python3
"""Minimal Xtensa (ESP32-S3, windowed ABI) interpreter for running single leaf-ish
functions of the stock image on the host, e.g. to produce reference vectors for a C
port. It executes the instruction subset GCC emits for integer / soft-double /
single-float code; anything else raises. Instructions are decoded with objdump at the
exact PC (so literal pools between functions cannot desynchronise the decoder).

    from xt_emu import Emu
    e = Emu(); buf = e.alloc(236)
    r = e.call(0x42072bf0, buf)

ROM routines (memset, memcpy, libgcc soft float) are implemented natively as hooks.
More hooks can be added per address (e.hooks[addr] = fn(regs): arguments in
regs[10..15], result in regs[10]) to replace functions of the image that cannot run
here: bus access, printf, RTOS calls."""
import os, re, struct, subprocess

RE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBJ = os.environ.get('XTENSA_OBJDUMP', os.path.expanduser(
    '~/.espressif/tools/xtensa-esp32s3-elf/esp-12.2.0_20230208/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-objdump'))
SEGS = [(0x3c110020, 'seg0_3c110020.bin'), (0x3fc99e00, 'seg1_3fc99e00.bin'),
        (0x40374000, 'seg2_40374000.bin'), (0x42000020, 'seg3_42000020.bin'),
        (0x4037aa08, 'seg4_4037aa08.bin')]
M32 = 0xffffffff


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def d2w(x):
    lo, hi = struct.unpack('<II', struct.pack('<d', x))
    return lo, hi


def w2d(lo, hi):
    return struct.unpack('<d', struct.pack('<II', lo & M32, hi & M32))[0]


class Emu:
    RAM_BASE = 0x3fd00000        # scratch RAM: heap from the bottom, stack from the top
    RAM_SIZE = 0x20000

    def __init__(self):
        self.segs = []
        for base, name in SEGS:
            path = os.path.join(RE, name)
            self.segs.append((base, bytearray(open(path, 'rb').read()), path))
        self.ram = bytearray(self.RAM_SIZE)
        self.heap = self.RAM_BASE
        self.dec = {}
        self.hooks = {
            0x400011e8: self.h_memset,
            0x40002334: self.h_floatsidf, 0x40002418: self.h_muldf3, 0x40002184: self.h_adddf3,
            0x400022d4: self.h_fixdfsi, 0x40002274: self.h_divsf3, 0x400023dc: self.h_ltdf2,
            0x400023a0: self.h_gtdf2,
            0x400011f4: self.h_memcpy, 0x40002250: self.h_divdf3, 0x400024fc: self.h_subdf3,
            0x4000252c: self.h_truncdfsf2, 0x400022a4: self.h_extendsfdf2,
        }
        self.count = 0

    # ---- memory
    def _find(self, a, n):
        if self.RAM_BASE <= a and a + n <= self.RAM_BASE + self.RAM_SIZE:
            return self.ram, a - self.RAM_BASE
        for base, d, _ in self.segs:
            if base <= a and a + n <= base + len(d):
                return d, a - base
        raise RuntimeError('bad address %#x' % a)

    def rd(self, a, n):
        d, o = self._find(a, n)
        return int.from_bytes(d[o:o + n], 'little')

    def wr(self, a, n, v):
        d, o = self._find(a, n)
        d[o:o + n] = (v & ((1 << (8 * n)) - 1)).to_bytes(n, 'little')

    def read(self, a, n):
        d, o = self._find(a, n)
        return bytes(d[o:o + n])

    def write(self, a, data):
        d, o = self._find(a, len(data))
        d[o:o + len(data)] = data

    def map(self, base, size):
        """Add zero-filled RAM (e.g. the .bss of the image)."""
        self.segs.append((base, bytearray(size), None))

    def alloc(self, n):
        a = self.heap
        self.heap += (n + 15) & ~15
        return a

    # ---- ROM hooks: arguments in a10.., result in a10 (a11)
    def h_memset(self, r):
        self.write(r[10], bytes([r[11] & 0xff]) * r[12])

    def h_memcpy(self, r):
        self.write(r[10], self.read(r[11], r[12]))

    def h_divdf3(self, r):
        r[10], r[11] = d2w(w2d(r[10], r[11]) / w2d(r[12], r[13]))

    def h_subdf3(self, r):
        r[10], r[11] = d2w(w2d(r[10], r[11]) - w2d(r[12], r[13]))

    def h_truncdfsf2(self, r):
        r[10] = struct.unpack('<I', struct.pack('<f', w2d(r[10], r[11])))[0]

    def h_extendsfdf2(self, r):
        r[10], r[11] = d2w(struct.unpack('<f', struct.pack('<I', r[10]))[0])

    def h_floatsidf(self, r):
        r[10], r[11] = d2w(float(s32(r[10])))

    def h_muldf3(self, r):
        r[10], r[11] = d2w(w2d(r[10], r[11]) * w2d(r[12], r[13]))

    def h_adddf3(self, r):
        r[10], r[11] = d2w(w2d(r[10], r[11]) + w2d(r[12], r[13]))

    def h_fixdfsi(self, r):
        v = w2d(r[10], r[11])
        r[10] = max(-2 ** 31, min(2 ** 31 - 1, int(v))) & M32

    def h_divsf3(self, r):
        a = struct.unpack('<f', struct.pack('<I', r[10]))[0]
        b = struct.unpack('<f', struct.pack('<I', r[11]))[0]
        r[10] = struct.unpack('<I', struct.pack('<f', a / b))[0]

    def h_ltdf2(self, r):
        a, b = w2d(r[10], r[11]), w2d(r[12], r[13])
        r[10] = (-1 if a < b else (0 if a == b else 1)) & M32

    def h_gtdf2(self, r):
        a, b = w2d(r[10], r[11]), w2d(r[12], r[13])
        r[10] = (1 if a > b else (0 if a == b else -1)) & M32

    # ---- decoder
    def decode(self, pc):
        if pc in self.dec:
            return self.dec[pc]
        for base, d, path in self.segs:
            if base <= pc < base + len(d) and path:
                break
        else:
            raise RuntimeError('pc outside the image: %#x' % pc)
        out = subprocess.run([OBJ, '-D', '-b', 'binary', '-m', 'xtensa', '--adjust-vma=%#x' % base,
                              '--start-address=%#x' % pc, '--stop-address=%#x' % (pc + 0x80), path],
                             capture_output=True, text=True).stdout.splitlines()[7:]
        for line in out:
            m = re.match(r'^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(\S+)\s*(.*)$', line)
            if not m:
                break
            addr, raw, mn, ops = int(m.group(1), 16), m.group(2), m.group(3), m.group(4)
            ops = [o.strip() for o in re.sub(r'\(.*\)', '', ops).split(',')] if ops.strip() else []
            self.dec.setdefault(addr, (mn, ops, len(raw) // 2))
            if mn in ('j', 'retw.n', 'retw', 'jx', 'ret', 'ret.n'):
                break
        return self.dec[pc]

    # ---- execution
    def call(self, addr, *args, limit=2000000):
        """Call a windowed-ABI function like `call8 addr`; returns a2 of the callee."""
        r = [0] * 16
        r[1] = self.RAM_BASE + self.RAM_SIZE - 64
        for i, a in enumerate(args):
            r[10 + i] = a & M32
        self.fr = [0.0] * 16
        br = [0] * 16
        frames = []            # (caller regs, return pc)
        pc, ret_pc = addr, None
        lbeg = lend = lcount = 0
        sar = 0
        pending = None         # return pc of the call being entered (None = our caller)
        R = lambda o: int(o[1:])
        I = lambda o: int(o, 0)
        n = 0
        while True:
            n += 1
            if n > limit:
                raise RuntimeError('instruction limit at %#x' % pc)
            mn, o, ln = self.decode(pc)
            npc = pc + ln
            if mn == 'entry':
                new = [0] * 16
                new[0] = pending or 0
                new[1] = (r[1] - I(o[1])) & M32
                new[2:8] = r[10:16]
                frames.append((r, pending))
                r = new
            elif mn in ('retw.n', 'retw'):
                old, rpc = frames.pop()
                old[10:16] = r[2:8]
                r = old
                if rpc is None:
                    self.count += n
                    return r[10]
                npc = rpc
            elif mn == 'call8':
                if I(o[0]) in self.hooks:
                    self.hooks[I(o[0])](r)
                else:
                    pending = npc
                    npc = I(o[0])
            elif mn == 'callx8':
                t = r[R(o[0])]
                if t in self.hooks:
                    self.hooks[t](r)
                elif 0x40000000 <= t < 0x40060000:
                    raise RuntimeError('unhooked ROM call %#x at %#x' % (t, pc))
                else:
                    pending = npc
                    npc = t
            elif mn == 'l32r':
                r[R(o[0])] = self.rd(I(o[1]), 4)
            elif mn in ('l8ui', 'l16ui', 'l16si', 'l32i', 'l32i.n'):
                a = (r[R(o[1])] + I(o[2])) & M32
                if mn == 'l8ui':
                    v = self.rd(a, 1)
                elif mn == 'l16ui':
                    v = self.rd(a, 2)
                elif mn == 'l16si':
                    v = self.rd(a, 2)
                    v = (v - 0x10000 if v & 0x8000 else v) & M32
                else:
                    v = self.rd(a, 4)
                r[R(o[0])] = v
            elif mn in ('s8i', 's16i', 's32i', 's32i.n'):
                a = (r[R(o[1])] + I(o[2])) & M32
                self.wr(a, {'s8i': 1, 's16i': 2}.get(mn, 4), r[R(o[0])])
            elif mn in ('movi', 'movi.n'):
                r[R(o[0])] = I(o[1]) & M32
            elif mn == 'mov.n':
                r[R(o[0])] = r[R(o[1])]
            elif mn in ('add', 'add.n'):
                r[R(o[0])] = (r[R(o[1])] + r[R(o[2])]) & M32
            elif mn in ('addi', 'addi.n', 'addmi'):
                r[R(o[0])] = (r[R(o[1])] + I(o[2])) & M32
            elif mn == 'sub':
                r[R(o[0])] = (r[R(o[1])] - r[R(o[2])]) & M32
            elif mn == 'neg':
                r[R(o[0])] = (-r[R(o[1])]) & M32
            elif mn == 'or':
                r[R(o[0])] = r[R(o[1])] | r[R(o[2])]
            elif mn == 'xor':
                r[R(o[0])] = r[R(o[1])] ^ r[R(o[2])]
            elif mn == 'and':
                r[R(o[0])] = r[R(o[1])] & r[R(o[2])]
            elif mn == 'slli':
                r[R(o[0])] = (r[R(o[1])] << I(o[2])) & M32
            elif mn == 'srai':
                r[R(o[0])] = (s32(r[R(o[1])]) >> I(o[2])) & M32
            elif mn == 'srli':
                r[R(o[0])] = r[R(o[1])] >> I(o[2])
            elif mn == 'sext':
                b = I(o[2])
                v = r[R(o[1])] & ((1 << (b + 1)) - 1)
                r[R(o[0])] = (v - (1 << (b + 1)) if v >> b else v) & M32
            elif mn == 'extui':
                r[R(o[0])] = (r[R(o[1])] >> I(o[2])) & ((1 << I(o[3])) - 1)
            elif mn == 'mull':
                r[R(o[0])] = (r[R(o[1])] * r[R(o[2])]) & M32
            elif mn == 'mul16u':
                r[R(o[0])] = (r[R(o[1])] & 0xffff) * (r[R(o[2])] & 0xffff)
            elif mn == 'mul16s':
                a, b = r[R(o[1])] & 0xffff, r[R(o[2])] & 0xffff
                a = a - 0x10000 if a & 0x8000 else a
                b = b - 0x10000 if b & 0x8000 else b
                r[R(o[0])] = (a * b) & M32
            elif mn == 'mulsh':
                r[R(o[0])] = ((s32(r[R(o[1])]) * s32(r[R(o[2])])) >> 32) & M32
            elif mn == 'muluh':
                r[R(o[0])] = ((r[R(o[1])] * r[R(o[2])]) >> 32) & M32
            elif mn in ('quos', 'rems'):
                a, b = s32(r[R(o[1])]), s32(r[R(o[2])])
                if b == 0:
                    raise RuntimeError('divide by zero at %#x' % pc)
                q = abs(a) // abs(b)
                if (a < 0) != (b < 0):
                    q = -q
                r[R(o[0])] = (q if mn == 'quos' else a - q * b) & M32
            elif mn in ('quou', 'remu'):
                a, b = r[R(o[1])], r[R(o[2])]
                if b == 0:
                    raise RuntimeError('divide by zero at %#x' % pc)
                r[R(o[0])] = (a // b) if mn == 'quou' else (a % b)
            elif mn == 'maxu':
                r[R(o[0])] = max(r[R(o[1])], r[R(o[2])])
            elif mn == 'minu':
                r[R(o[0])] = min(r[R(o[1])], r[R(o[2])])
            elif mn == 'max':
                r[R(o[0])] = max(s32(r[R(o[1])]), s32(r[R(o[2])])) & M32
            elif mn == 'min':
                r[R(o[0])] = min(s32(r[R(o[1])]), s32(r[R(o[2])])) & M32
            elif mn == 'abs':
                r[R(o[0])] = abs(s32(r[R(o[1])])) & M32
            elif mn == 'movnez':
                if r[R(o[2])] != 0:
                    r[R(o[0])] = r[R(o[1])]
            elif mn == 'moveqz':
                if r[R(o[2])] == 0:
                    r[R(o[0])] = r[R(o[1])]
            elif mn == 'movltz':
                if s32(r[R(o[2])]) < 0:
                    r[R(o[0])] = r[R(o[1])]
            elif mn == 'movgez':
                if s32(r[R(o[2])]) >= 0:
                    r[R(o[0])] = r[R(o[1])]
            elif mn == 'ssr':
                sar = r[R(o[0])] & 31
            elif mn == 'ssl':
                sar = 32 - (r[R(o[0])] & 31)
            elif mn == 'ssai':
                sar = I(o[0]) & 31
            elif mn == 'sra':
                r[R(o[0])] = (s32(r[R(o[1])]) >> sar) & M32
            elif mn == 'srl':
                r[R(o[0])] = r[R(o[1])] >> sar
            elif mn == 'sll':
                r[R(o[0])] = (r[R(o[1])] << (32 - sar)) & M32
            elif mn == 'j':
                npc = I(o[0])
            elif mn in ('beqz', 'beqz.n', 'bnez', 'bnez.n', 'bgez', 'bltz'):
                v = s32(r[R(o[0])])
                if {'beqz': v == 0, 'beqz.n': v == 0, 'bnez': v != 0, 'bnez.n': v != 0,
                        'bgez': v >= 0, 'bltz': v < 0}[mn]:
                    npc = I(o[1])
            elif mn in ('beq', 'bne', 'blt', 'bge', 'bltu', 'bgeu'):
                a, b = r[R(o[0])], r[R(o[1])]
                sa, sb = s32(a), s32(b)
                if {'beq': a == b, 'bne': a != b, 'blt': sa < sb, 'bge': sa >= sb,
                        'bltu': a < b, 'bgeu': a >= b}[mn]:
                    npc = I(o[2])
            elif mn in ('beqi', 'bnei', 'blti', 'bgei', 'bltui', 'bgeui'):
                a, b = r[R(o[0])], I(o[1])
                sa = s32(a)
                if {'beqi': sa == b, 'bnei': sa != b, 'blti': sa < b, 'bgei': sa >= b,
                        'bltui': a < (b & M32), 'bgeui': a >= (b & M32)}[mn]:
                    npc = I(o[2])
            elif mn in ('bbci', 'bbsi'):
                bit = (r[R(o[0])] >> I(o[1])) & 1
                if bit == (1 if mn == 'bbsi' else 0):
                    npc = I(o[2])
            elif mn == 'loop':
                lcount = (r[R(o[0])] - 1) & M32
                lbeg, lend = npc, I(o[1])
            elif mn == 'memw' or mn == 'nop' or mn == 'nop.n':
                pass
            # ---- single-precision FPU
            elif mn in ('float.s', 'ufloat.s'):
                v = s32(r[R(o[1])]) if mn == 'float.s' else r[R(o[1])]
                self.fr[R(o[0])] = f32(v / float(1 << I(o[2])))
            elif mn == 'trunc.s':
                v = self.fr[R(o[1])] * (1 << I(o[2]))
                if v != v:
                    v = 0x7fffffff
                r[R(o[0])] = max(-2 ** 31, min(2 ** 31 - 1, int(v))) & M32
            elif mn in ('lsi', 'ssi'):
                a = (r[R(o[1])] + I(o[2])) & M32
                if mn == 'lsi':
                    self.fr[R(o[0])] = struct.unpack('<f', self.read(a, 4))[0]
                else:
                    self.write(a, struct.pack('<f', self.fr[R(o[0])]))
            elif mn in ('ole.s', 'olt.s', 'oeq.s'):
                a, b = self.fr[R(o[1])], self.fr[R(o[2])]
                br[R(o[0])] = int({'ole.s': a <= b, 'olt.s': a < b, 'oeq.s': a == b}[mn])
            elif mn in ('bt', 'bf'):
                if br[R(o[0])] == (1 if mn == 'bt' else 0):
                    npc = I(o[1])
            elif mn == 'wfr':
                self.fr[R(o[0])] = struct.unpack('<f', struct.pack('<I', r[R(o[1])]))[0]
            elif mn == 'rfr':
                r[R(o[0])] = struct.unpack('<I', struct.pack('<f', self.fr[R(o[1])]))[0]
            elif mn == 'neg.s':
                self.fr[R(o[0])] = -self.fr[R(o[1])]
            elif mn == 'mov.s':
                self.fr[R(o[0])] = self.fr[R(o[1])]
            elif mn == 'add.s':
                self.fr[R(o[0])] = f32(self.fr[R(o[1])] + self.fr[R(o[2])])
            elif mn == 'sub.s':
                self.fr[R(o[0])] = f32(self.fr[R(o[1])] - self.fr[R(o[2])])
            elif mn == 'mul.s':
                self.fr[R(o[0])] = f32(self.fr[R(o[1])] * self.fr[R(o[2])])
            elif mn == 'madd.s':      # fr += fs * ft, rounded once (fused)
                self.fr[R(o[0])] = f32(self.fr[R(o[0])] + self.fr[R(o[1])] * self.fr[R(o[2])])
            else:
                raise RuntimeError('unsupported instruction at %#x: %s %s' % (pc, mn, o))
            if npc == lend and lend:
                if lcount:
                    lcount -= 1
                    npc = lbeg
                else:
                    lend = 0
            pc = npc
