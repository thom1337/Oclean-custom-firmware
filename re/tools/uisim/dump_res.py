#!/usr/bin/env python3
"""Download the brush's OEM picture partition through the custom firmware's web API
(GET /api/res?off=&len=) into one file. Read-only on the brush.
    dump_res.py http://192.168.86.32 res_dump.bin
Afterwards: ui_extract.py --ota ota.bin --src res_dump.bin --out ui   (PNG of every picture)."""
import sys, urllib.request
base, out = sys.argv[1].rstrip('/'), sys.argv[2]
CH = 65536
with urllib.request.urlopen(base + '/api/res?off=0&len=16') as r:
    total = int(r.headers['X-Res-Size'])
print('partition size', total)
with open(out, 'wb') as f:
    off = 0
    while off < total:
        n = min(CH, total - off)
        with urllib.request.urlopen('%s/api/res?off=%d&len=%d' % (base, off, n), timeout=30) as r:
            d = r.read()
        if len(d) != n: sys.exit('short read at 0x%x: %d of %d' % (off, len(d), n))
        f.write(d); off += n
        print('\r%5.1f %%' % (100.0 * off / total), end='', flush=True)
print('\nwrote', out)
