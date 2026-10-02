#!/usr/bin/env python3
"""Oclean X Ultra 20 (OCLEANV20, stock app 0.0.1.1 / 0.0.1.6) UI picture extractor.

Where things live (all derived from the stock app image ota.bin):
  * Pixels  : NOT in ota.bin.  They live in a raw data partition, type 0x40 subtype 0x00
              (esp_partition_find_first(0x40,0,NULL) at 0x40378720/0x40378754/0x40378794,
              esp_partition_read = 0x4207bc6c).  SRC below = bytes of that partition from
              partition offset 0 (a device dump, or a pack such as basepicV1a.bin which the
              stock downloader writes verbatim at offset 0 - see 0x420138d4/0x42024da4).
  * Index   : u32 LE table of 927 picture offsets in DROM 0x3c11b248 (= ota.bin file 0xB248),
              entry[927]==0 terminator.  size(id) = off[id+1]-off[id]; id 926 = 80x88 (widget).
  * Special : ids 927..950 -> 24 x u32 at DROM 0x3c11b1e8 (ota.bin 0xB1E8), relative to
              partition 0x77E000, 80x44, 3 bytes/px [hi, lo, alpha(0 = transparent)].
              id 998 -> 80x160 RGB565 at 0x75E000 (bank 'A') / 0x76E000 (bank 'B').
  * Format  : raw RGB565, BIG-endian (hi byte first, exactly the SPI byte order), row-major,
              no header, no compression, no palette.  Width/height come from the UI widget
              objects in .data (DRAM 0x3fc9af18.., 24-byte objects {x,y,w,h,type=3,desc*,..}).
Usage:
  oclean_ui_extract.py --ota ota.bin --src resource_partition.bin [--out DIR] [--c-ids 173-182,863-873]
"""
import argparse, json, os, re, struct
import numpy as np
from PIL import Image

DROM_VMA, DROM_FO = 0x3c110020, 0x20        # ota.bin seg0
DRAM_VMA, DRAM_FO, DRAM_LEN = 0x3fc99e00, 0x41698, 0x7f70   # ota.bin seg1
PIC_TABLE_VMA, N_PICS = 0x3c11b248, 927
SPECIAL_TABLE_VMA, N_SPECIAL, SPECIAL_BASE = 0x3c11b1e8, 24, 0x77E000
CUSTOM_A, CUSTOM_B = 0x75E000, 0x76E000
# ids drawn with RGB565 0x0000 treated as transparent (0x40378298 pixel loop)
BLACK_KEY = {0, 53, 118, 129, 130, 131, 132, 133, 297, 331, 348, 365, 382, 416, 592, 609, 626,
             661, 708, 709, 715, 732, 733, 735, 842, 843, 844, 845, 846, 847,
             849, 850, 851, 852, 854, 855, 856, 857, 858, 859, 860, 861, 862, 863, 864, 865,
             867, 868, 869, 870, 871, 872, 873, 874}            # plus every id > 937
COLOR_KEY = {185: 0x4646, 195: 0xF9C7}                           # BE pixel value skipped
# Glyph runs whose dims are not on a static widget but follow from a same-size sibling that is
# (or from the digit LUT at DRAM 0x3fc9af0a = [20..29,0,1,2,30] used with desc base 843).
SIBLING_DIMS = {}
for _r, _wh in ((range(172, 183), (14, 24)),     # 173..182 = '0'..'9' (0x420212e0: base 173 + digit)
                (range(185, 205), (14, 24)),     # second 14x24 set (widgets 185, 195)
                (range(696, 707), (8, 18)),      # widgets 696/697 (+707 = 4x18 separator)
                (range(863, 873), (12, 24)),     # '0'..'9' via 843 + LUT[d] (0x4201f6dc clock)
                ((873,), (5, 24)), ((844,), (7, 24)), ((843, 845), (9, 24)),
                (range(104, 128), (8, 12)),      # run containing widget id 118 (8x12)
                (range(0, 8), (40, 77)),         # widgets 0 and 7 (charge capsule frames)
                ((708,), (22, 22)),              # same size as widgets 709/732/733/842 (22x22)
                (range(736, 757), (79, 36))):    # run starting at widget id 735 (79x36)
    for _i in _r:
        SIBLING_DIMS[_i] = _wh
