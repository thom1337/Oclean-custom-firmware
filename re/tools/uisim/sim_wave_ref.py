#!/usr/bin/env python3
"""Cross-check of main/oem_wave.c against the stock generator 0x40377f70.

An independent model of the stock routine, written instruction by instruction from
`re/tools/fd.py 0x40377f70` (comments name the opcodes), with float32 arithmetic and
with the wave table, the swell steps, the gear table and the float literals read
straight from the stock image (re/seg0_3c110020.bin), not from the C source. It
replays what `sim_wave -d` dumps (every gear, full and halved duty, 400 chunks each,
brushing second = chunk / 20) and compares sample by sample.

    cc -std=gnu11 -ffp-contract=off -I main re/tools/uisim/sim_wave.c main/oem_wave.c -lm -o sim_wave
    ./sim_wave -d | python3 re/tools/uisim/sim_wave_ref.py
"""
import math, os, struct, sys
import numpy as np
f32 = np.float32
RE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
d = open(os.path.join(RE, 'seg0_3c110020.bin'), 'rb').read(); B = 0x3c110020
rd = lambda a, n: d[a - B:a - B + n]
WAVE = struct.unpack('100b', rd(0x3c11ad93, 100))
SWELL = list(rd(0x3c11abf3, 3))
GEAR = [rd(0x3c1194b8 + 7 * i, 7) for i in range(54)]
PI = struct.unpack('<d', struct.pack('<II', 0x53c8d4f1, 0x400921fb))[0]
K16383 = struct.unpack('<f', struct.pack('<I', 0x467ffc00))[0]
K98 = struct.unpack('<f', struct.pack('<I', 0x42c40000))[0]
K50 = struct.unpack('<f', struct.pack('<I', 0x42480000))[0]

def s8(v): v &= 0xff; return v - 256 if v & 0x80 else v
def s16(v): v &= 0xffff; return v - 65536 if v & 0x8000 else v

class St:  # the RAM bytes
    def __init__(s, freq, duty, typ):
        s.freq, s.base, s.type, s.amp, s.dir = freq, duty, typ, duty, 1
        s.pulse_on = s.sw_hold = s.sw_off = s.sw_idx = s.sw_state = 0

def chunk(s, elapsed):
    a2 = 24000 // s.freq                       # quos
    a6 = s.type
    a10_extra = 6 if a6 == 0x50 else 0         # movnez
    a4 = s16(s.amp)                            # l16si
    flag = 0
    out = []
    a7 = 0
    while a7 != 2 * a2:
        if a6 == 0x51:
            x = float(a7) * PI                 # floatunsidf, muldf3
            x = x / float(a2)                  # floatsidf, divdf3
            f0 = f32(f32(math.sin(x)) * f32(K16383))   # sin, truncdfsf2, mul.s
            a10 = 0
        else:
            idx = (a7 * 50 + 100) // a2 - 1    # addx4, addx4, add, addi 100, quou, addi -1
            idx = max(s16(idx), 0)             # sext 15, max with a5 = 0
            t = s8(WAVE[idx] & 0xff)
            f0 = f32(f32(f32(t) / f32(K98)) * f32(K16383))   # float.s, divsf3, mul.s
            a10 = a10_extra
        if a4 + a10 > 44:                      # bge a12(44), a11 -> skip
            flag = 1; a10 = 0; a4 = 50
        f1 = f32(f32(a4 + a10) * f0)           # float.s, mul.s
        f1 = f32(f1 / f32(K50))                # divsf3
        out.append(s16(int(np.trunc(f1))))     # trunc.s, s16i
        a7 += 2
    if flag: s.amp = a4
    if a6 == 0x21:
        if s.sw_state == 0:
            s.sw_off = (s.sw_off + SWELL[s.sw_idx % 3]) & 0xff; s.sw_idx = (s.sw_idx + 1) & 0xff
            if s8(s.sw_off) > 19: s.sw_state = 1; s.sw_off = 20; s.sw_idx = 0
        elif s.sw_state == 1:
            s.sw_hold = (s.sw_hold + 1) & 0xff
            if s.sw_hold > 29: s.sw_state = 2; s.sw_hold = 0
        elif s.sw_state == 2:
            s.sw_off = (s.sw_off - SWELL[s.sw_idx % 3]) & 0xff; s.sw_idx = (s.sw_idx + 1) & 0xff
            if s8(s.sw_off) < 1: s.sw_state = 0; s.sw_off = 0; s.sw_idx = 0
        s.amp = s16(s8(s.sw_off) + s.base)
        return out
    if a6 == 0x20:
        pass
    elif a6 == 0x1f:
        if elapsed < 5: s.pulse_on = 0; return out
        v = (elapsed * 0xaaab) & 0xffff
        v = ((v >> 1) & 0x7fff) | ((v << 15) & 0xffff)
        if v <= 0x2aaa: s.pulse_on ^= 1
        if not s.pulse_on: s.amp = s.base; return out
    else:
        return out
    if s.dir:
        s.amp = s16(s.amp - 1)
        if s.amp < s.base - 15: s.dir = 0
    else:
        s.amp = s16(s.amp + 1)
        if s.amp >= s.base: s.dir = 1
    return out

states = {}
bad = n_lines = 0
for line in sys.stdin:
    head, body = line.split(':')
    gear, half, c, el, n = map(int, head.split())
    key = (gear, half)
    if key not in states:
        e = GEAR[gear - 1]
        states[key] = St(e[0] * 10 + e[1] // 10, e[2] >> 1 if half else e[2], e[4])
    ref = chunk(states[key], el)
    got = list(map(int, body.split()))
    n_lines += 1
    if ref != got or n != len(ref):
        bad += 1
        if bad < 5: print('MISMATCH', head, [(i, a, b) for i, (a, b) in enumerate(zip(ref, got)) if a != b][:5])
print(f'{n_lines} chunks compared ({len(states)} gear/duty variants), {bad} mismatches')
sys.exit(1 if bad or not n_lines else 0)
