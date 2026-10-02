#!/usr/bin/env python3
"""Build a stand-in for the brush's OEM picture partition, for the host UI simulator.

The real pictures exist only on the brush. This writes an image with the same layout
(offsets from the stock table, sizes from the stock widgets) where every picture is a
placeholder labelled with its id; digit glyph sets are drawn as real digits so composed
screens are readable. If --real is given (a partition dump, or the public basepicV1a.bin
whose ids 0..25 line up), those bytes are used where present.

usage: mkres.py --index index.json --out res_fake.bin [--real dump.bin [--real-max-id 25]]
(index.json comes from: ui_extract.py --ota ota.bin --src /dev/null --out DIR)
"""
import argparse, json, struct, zlib
from PIL import Image, ImageDraw, ImageFont

GLYPHS = {}
for d in range(10):
    GLYPHS[173 + d] = str(d); GLYPHS[185 + d] = str(d); GLYPHS[195 + d] = str(d)
    GLYPHS[697 + d] = str(d); GLYPHS[863 + d] = str(d)
GLYPHS.update({172: '%', 183: ':', 184: 's', 696: 'V', 707: '.', 873: ':'})
BLACK_KEY = {0, 53, 118, 129, 130, 131, 132, 133, 297, 331, 348, 365, 382, 416, 592, 609, 626, 661, 708, 709,
             715, 732, 733, 735} | set(range(842, 848)) | (set(range(849, 875)) - {853, 866})

def font(px):
    for p in ('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', '/usr/share/fonts/TTF/DejaVuSans-Bold.ttf'):
        try: return ImageFont.truetype(p, px)
        except OSError: pass
    return ImageFont.load_default()

def to565be(im):
    out = bytearray()
    for r, g, b in im.convert('RGB').getdata():
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out += bytes((v >> 8, v & 0xFF))
    return bytes(out)

def placeholder(i, w, h, keyed):
    hsh = zlib.crc32(b'%d' % i)
    col = (60 + hsh % 160, 60 + (hsh >> 8) % 160, 60 + (hsh >> 16) % 160)
    im = Image.new('RGB', (w, h), (0, 0, 0) if keyed else tuple(c // 4 for c in col))
    d = ImageDraw.Draw(im)
    if i in GLYPHS:
        f = font(max(8, int(h * 0.9)))
        d.text((w // 2, h // 2), GLYPHS[i], fill=(255, 255, 255), font=f, anchor='mm')
        return im
    d.rectangle([0, 0, w - 1, h - 1], outline=col)
    txt = '#%d' % i
    f = font(max(7, min(14, h - 2, w // 3)))
    d.text((w // 2, h // 2), txt, fill=col if keyed else (255, 255, 255), font=f, anchor='mm')
    return im

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--index', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--real'); ap.add_argument('--real-max-id', type=int, default=25)
    a = ap.parse_args()
    idx = json.load(open(a.index))['pictures']
    real = open(a.real, 'rb').read() if a.real else b''
    img = bytearray(b'\xff' * 0x800000)
    for e in idx:
        i, off, w, h = e['id'], e['offset'], e['w'], e['h']
        if i == 998:
            img[off:off + 25600] = to565be(placeholder(998, 80, 160, False)); continue
        if i >= 927:                         # zone overlay: {hi, lo, alpha}, an ellipse on transparent
            im = Image.new('RGB', (80, 44), (0, 0, 0)); d = ImageDraw.Draw(im)
            k = i - 927
            x0 = 4 + (k % 6) * 12
            d.ellipse([x0, 6, x0 + 14, 36], fill=(40, 200, 120) if k % 2 == 0 else (230, 140, 40))
            d.text((x0 + 7, 21), str(i), fill=(255, 255, 255), font=font(7), anchor='mm')
            px = to565be(im); buf = bytearray()
            for p in range(80 * 44):
                hi, lo = px[2 * p], px[2 * p + 1]
                buf += bytes((hi, lo, 0 if (hi | lo) == 0 else 0xFF))
            img[off:off + len(buf)] = buf; continue
        size = w * h * 2
        if real and i <= a.real_max_id and off + size <= len(real):
            img[off:off + size] = real[off:off + size]
        else:
            img[off:off + size] = to565be(placeholder(i, w, h, i in BLACK_KEY))
    open(a.out, 'wb').write(img)
    print('wrote', a.out, len(img))

if __name__ == '__main__':
    main()
