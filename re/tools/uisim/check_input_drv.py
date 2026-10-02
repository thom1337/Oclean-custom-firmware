#!/usr/bin/env python3
"""Differential test of main/hw_touch.c and main/hw_pressure.c against the stock code.

The stock driver functions (touch state machine 0x4201b458 with init 0x42025f2c and
sleep 0x42026814; AW8686X init 0x42027894, temperature coefficient 0x403787cc and
sample 0x420277d4) are executed from the stock image in the Xtensa interpreter
(re/tools/xt_emu.py). Their three bus calls (0x4200cbd0, 0x4200cbec, 0x4200cc30), the
delay call and the gesture decoder are hooked and logged. The same scenario is run
through the port (re/tools/uisim/sim_input_drv.c, which logs its bus calls the same
way) and the two logs are compared line by line: every register write, every read,
every delay, the state after every step and the pressure after every sample.

    python3 re/tools/uisim/check_input_drv.py [-v]
"""
import os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, 're', 'tools'))
from xt_emu import Emu

BSS_START, BSS_END = 0x3fca1d70, 0x3fcb0000      # the image's .data ends at 0x3fca1d70
TOUCH_STATE, PRESSURE, TEMP_COEF = 0x3fca4ded, 0x3fca5c80, 0x3fc9fd78


class Stock:
    def __init__(self):
        e = self.e = Emu()
        e.map(BSS_START, BSS_END - BSS_START)
        self.log, self.rx, self.rdy, self.nvs, self.temp = [], [], 1, {}, 0.0
        h = e.hooks
        h[0x4200cbd0] = self.write8
        h[0x4200cbec] = self.write16
        h[0x4200cc30] = self.read
        h[0x4200cc4c] = self.rdy_level
        h[0x4201b580] = self.delay
        h[0x42025ca4] = self.gesture
        h[0x42026ab0] = self.nvs_cal
        h[0x420241f0] = self.nvs_temp
        h[0x4201e240] = self.imu_temp
        h[0x40388000] = self.malloc
        for a in (0x420ebce8, 0x420ebf14, 0x400015d8, 0x400005d0):     # printf, puts, ROM printf, ets_printf
            h[a] = lambda r: None

    def write8(self, r):
        e = self.e
        self.log.append('W8 %02X %02X' % (r[10] & 0xff, r[11] & 0xff) +
                        ''.join(' %02X' % b for b in e.read(r[13], r[12] & 0xff)))
        r[10] = 0

    def write16(self, r):
        e = self.e
        self.log.append('W16 %02X %02X %02X' % (r[10] & 0xff, r[11] & 0xff, r[12] & 0xff) +
                        ''.join(' %02X' % b for b in e.read(r[14], r[13] & 0xff)))

    def read(self, r):
        n = r[12] & 0xff
        self.log.append('R %02X %02X %d' % (r[10] & 0xff, r[11] & 0xff, n))
        data = self.rx.pop(0) if self.rx else b''
        self.e.write(r[13], (data + bytes(n))[:n])
        r[10] = 0

    def rdy_level(self, r):
        r[10] = self.rdy

    def delay(self, r):
        self.log.append('D %d' % ((r[10] & 0xffff) * 1000))

    def gesture(self, r):
        self.log.append('G %d %d' % (r[10] & 0xffff, r[11] & 0xffff))

    def nvs_cal(self, r):            # 0x42026ab0(x, buf, len)
        if 'aw8686x_config' in self.nvs:
            self.e.write(r[11], (self.nvs['aw8686x_config'] + bytes(32))[:r[12]])
        r[10] = 0

    def nvs_temp(self, r):           # 0x420241f0(buf, offset, len)
        if 'rec_temperature' in self.nvs:
            self.e.write(r[10], (self.nvs['rec_temperature'] + bytes(32))[:r[12]])

    def imu_temp(self, r):
        r[10] = struct.unpack('<I', struct.pack('<f', self.temp))[0]

    def malloc(self, r):
        r[10] = self.e.alloc(r[10])

    def run(self, cmd):
        e, w = self.e, cmd.split()
        if w[0] == 'rdy':
            self.rdy = int(w[1])
        elif w[0] == 'rx':
            self.rx.append(bytes.fromhex(w[1]))
        elif w[0] == 'nvs':
            self.nvs[w[1]] = bytes.fromhex(w[2])
        elif w[0] == 'temp':
            self.temp = float(w[1])
        elif w[0] == 'tset':
            e.call(0x4201b430, int(w[1]))
            self.log.append('= state %d' % e.rd(TOUCH_STATE, 1))
        elif w[0] == 'step':
            e.call(0x4201b458)
            self.log.append('= state %d' % e.rd(TOUCH_STATE, 1))
        elif w[0] == 'pinit':
            ok = e.call(0x42027894) == 0 and e.rd(0x3fca5c70, 4) != 0     # what 0x42027bd0 does
            e.wr(TEMP_COEF, 4, e.call(0x403787cc))
            self.log.append('= avail %d' % ok)
        elif w[0] == 'psample':
            e.call(0x420277d4)
            v = e.rd(PRESSURE, 2)
            v = v - 0x10000 if v & 0x8000 else v
            self.log.append('= pressure %d' % max(0, v))        # 0x42018530


