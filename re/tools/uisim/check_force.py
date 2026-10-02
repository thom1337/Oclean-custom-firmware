#!/usr/bin/env python3
"""Differential test of main/oem_force.c against the stock machine code.

The stock firmware computes the brushing force with a closed Awinic library. This
script executes that library straight from the stock image (segments in re/) with the
Xtensa interpreter re/tools/xt_emu.py, feeds the same sample streams to the C port
(re/tools/uisim/sim_force.c in pipe mode) and compares the complete 236-byte library
state after every sample.

    python3 re/tools/uisim/check_force.py [--quick] [--seed N] [--exe ./sim_force]

--exe uses an already built sim_force (e.g. one built with --coverage, to see with
gcov which branches of the port the streams reach).

Needs the xtensa objdump of the ESP-IDF toolchain (XTENSA_OBJDUMP or the default
~/.espressif path) and a host C compiler."""
import os, random, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, 're', 'tools'))
from xt_emu import Emu

# stock entry points
F_SIZE, F_RESET0, F_PARAMS, F_SETCOEF, F_RESET, F_STEP, F_GET_FORCE = (
    0x42103868, 0x42072bf0, 0x42072b44, 0x42103838, 0x42072b30, 0x42072c44, 0x42103774)
FILTER_CFG = 0x3c11c8da      # the 4 bytes the stock driver passes with every sample


class Stock:
    def __init__(self):
        self.e = Emu()
        e = self.e
        assert e.call(F_SIZE) == 236
        self.st, self.par, self.tmp, self.cfg = e.alloc(236), e.alloc(40), e.alloc(8), e.alloc(8)
        e.write(self.cfg, e.read(FILTER_CFG, 4))

    def init(self, noise, coef, pos_to, neg_to, thr_press, thr_release, thr_track, quiet_lim, thr_heavy, heavy_n):
        e = self.e
        e.write(self.st, bytes(236))
        e.call(F_RESET0, self.st)
        # parameter block of 0x42027894 (literals 0x42002d2c..: 01 01, 54, 34, 20, 10, 10,
        # 20, 60, 1600, 8, noise, 20, 60000, 60000, 1)
        e.write(self.par, struct.pack('<BBHHHHHHHHHHHIIH', 1, 1, 54, 34, thr_press, thr_release, thr_track,
                                      quiet_lim, 60, thr_heavy, heavy_n, noise, 20, pos_to, neg_to, 1))
        e.call(F_PARAMS, self.st, self.par)
        e.wr(self.tmp, 2, coef)
        e.call(F_SETCOEF, self.st, self.tmp)
        e.call(F_RESET, self.st)

    def step(self, v):
        e = self.e
        e.wr(self.tmp, 2, v & 0xffff)
        e.call(F_STEP, self.st, self.tmp, self.cfg)
        e.call(F_GET_FORCE, self.st, 0, self.tmp + 4)
        f = e.rd(self.tmp + 4, 2)
        return (f - 0x10000 if f & 0x8000 else f), e.read(self.st, 236)


