"""mitmproxy addon: capture (don't modify) the Oclean X Ultra 20's OTA traffic.

Run this one FIRST, before the flash addon, to confirm the brush accepts a
transparent proxy (i.e. does not pin TLS) and to see its real OTA handshake:

    sudo python3 oclean_mitm.py --brush <ip> --addon oclean_recon_addon.py

It logs every flow from the brush to <out>/flows.log, saves firmware-shaped
response bodies, and if an OTA-check reply leaks a firmware URL it downloads it
directly so you have the genuine image for reference. Nothing is rewritten.

NOTE the captures contain your device's identifiers and per-session cloud tokens
(MAC, the encrypted SN blob, the security/encryption fields). They are written
under flash/captures/, which is git-ignored — keep it that way; do not commit them.
"""
import logging
import os
import re
import time
import urllib.request

log = logging.getLogger("oclean")
BRUSH = os.environ.get("BRUSH_IP", "192.168.0.1")
OUT = os.environ.get("OUT_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures"))
os.makedirs(OUT, exist_ok=True)
_fh = logging.FileHandler(os.path.join(OUT, "flows.log"))
_fh.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
log.addHandler(_fh)
log.setLevel(logging.INFO)

URLRE = re.compile(rb'https?://[^\s"\'<>\\)]+')
FW_HINT = re.compile(r'\.bin|firmware|fota|/ota|upgrade|download|oss|aliyun', re.I)


def is_brush(flow):
    try:
        return flow.client_conn.peername[0] == BRUSH
    except Exception:
        return False


class OcleanCap:
    def __init__(self):
        self.n = 0
        self.seen_urls = set()

    def _save(self, data, tag):
        fn = os.path.join(OUT, f"{tag}_{self.n:03d}_{int(time.time())}.bin")
        self.n += 1
        with open(fn, "wb") as f:
            f.write(data)
        return fn

    def request(self, flow):
        if not is_brush(flow):
            return
        log.warning(f"[BRUSH -> ] {flow.request.method} {flow.request.pretty_url}")
        b = flow.request.raw_content or b""
        if b and len(b) < 2048:
            try:
                log.warning(f"           req-body: {b.decode('utf-8', 'replace')}")
            except Exception:
                pass

    def response(self, flow):
        if not is_brush(flow):
            return
        r, req = flow.response, flow.request
        ct = r.headers.get("content-type", "")
        body = r.raw_content or b""
        log.warning(f"[ -> BRUSH] {req.pretty_url}  {r.status_code} {ct} {len(body)}B")

        # firmware binary? ESP32 app image magic byte is 0xE9
        looks_fw = (body[:1] == b"\xe9") or ("octet-stream" in ct) \
            or FW_HINT.search(req.pretty_url or "") \
            or (len(body) > 40000 and b"\x00\x00\x00" in body[:64])
        if looks_fw and body:
            fn = self._save(body, "firmware")
            log.warning(f"    >>> SAVED FIRMWARE-LIKE BODY -> {fn}  ({len(body)}B magic={body[:4].hex()})")

        if body and ("json" in ct or "text" in ct or len(body) < 4096):
            try:
                log.warning(f"    body: {body.decode('utf-8', 'replace')[:800]}")
            except Exception:
                pass
        for m in URLRE.findall(body[:40000]):
            u = m.decode("latin1")
            if u in self.seen_urls:
                continue
            if FW_HINT.search(u):
                self.seen_urls.add(u)
                log.warning(f"    >>> FIRMWARE URL CANDIDATE: {u}")
                try:
                    data = urllib.request.urlopen(u, timeout=40).read()
                    fn = self._save(data, "urldl")
                    log.warning(f"        DOWNLOADED {len(data)}B -> {fn}  magic={data[:4].hex()}")
                except Exception as e:
                    log.warning(f"        direct download failed: {e}")

    def tls_failed_client(self, data):
        # if we ever see this, the brush rejected our cert -> it validates/pins TLS
        # and we cannot read the body. (Observed NOT to happen on this unit.)
        try:
            log.warning(f"[TLS] brush refused our cert for SNI={getattr(data, 'sni', None)} "
                        f"— endpoint is TLS-validated (can't decrypt)")
        except Exception:
            pass


addons = [OcleanCap()]