def record(noise, coef):
    data = struct.pack('<HH', noise, coef) + b'\x5a\x5a'
    return (b':\xa0\x06' + data + struct.pack('<I', sum(data)) + b'\x00\r\n').hex()


def touch_scenario():
    st = lambda status, x, y: 'rx ' + struct.pack('<6H', status, 0, 0, 0, x, y).hex()
    c = ['tset 0', 'step', 'rx 1604000102030405', 'step', 'rdy 1', 'step', 'rdy 0', 'step', 'step']
    c += [st(0, 0xFFFF, 0xFFFF), 'step', st(0, 120, 37), 'step', st(4, 121, 40), 'step']
    c += [st(8, 0, 0), 'step', 'step', 'step', st(2, 0, 0), 'step', 'step', 'step']       # reset / ATI error -> re-init
    c += ['tset 6', 'step', st(0, 5, 250), 'step']
    c += ['tset 7', 'step', 'step']                                                         # sleep twice
    c += ['tset 0', 'tset 33', 'step', 'step', st(0, 77, 78), 'step']                       # wake: init with 0x55
    c += ['tset 7', 'step', 'tset 33', 'step', 'step']
    for s in (2, 3, 4, 8, 9, 10, 11, 12, 32, 34, 255):
        c += ['tset %d' % s, 'rx 1604000000000000', 'step']
    return c


def pressure_scenario(cal, temp, base, raw):
    c = []
    if cal:
        c.append('nvs aw8686x_config ' + cal)
    if base is not None:
        c.append('nvs rec_temperature %02x' % base)
    c += ['temp %s' % temp, 'rx 62', 'rx 37', 'rx b0', 'rx 4b', 'pinit']
    for v in raw:
        c += ['rx ' + struct.pack('<H', (v + 0x2000) & 0xffff).hex(), 'psample']
    return c


def press_curve(zero, peak, n=260):
    out = []
    for i in range(n):
        if i < 60:
            lvl = 0
        elif i < 80:
            lvl = peak * (i - 60) // 20
        elif i < 200:
            lvl = peak + (i * 37 % 23) - 11
        elif i < 210:
            lvl = peak * (210 - i) // 10
        else:
            lvl = 0
        out.append(zero + lvl + (i * 7 % 5) - 2)
    return out


def main():
    verbose = '-v' in sys.argv
    exe = os.path.join(tempfile.mkdtemp(prefix='sim_input_drv'), 'sim_input_drv')
    subprocess.check_call(['cc', '-std=gnu11', '-Wall', '-Wextra', '-ffp-contract=off',
                           '-I', os.path.join(HERE, 'fake_idf_input'), '-I', os.path.join(ROOT, 'main'),
                           os.path.join(HERE, 'sim_input_drv.c')] +
                          [os.path.join(ROOT, 'main', f) for f in ('hw_touch.c', 'hw_pressure.c', 'oem_force.c')] +
                          ['-o', exe])
    scenarios = [
        ('touch: cold start, run, re-init, reseed, sleep, wake', touch_scenario()),
        ('pressure: factory calibration, +6 C', pressure_scenario(record(7, 31), 31.5, 25, press_curve(-1200, 1500))),
        ('pressure: no calibration, no base temperature', pressure_scenario(None, 22.9, None, press_curve(300, 2600))),
        ('pressure: cold (limit 0.75)', pressure_scenario(record(4, 55), -12.0, 30, press_curve(-4000, 900))),
        ('pressure: hot (limit 1.25), strong press', pressure_scenario(record(9, 40), 58.2, 20, press_curve(2000, 5000))),
        ('pressure: wrong chip id', ['temp 20', 'rx 00', 'rx 63', 'rx ff', 'pinit']),
        ('pressure: id on the third read', ['temp 20', 'rx 00', 'rx 00', 'rx 64', 'pinit', 'rx 0020', 'psample']),
    ]
    bad = 0
    for name, cmds in scenarios:
        stock = Stock()
        for c in cmds:
            stock.run(c)
        out = subprocess.run([exe], input='\n'.join(cmds) + '\n', capture_output=True, text=True, check=True).stdout
        port = []
        for line in out.splitlines():
            w = line.split()
            if w[0] in ('W8', 'W16', 'R', 'D', 'G'):
                port.append(line)
            elif w[0] == '=' and len(w) > 2:
                port.append(' '.join(w[:3]))
        ok = port == stock.log
        nw = sum(1 for x in stock.log if x[0] == 'W')
        print('%-55s %4d transfers, %4d result lines  %s' % (
            name, sum(1 for x in stock.log if x[0] in 'WR'), sum(1 for x in stock.log if x[0] == '='), 'ok' if ok else 'FAILED'))
        if verbose or not ok:
            for i in range(max(len(port), len(stock.log))):
                a = stock.log[i] if i < len(stock.log) else '(none)'
                b = port[i] if i < len(port) else '(none)'
                if verbose or a != b:
                    print('   %4d  stock: %-40s port: %s%s' % (i, a[:70], b[:70], '' if a == b else '   <<<'))
                    if a != b and not verbose:
                        break
        if not ok:
            bad += 1
        del nw
    print('\n%d scenario(s) differ' % bad if bad else '\nall scenarios match the stock code')
    bad += port_only(exe)
    return 1 if bad else 0


