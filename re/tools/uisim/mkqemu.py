#!/usr/bin/env python3
"""Build a 16 MB flash image for `qemu-system-xtensa -machine esp32s3` from a build dir.

The image mimics a brush that was flashed by app-only OTA: IDF bootloader, a partition
table that contains an OEM-style picture partition (type 0x40), the app in ota_0 and,
optionally, a stand-in picture image (see mkres.py).

QEMU does not emulate the SAR ADC, so IDF's start-up ADC self-calibration never
finishes; --stub NAME (repeatable) turns the named functions into an immediate return in
the QEMU copy of the app (image checksum and SHA-256 are fixed up).

usage: mkqemu.py BUILD_DIR OUT.bin [--res res.bin] [--stub adc_hw_calibration ...]
"""
import argparse, hashlib, os, struct, subprocess, sys

NM = os.environ.get('XTENSA_NM', os.path.expanduser(
    '~/.espressif/tools/xtensa-esp32s3-elf/esp-12.2.0_20230208/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-nm'))

def ptable():
    """nvs, otadata, phy, ota_0 (3 MB), ota_1 (3 MB), res (type 0x40, 8 MB)."""
    rows = [(b'nvs', 1, 2, 0x9000, 0x6000), (b'otadata', 1, 0, 0xf000, 0x2000), (b'phy_init', 1, 1, 0x11000, 0x1000),
            (b'ota_0', 0, 0x10, 0x20000, 0x300000), (b'ota_1', 0, 0x11, 0x320000, 0x300000),
            (b'res', 0x40, 0, 0x620000, 0x800000)]
    out = b''
    for name, t, st, off, size in rows:
        out += struct.pack('<2sBBII16sI', b'\xaa\x50', t, st, off, size, name, 0)
    out += b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(out).digest()
    return out.ljust(0xC00, b'\xff'), dict((r[0].decode(), (r[3], r[4])) for r in rows)

def stub(app, elf, names):
    syms = {}
    for line in subprocess.run([NM, elf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3: syms[p[2]] = int(p[0], 16)
    app = bytearray(app)
    nseg = app[1]
    segs, off = [], 24
    for _ in range(nseg):
        la, ln = struct.unpack_from('<II', app, off)
        segs.append((la, off + 8, ln)); off += 8 + ln
    for n in names:
        a = syms[n]
        for la, fo, ln in segs:
            if la <= a < la + ln:
                o = fo + a - la
                assert app[o] == 0x36, 'no entry at %s' % n     # entry a1, N
                app[o + 3:o + 5] = b'\x1d\xf0'                  # retw.n
                break
        else:
            sys.exit('symbol %s not in image' % n)
    # fix the XOR checksum (last byte of the 16-byte-padded segment area) and the SHA-256
    end = off
    ck = 0xEF
    for la, fo, ln in segs:
        for b in app[fo:fo + ln]: ck ^= b
    pad_end = (end + 16) & ~15
    app[pad_end - 1] = ck
    if app[23] == 1:                                             # hash appended
        app[pad_end:pad_end + 32] = hashlib.sha256(bytes(app[:pad_end])).digest()
    return bytes(app)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build'); ap.add_argument('out')
    ap.add_argument('--res'); ap.add_argument('--stub', action='append', default=[])
    ap.add_argument('--project', default='oclean_custom')
    a = ap.parse_args()
    img = bytearray(b'\xff' * 0x1000000)
    def put(off, data): img[off:off + len(data)] = data
    put(0, open(os.path.join(a.build, 'bootloader', 'bootloader.bin'), 'rb').read())
    pt, parts = ptable()
    put(0x8000, pt)
    app = open(os.path.join(a.build, a.project + '.bin'), 'rb').read()
    if a.stub: app = stub(app, os.path.join(a.build, a.project + '.elf'), a.stub)
    put(parts['ota_0'][0], app)
    if a.res:
        r = open(a.res, 'rb').read()[:parts['res'][1]]
        put(parts['res'][0], r)
    open(a.out, 'wb').write(img)
    print('wrote', a.out)

if __name__ == '__main__':
    main()