# weaker: same byte size as a widget-confirmed glyph, no widget of their own (verify visually)
SIZE_MATCH_DIMS = {i: (8, 12) for r in (range(26, 38), range(39, 51), range(52, 64), range(65, 77),
                                        range(78, 90), range(91, 103), range(758, 770)) for i in r}
SIZE_MATCH_DIMS.update({140: (9, 12), 141: (9, 12)})              # 216 B like widget id 129 (9x12)


def drom_off(vma): return vma - DROM_VMA + DROM_FO


def load_ota(path):
    ota = open(path, 'rb').read()
    t = drom_off(PIC_TABLE_VMA)
    offs = [struct.unpack_from('<I', ota, t + 4 * i)[0] for i in range(N_PICS + 1)]
    assert offs[0] == 0 and offs[N_PICS] == 0 and all(offs[i] < offs[i + 1] for i in range(N_PICS - 1)), 'table mismatch'
    s = drom_off(SPECIAL_TABLE_VMA)
    special = [struct.unpack_from('<I', ota, s + 4 * i)[0] for i in range(N_SPECIAL)]
    dram = ota[DRAM_FO:DRAM_FO + DRAM_LEN]
    descs = {DRAM_VMA + m.start() for m in re.finditer(rb'\*C#', dram)}
    widgets = []
    for o in range(0, len(dram) - 24, 4):
        p = struct.unpack_from('<I', dram, o)[0]
        if p in descs:
            so = o - 8
            x, y, w, h = dram[so:so + 4]
            d = dram[p - DRAM_VMA:p - DRAM_VMA + 6]
            hi, lo = d[3], d[4]
            pid = ((hi << 8) | lo) - (hi if hi in (1, 2, 3) else 0)   # decode as 0x4037848c
            widgets.append(dict(obj=DRAM_VMA + so, x=x, y=y, w=w, h=h, desc=p, id=pid))
    return offs, special, widgets


