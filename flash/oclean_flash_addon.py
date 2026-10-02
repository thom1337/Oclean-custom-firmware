"""mitmproxy addon: make the Oclean X Ultra 20's stock OTA flash *your* image.

Run it through oclean_mitm.py (which sets up the ARP spoof + iptables redirect
and launches mitmdump with this addon). See flash/README.md for the whole story;
the short version of what the captured handshake told us:

  * The stock firmware does NOT pin TLS, so transparent mitmproxy with its own
    cert sees the OTA traffic in the clear.
  * It polls  POST .../OTA/v1/V1Brush/OTAUpGrade  with the versions it is running
    {"ota","voice","img"}. The cloud replies with JSON:
        data.isAppDown      false = device-side flash (what we want)
        data.newOTAVersion  the offered version
        data.otaFilePath    URL the device will GET and esp_ota-write
    When the brush is already on the cloud's latest, otaFilePath comes back EMPTY
    and there is nothing to flash.

  * The trick (this is what finally worked — see captures/flash.log in the repo
    history): DOWNGRADE the version the brush *reports* in the request to
    OLD_VERSION, so the genuine, signed reply offers a real upgrade with a real
    path and isAppDown=false. Then REWRITE that reply so the version reads high
    (TARGET_VERSION, won't offer again) and every *FilePath points at an http://
    host the brush resolves and whose path ends in /ota.bin — iptables has
    redirected :80 to us, and the request handler below serves the local image
    by that /ota.bin suffix (range/206 supported; the real flash was one full GET).

Config via env (all optional):
  BRUSH_IP        only touch this client's flows          (default 192.168.0.1 placeholder)
  OUT_DIR         log dir                                  (default <flash>/captures)
  OTA_FILE        app image served for */ota.bin           (default <repo>/oclean_custom_ota.bin)
  VOC_FILE        */voc.bin, only if present               (default <flash>/fw/voc.bin)
  IMG_FILE        */img.bin, only if present               (default <flash>/fw/img.bin)
  TARGET_VERSION  high version advertised to the brush      (default 1.3.3.7)
  OLD_VERSION     version the request is downgraded to      (default 0.0.0.1)
  REWRITE_HOST    http host + base path the reply points at (default hwapicore.oclean.com/...)
  DOWNGRADE_REQ   set to 0 to disable the request downgrade (default on)
  REWRITE_RESP    set to 0 to disable the response rewrite  (default on)

Note on voc/img: the custom firmware does not need the OEM voice/image blobs, and
those files are Oclean's, not ours, so they are NOT shipped in this repo. If the
brush asks for voc.bin/img.bin and no local file exists, this addon lets the
request go through to the genuine cloud untouched.
"""
import json
import logging
import os

from mitmproxy import http

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

BRUSH = os.environ.get("BRUSH_IP", "192.168.0.1")
OUT = os.environ.get("OUT_DIR", os.path.join(HERE, "captures"))
TARGET_VERSION = os.environ.get("TARGET_VERSION", "1.3.3.7")
OLD_VERSION = os.environ.get("OLD_VERSION", "0.0.0.1")
# host + base path to point the download at. Must RESOLVE from the brush and be
# plain http:// (port 80) so the iptables REDIRECT catches it; the version
# segment is cosmetic (we serve by the /ota.bin suffix). hwapicore.oclean.com is
# proven resolvable — the brush talks to it every heartbeat.
REWRITE_BASE = os.environ.get(
    "REWRITE_HOST", "http://hwapicore.oclean.com/upload/ota/OCLEANV20/0.0.1.6")


def _flag(name, default):
    return os.environ.get(name, default) not in ("", "0", "false", "False")


DOWNGRADE_REQ = _flag("DOWNGRADE_REQ", "1")
REWRITE_RESP = _flag("REWRITE_RESP", "1")

SERVE = {
    "ota.bin": os.environ.get("OTA_FILE", os.path.join(REPO, "oclean_custom_ota.bin")),
    "voc.bin": os.environ.get("VOC_FILE", os.path.join(HERE, "fw", "voc.bin")),
    "img.bin": os.environ.get("IMG_FILE", os.path.join(HERE, "fw", "img.bin")),
}

os.makedirs(OUT, exist_ok=True)
log = logging.getLogger("oclean-flash")
_fh = logging.FileHandler(os.path.join(OUT, "flash.log"))
_fh.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
log.addHandler(_fh)
log.setLevel(logging.INFO)


def is_brush(flow):
    try:
        return flow.client_conn.peername[0] == BRUSH
    except Exception:
        return False


def _which_file(path):
    """(name, local_path) if this request path is a firmware file we serve, else (None, None)."""
    for name, local in SERVE.items():
        if path.endswith("/" + name) or path.endswith(name):
            return name, local
    return None, None


