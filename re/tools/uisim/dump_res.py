#!/usr/bin/env python3
"""Download the brush's OEM picture partition through the custom firmware's web API
(GET /api/res?off=&len=) into one file. Read-only on the brush.
    dump_res.py http://<ip> res_dump.bin
    OCLEAN_PASS='…' dump_res.py http://<ip> res_dump.bin     (a brush with a web password)
The password goes with the first request only; the session cookie the brush answers
with carries the rest (each password check costs the brush about a second).
Afterwards: ui_extract.py --ota ota.bin --src res_dump.bin --out ui   (PNG of every picture)."""
import base64, os, sys, urllib.error, urllib.request
base, out = sys.argv[1].rstrip('/'), sys.argv[2]
CH = 65536
hdrs = {}
if os.environ.get('OCLEAN_PASS'):
    hdrs['Authorization'] = 'Basic ' + base64.b64encode(('oclean:' + os.environ['OCLEAN_PASS']).encode()).decode()

def get(url, timeout=30):
    with urllib.request.urlopen(urllib.request.Request(url, headers=hdrs), timeout=timeout) as r:
        cookie = r.headers.get('Set-Cookie', '').split(';')[0]
        if cookie.startswith('oclean='):
            hdrs.pop('Authorization', None); hdrs['Cookie'] = cookie
        return r.headers, r.read()

def refused(e, where=''):
    if e.code != 401: sys.exit(where + str(e))
    sys.exit(where + ('the brush refused the password in OCLEAN_PASS' if os.environ.get('OCLEAN_PASS')
                      else 'the brush wants its web password: set OCLEAN_PASS'))

try:
    h, _ = get(base + '/api/res?off=0&len=16')
except urllib.error.HTTPError as e:
    refused(e)
total = int(h['X-Res-Size'])
print('partition size', total)
with open(out, 'wb') as f:
    off = 0
    while off < total:
        n = min(CH, total - off)
        try:
            _, d = get('%s/api/res?off=%d&len=%d' % (base, off, n))
        except urllib.error.HTTPError as e:
            refused(e, 'at 0x%x: ' % off)
        if len(d) != n: sys.exit('short read at 0x%x: %d of %d' % (off, len(d), n))
        f.write(d); off += n
        print('\r%5.1f %%' % (100.0 * off / total), end='', flush=True)
print('\nwrote', out)