def rgb565be_to_rgb(buf, w, h):
    a = np.frombuffer(buf, dtype='>u2').reshape(h, w).astype(np.uint32)
    return np.stack([((a >> 11) & 31) * 255 // 31, ((a >> 5) & 63) * 255 // 63, (a & 31) * 255 // 31], -1).astype(np.uint8)


def smooth(buf, w):
    h = len(buf) // 2 // w
    im = rgb565be_to_rgb(buf[:w * h * 2], w, h).astype(int)
    return np.abs(im[1:] - im[:-1]).mean() if h > 1 else 1e9


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ota', default='$HOME/.oclean/fw/ota.bin')
    ap.add_argument('--src', required=True, help='resource partition bytes from offset 0')
    ap.add_argument('--out', default='ui_out')
    ap.add_argument('--c-ids', default='', help='e.g. 173-182,863-873 -> ui_assets.h')
    a = ap.parse_args()
    offs, special, widgets = load_ota(a.ota)
    src = open(a.src, 'rb').read()
    os.makedirs(os.path.join(a.out, 'png'), exist_ok=True)
    known = {}
    for wd in widgets:
        i = wd['id']
        if i < N_PICS - 1 and offs[i + 1] - offs[i] == wd['w'] * wd['h'] * 2:
            known[i] = (wd['w'], wd['h'])
    known[926] = (80, 88)                     # widget obj 0x3fc9b098; size not in table
    # propagate widget dims along runs of consecutive same-size pictures (frame/glyph sets)
    run_dims = {}
    i = 0
    while i < N_PICS - 1:
        j, sz = i, offs[i + 1] - offs[i]
        while j + 1 < N_PICS - 1 and offs[j + 2] - offs[j + 1] == sz:
            j += 1
        dims = {known[k] for k in range(i, j + 1) if k in known}
        if len(dims) == 1:
            for k in range(i, j + 1):
                if k not in known:
                    run_dims[k] = next(iter(dims))
        i = j + 1
    for k, v in run_dims.items():
        SIBLING_DIMS.setdefault(k, v)
    index = []
    for i in range(N_PICS):
        off = offs[i]
        if i in known:
            w, h = known[i]; how = 'widget'
        elif i in SIBLING_DIMS and SIBLING_DIMS[i][0] * SIBLING_DIMS[i][1] * 2 == offs[i + 1] - off:
            w, h = SIBLING_DIMS[i]; how = 'sibling'
        elif i in SIZE_MATCH_DIMS and SIZE_MATCH_DIMS[i][0] * SIZE_MATCH_DIMS[i][1] * 2 == offs[i + 1] - off:
            w, h = SIZE_MATCH_DIMS[i]; how = 'size-match(guess)'
        else:
            px = (offs[i + 1] - off) // 2
            cands = [c for c in range(4, 81) if px % c == 0 and px // c <= 160]
            seg = src[off:off + px * 2]
            if px % 80 == 0:
                w = 80
            elif len(seg) == px * 2 and cands:
                w = min(cands, key=lambda c: smooth(seg, c))
            else:
                w = cands[-1] if cands else px
            h = px // w; how = 'inferred'
        size = w * h * 2
        ok = off + size <= len(src)
        if ok:
            Image.fromarray(rgb565be_to_rgb(src[off:off + size], w, h)).save(os.path.join(a.out, 'png', '%03d.png' % i))
        index.append(dict(id=i, offset=off, size=size, w=w, h=h, dims=how, in_src=ok,
                          black_transparent=(i in BLACK_KEY), color_key=COLOR_KEY.get(i)))
    for k, rel in enumerate(special):                # 3 bytes/px with alpha
        i, off, size = 927 + k, SPECIAL_BASE + rel, 80 * 44 * 3
        ok = off + size <= len(src)
        if ok:
            b = np.frombuffer(src[off:off + size], dtype=np.uint8).reshape(44, 80, 3)
            v = (b[..., 0].astype(np.uint32) << 8) | b[..., 1]
            rgba = np.stack([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31,
                             np.where(b[..., 2] != 0, 255, 0)], -1).astype(np.uint8)
            Image.fromarray(rgba, 'RGBA').save(os.path.join(a.out, 'png', '%03d.png' % i))
        index.append(dict(id=i, offset=off, size=size, w=80, h=44, dims='code', in_src=ok, format='rgb565be+alpha8',
                          draw_y=20 if i <= 0x3a9 else 64))
    for bank, off in (('A', CUSTOM_A), ('B', CUSTOM_B)):
        ok = off + 25600 <= len(src)
        if ok:
            Image.fromarray(rgb565be_to_rgb(src[off:off + 25600], 80, 160)).save(os.path.join(a.out, 'png', '998%s.png' % bank))
        index.append(dict(id=998, bank=bank, offset=off, size=25600, w=80, h=160, dims='code', in_src=ok))
    json.dump(dict(pictures=index, widgets=widgets), open(os.path.join(a.out, 'index.json'), 'w'), indent=1)
    if a.c_ids:
        ids = []
        for part in a.c_ids.split(','):
            lo, _, hi = part.partition('-'); ids += range(int(lo), int(hi or lo) + 1)
        with open(os.path.join(a.out, 'ui_assets.h'), 'w') as f:
            f.write('// RGB565 big-endian (send bytes as-is to ST7735, COLMOD=0x05)\n#include <stdint.h>\n')
            for i in ids:
                e = index[i]; d = src[e['offset']:e['offset'] + e['size']]
                f.write('// id %d  %dx%d  partition+0x%06x\nstatic const uint8_t ui_pic_%d[%d] = {\n' % (i, e['w'], e['h'], e['offset'], i, len(d)))
                for r in range(0, len(d), 16):
                    f.write('  ' + ','.join('0x%02x' % x for x in d[r:r + 16]) + ',\n')
                f.write('};\n')
    print('pictures in src: %d/%d   dims from widgets: %d' % (sum(e['in_src'] for e in index[:N_PICS]), N_PICS, len(known)))


if __name__ == '__main__':
    main()