def streams(rng, quick):
    """(name, samples, noise, coef, [pos_timeout, neg_timeout, thr_press, thr_release, thr_track,
    quiet_lim, thr_heavy, heavy_n])  -- the optional parameters default to the stock values"""
    def noise(n, amp):
        return [rng.randint(-amp, amp) for _ in range(n)]

    def brushing(zero, coef_counts, secs, ripple):
        out = [zero + x for x in noise(80, 2)]
        for _ in range(secs):
            level = rng.choice([0, 0, 200, 600, 1200, 1800, 2600, 4000]) * coef_counts // 23
            n = rng.randint(10, 120)
            ramp = rng.randint(1, 15)
            cur = out[-1] - zero
            for i in range(n):
                tgt = cur + (level - cur) * min(i + 1, ramp) // ramp
                out.append(zero + tgt + rng.randint(-ripple, ripple))
        return out

    def walk(zero, n, step):
        out, v = [], zero
        for _ in range(n):
            v += rng.randint(-step, step)
            v = max(-8192, min(8191, v))
            out.append(v)
        return out

    def taps(zero, n):
        out = [zero + x for x in noise(60, 1)]
        for _ in range(n):
            h = rng.choice([30, 60, 100, 150, 300, 800, 3000])
            w = rng.randint(1, 30)
            out += [zero + h + rng.randint(-3, 3) for _ in range(w)]
            out += [zero + rng.randint(-3, 3) + rng.choice([0, 0, -20, -60, 40]) for _ in range(rng.randint(1, 60))]
        return out

    def drift(zero, n, per):
        return [zero + i // per + rng.randint(-2, 2) for i in range(n)] + \
               [zero + n // per - i // per + rng.randint(-2, 2) for i in range(n)]

    def dips(zero, n):
        out = [zero + x for x in noise(60, 2)]
        for _ in range(n):
            depth = rng.choice([3, 8, 12, 15, 18, 25, 28, 40, 90, 300])
            w = rng.randint(1, 80)
            sl = rng.choice([1, 2, 5, 50])
            cur = 0
            for i in range(w):
                cur = max(-depth, cur - depth // sl - 1)
                out.append(zero + cur + rng.randint(-1, 1))
            out += [zero + rng.randint(-2, 2) for _ in range(rng.randint(5, 50))]
        return out

    k = 1 if quick else 4
    S = []
    S.append(('brushing, default calibration', brushing(-1500, 23, 60 * k, 6), 9, 23, 60000, 60000))
    S.append(('brushing, calibrated coef 41 noise 5', brushing(700, 41, 40 * k, 3), 5, 41, 60000, 60000))
    S.append(('brushing, coef 250 (above 200)', brushing(-3000, 250, 30 * k, 10), 12, 250, 60000, 60000))
    S.append(('taps and dips after release', taps(100, 120 * k), 9, 23, 60000, 60000))
    S.append(('negative excursions', dips(-200, 80 * k), 9, 23, 60000, 60000))
    S.append(('negative excursions, coef 300', dips(2000, 60 * k), 6, 300, 60000, 60000))
    S.append(('slow drift', drift(-900, 1500 * k, 12), 9, 23, 60000, 60000))
    S.append(('slow drift, coef 220', drift(50, 1000 * k, 7), 9, 220, 60000, 60000))
    S.append(('random walk small', walk(0, 2500 * k, 4), 9, 23, 60000, 60000))
    S.append(('random walk large', walk(-4000, 2500 * k, 60), 4, 100, 60000, 60000))
    S.append(('random walk huge, coef 600', walk(3000, 1500 * k, 900), 20, 600, 60000, 60000))
    S.append(('short timeouts: long press and long dip', [10 + x for x in noise(60, 1)] + [3000 + x for x in noise(400, 5)] + [10 + x for x in noise(100, 2)] +
              [-900 + x for x in noise(400, 3)] + [10 + x for x in noise(200, 2)] + taps(10, 30 * k), 9, 23, 150, 120))
    S.append(('full-scale edges', [-8192] * 40 + [8191] * 60 + [-8192] * 60 + walk(0, 800 * k, 3000), 9, 23, 60000, 60000))
    # Parameters the stock firmware never uses: they reach branches of the library
    # that are dead with the stock constants, to check the port there as well.
    S.append(('wide tracking window (thr_track 300)', taps(0, 60 * k) + walk(0, 1500 * k, 12), 9, 23,
              60000, 60000, 400, 200, 300, 20, 1600, 8))
    S.append(('wide tracking window, coef 240', dips(500, 40 * k) + walk(500, 1500 * k, 8), 3, 240,
              60000, 60000, 500, 100, 300, 20, 1600, 8))
    S.append(('noise 1, thr_track 40', walk(-100, 2000 * k, 3) + taps(-100, 40 * k), 1, 23,
              60000, 60000, 60, 30, 40, 2, 300, 3))
    S.append(('noise 0', walk(0, 800 * k, 5), 0, 23, 300, 300, 20, 10, 10, 20, 1600, 8))
    S.append(('low heavy threshold', brushing(-1000, 23, 40 * k, 8) + dips(-1000, 30 * k), 9, 23,
              500, 400, 20, 10, 10, 20, 150, 2))
    S.append(('thr_release above thr_press', taps(300, 80 * k), 9, 50, 60000, 60000, 20, 90, 10, 20, 1600, 8))
    return S


def main():
    quick = '--quick' in sys.argv
    seed = int(sys.argv[sys.argv.index('--seed') + 1]) if '--seed' in sys.argv else 1
    rng = random.Random(seed)
    if '--exe' in sys.argv:
        exe = os.path.abspath(sys.argv[sys.argv.index('--exe') + 1])
    else:
        exe = os.path.join(tempfile.mkdtemp(prefix='sim_force'), 'sim_force')
        subprocess.check_call(['cc', '-std=gnu11', '-Wall', '-Wextra', '-ffp-contract=off', '-I', os.path.join(ROOT, 'main'),
                               os.path.join(HERE, 'sim_force.c'), '-o', exe])
    stock = Stock()
    bad = total = 0
    for name, samples, *par in streams(rng, quick):
        par = par + [60000, 60000, 20, 10, 10, 20, 1600, 8][len(par) - 2:]
        stock.init(*par)
        feed = 'I ' + ' '.join(str(p) for p in par) + '\n' + ''.join('S %d\n' % v for v in samples)
        out = subprocess.run([exe, 'pipe'], input=feed, capture_output=True, text=True, check=True).stdout.split('\n')
        fmin = fmax = 0
        ok = True
        for i, v in enumerate(samples):
            f_ref, st_ref = stock.step(v)
            f_c, st_c = out[i].split()
            fmin, fmax = min(fmin, f_ref), max(fmax, f_ref)
            if int(f_c) != f_ref or bytes.fromhex(st_c) != st_ref:
                ok = False
                bad += 1
                diff = [(o, st_ref[o], bytes.fromhex(st_c)[o]) for o in range(236) if st_ref[o] != bytes.fromhex(st_c)[o]]
                print('  MISMATCH in "%s" at sample %d (input %d): force stock %d port %s; state bytes (offset, stock, port): %s'
                      % (name, i, v, f_ref, f_c, ', '.join('%#x:%02x/%02x' % d for d in diff[:12])))
                print('    previous inputs:', samples[max(0, i - 8):i + 1])
                break
        total += len(samples)
        print('%-45s %6d samples  force %6d..%-6d %s' % (name, len(samples), fmin, fmax, 'ok' if ok else 'FAILED'))
    print('\n%d samples compared, %d stream(s) differ (%d stock instructions executed)' % (total, bad, stock.e.count))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