class OcleanFlash:
    def __init__(self):
        self.served = {}

    # intercept the firmware downloads before they hit upstream, and downgrade the
    # version the brush reports so the cloud offers a genuine upgrade.
    def request(self, flow):
        if not is_brush(flow):
            return
        name, local = _which_file(flow.request.path.split("?")[0])
        if not name:
            log.warning(f"[BRUSH ->] {flow.request.method} {flow.request.pretty_url}")
            b = flow.request.raw_content or b""
            if b and len(b) < 2048 and any(
                    k in flow.request.path for k in ("OTAUpGrade", "OTAStatistics", "GetOTACounterMode")):
                log.warning(f"    req-body: {b.decode('utf-8', 'replace')}")
            if DOWNGRADE_REQ and flow.request.path.endswith("OTAUpGrade") and b:
                try:
                    body = json.loads(b.decode("utf-8", "replace"))
                    orig = {k: body.get(k) for k in ("ota", "voice", "img")}
                    for k in ("ota", "voice", "img"):
                        if k in body:
                            body[k] = OLD_VERSION
                    flow.request.set_text(json.dumps(body))
                    log.warning(f"    <<< request downgraded: {orig} -> {OLD_VERSION}")
                except Exception as e:
                    log.warning(f"    request downgrade failed: {e}")
            return
        if not local or not os.path.exists(local):
            log.warning(f"    !!! no local {name} ({local}) -- letting the genuine download pass")
            return
        data = open(local, "rb").read()
        total = len(data)
        rng = flow.request.headers.get("range") or flow.request.headers.get("Range")
        if rng and rng.lower().startswith("bytes="):
            try:
                spec = rng.split("=", 1)[1].split(",")[0]
                a, _, b = spec.partition("-")
                start = int(a) if a else 0
                end = int(b) if b else total - 1
                end = min(end, total - 1)
                if start > end or start >= total:
                    flow.response = http.Response.make(416, b"", {"Content-Range": f"bytes */{total}"})
                    log.warning(f"    >>> {name}: bad range {rng} -> 416")
                    return
                chunk = data[start:end + 1]
                flow.response = http.Response.make(
                    206, chunk,
                    {"Content-Type": "application/octet-stream",
                     "Content-Range": f"bytes {start}-{end}/{total}",
                     "Accept-Ranges": "bytes"})
                self.served[name] = self.served.get(name, 0) + len(chunk)
                log.warning(f"    >>> SERVE {name} [{start}-{end}/{total}] "
                            f"{len(chunk)}B from {os.path.basename(local)}")
                return
            except Exception as e:
                log.warning(f"    range parse failed ({rng}): {e} -- serving whole file")
        flow.response = http.Response.make(
            200, data, {"Content-Type": "application/octet-stream", "Accept-Ranges": "bytes"})
        self.served[name] = total
        log.warning(f"    >>> SERVE {name} FULL {total}B from {os.path.basename(local)} "
                    f"(magic={data[:4].hex()})")

    # rewrite the OTAUpGrade reply so the brush fetches OUR image.
    def response(self, flow):
        if not is_brush(flow):
            return
        url = flow.request.pretty_url or ""
        if any(k in url for k in ("OTAStatistics", "GetOTACounterMode", "GetAdvertisingLink")):
            try:
                log.warning(f"    {url.rsplit('/', 1)[-1]} resp: {flow.response.get_text()[:400]}")
            except Exception:
                pass
            return
        if not url.endswith("OTAUpGrade"):
            return
        try:
            raw = flow.response.get_text()
            log.warning(f"    OTAUpGrade resp (after request-downgrade): {raw[:500]}")
            obj = json.loads(raw)
        except Exception as e:
            log.warning(f"    OTAUpGrade: not JSON ({e})")
            return
        if not REWRITE_RESP:
            return
        d = obj.get("data")
        if not isinstance(d, dict):
            d = {}
        before = {k: d.get(k) for k in ("newOTAVersion", "otaFilePath")}
        d["newOTAVersion"] = TARGET_VERSION
        d["newVoiceVersion"] = TARGET_VERSION
        d["newImgVersion"] = TARGET_VERSION
        d["otaFilePath"] = REWRITE_BASE + "/ota.bin"
        d["voiceFilePath"] = REWRITE_BASE + "/voc.bin"
        d["imgFilePath"] = REWRITE_BASE + "/img.bin"
        d["isAppDown"] = False  # device-side flash, not app-assisted
        obj["data"] = d
        obj["state"] = True
        obj["code"] = 0
        obj.setdefault("msg", "Success")
        flow.response.set_text(json.dumps(obj))
        log.warning(f"    >>> OTAUpGrade rewritten: {before} -> v{TARGET_VERSION}, paths forced")
        log.warning(f"        serving ota={d['otaFilePath']}")


addons = [OcleanFlash()]