def port_only(exe):
    """Behaviour of the port that has no single stock function to compare with."""
    def run(cmds):
        return subprocess.run([exe], input='\n'.join(cmds) + '\n', capture_output=True, text=True, check=True).stdout.splitlines()

    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    # RDY pin: driven low at boot (0x4200da4c), input + pull-up + falling edge with the handler (0x4200cc58)
    o = run(['tinit', 'irq 1', 'irq 0'])
    check(o[:3] == ['BUSINIT', 'GPIO config mask 1000 mode 2 pullup 1 intr 2', 'GPIO 12 level 0'], 'touch init pin setup: %s' % o[:3])
    check(o[4:] == ['GPIO config mask 1000 mode 1 pullup 1 intr 2', 'ISR add 12', '=',
                    'GPIO config mask 1000 mode 1 pullup 1 intr 2', 'ISR remove 12', '='], 'touch irq on/off: %s' % o[4:])
    # 1 Hz reseed watchdog (0x42025e20): state 6 after 3 s, with or without a finger, not while the motor runs
    o = run(['tset 5', 'tick', 'tick', 'tick', 'tset 5', 'touching 1', 'tick', 'tick', 'tick',
             'tset 5', 'session 1 200', 'tick', 'tick', 'tick', 'tick', 'session 1 0', 'tick', 'tick', 'tick',
             'tset 1', 'session 0 200', 'tick', 'tick', 'tick', 'tick'])
    states = [int(x.split()[2]) for x in o if x.startswith('= state')]
    check(states == [5, 5, 5, 6, 5, 5, 5, 6, 5, 5, 5, 5, 5, 5, 5, 6, 1, 1, 1, 1, 1], 'reseed watchdog: %s' % states)
    check(not any(x[0] in 'WR' for x in o), 'the watchdog itself sends nothing')
    # a status read outside the window (not acknowledged) is skipped; a real reset status still re-inits
    o = run(['tset 5', 'nak 1', 'step', 'rx ' + struct.pack('<6H', 8, 0, 0, 0, 0, 0).hex(), 'step'])
    check([x for x in o if x.split()[0] in ('W8', 'W16', 'R', 'G', '=')] == ['= state 5', 'R 44 10 12', '= state 5', 'R 44 10 12', '= state 33'],
          'unanswered status read: %s' % o)
    # start = init + timer; a second start does nothing; stop then start sets the chip up again
    o = run(['temp 20', 'rx 62', 'pstart', 'pstart', 'pstop', 'rx 62', 'pstart', 'pstop', 'rx 62', 'pinit', 'pstart'])
    check(sum(1 for x in o if x == 'R 6A 00 1') == 3 and sum(1 for x in o if x == 'T start 3 20 1') == 3 and
          sum(1 for x in o if x == 'W8 6A 2F 10') == 3, 'pressure start / stop / init: %d id reads, %d timer starts' % (
              sum(1 for x in o if x == 'R 6A 00 1'), sum(1 for x in o if x == 'T start 3 20 1')))
    o = run(['temp 20', 'rx 00', 'rx 00', 'rx 00', 'pstart'])
    check(not any(x.startswith('T start') for x in o), 'no sampling timer without the chip')
    # unanswered samples are dropped; after 50 in a row the value is withdrawn
    seq = ['temp 20', 'rx 62', 'pstart'] + ['rx 0020', 'psample'] * 30 + ['rx 0030', 'psample'] * 5
    o = run(seq + ['nak 49'] + ['psample'] * 49 + ['rx 0030', 'psample'] + ['nak 50'] + ['psample'] * 50 + ['psample'])
    res = [x for x in o if x.startswith('= pressure')]
    p_pressed = res[34]
    check(int(p_pressed.split()[2]) > 500, 'pressure while pressed: %s' % p_pressed)
    check(all(x == p_pressed for x in res[35:84]), 'unanswered samples keep the last value')
    check(res[84] == p_pressed, 'an answered sample resets the failure count')
    check(res[85 + 48] == p_pressed and res[85 + 49] == '= pressure 0 avail 0', '50 unanswered samples: %s' % res[85 + 49])
    check('T stop 3' in o, 'timer stopped when the chip is gone')
    for f in fails:
        print('  FAIL (port-only): ' + f)
    print('port-only checks: %s' % ('%d FAILED' % len(fails) if fails else 'ok'))
    return len(fails)


if __name__ == '__main__':
    sys.exit(main())
