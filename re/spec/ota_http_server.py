#!/usr/bin/env python3
"""
ota_http_server.py  --  minimal plaintext OTA server for the stock Oclean X Ultra 20.

Standard library only. Serves the custom app image to the brush after you have pointed
its "http_domain" at this box with ble_ota_flash.py. DO NOT RUN BLIND -- read
spec/ble_ota.md. This serves firmware to your own brush on your own LAN.

It answers exactly what the stock firmware (FUN_42011a60 / FUN_42013a94) needs:

  POST /OTA/v1/V1Brush/OTAUpGrade
        -> JSON: {"state":true,"code":0,"msg":"Success",
                  "data":{"isAppDown":true,
                          "otaFilePath":"http://<this-host>/ota.bin",
                          "newOTAVersion":"<ver>"}}
     Parser requires: state == boolean true, data object, data.isAppDown (bool),
     data.otaFilePath (string), data.newOTAVersion (string). No signature is checked.
     isAppDown:true makes the device queue a self-download (code 6) without needing
     the RTC one-shot flag.

  GET  /ota.bin   (or any path ending in ota.bin)
        -> the firmware, WITH HTTP Range (206) support -- esp_https_ota downloads in
           ~4096-byte ranged chunks. Sends Content-Length / Content-Range / Accept-Ranges.

  Any other POST (UploadBrushRecord, UploadingMacWiFi, OTAUpReceipt, OTAStatistics, ...)
        -> 200 {"state":true,"code":0,"data":"ok","msg":"Success"} so the brush, which
           now talks only to us, is never left hanging.

Usage:
    python3 ota_http_server.py --bind 0.0.0.0 --port 8080 --fw /path/oclean_custom_ota.bin
Then set the brush host to  http://<this-ip>:8080  (see ble_ota_flash.py).
"""

import argparse
import json
import os
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FW_PATH = None
VERSION = "1.3.3.7"
SELF_HOST = None        # "ip:port" the brush used to reach us; filled per-request

OTA_FILE_RE = re.compile(r"/ota\.bin$", re.IGNORECASE)


def _read_body(handler):
    n = int(handler.headers.get("Content-Length") or 0)
    return handler.rfile.read(n) if n else b""


def _send_json(handler, obj, code=200):
    body = json.dumps(obj).encode()
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Connection", "close")
    handler.end_headers()
    handler.wfile.write(body)


def _ota_reply(host_hdr):
    host = host_hdr or SELF_HOST or "127.0.0.1"
    return {
        "state": True,              # MUST be JSON boolean true (FUN_42103684/4210365c)
        "code": 0,
        "msg": "Success",
        "data": {
            "isAppDown": True,      # -> FUN_42011978 queues device-side OTA (code 6)
            "otaFilePath": f"http://{host}/ota.bin",   # used verbatim; http:// = plaintext
            "newOTAVersion": VERSION,                  # never compared by the device
            # harmless extras the production cloud sends; the parser ignores unknown fields
            "voiceFilePath": f"http://{host}/voc.bin",
            "imgFilePath": f"http://{host}/img.bin",
            "newVoiceVersion": VERSION,
            "newImgVersion": VERSION,
        },
    }


def _serve_file(handler, path):
    try:
        size = os.path.getsize(path)
        f = open(path, "rb")
    except OSError as e:
        handler.send_error(404, f"cannot open firmware: {e}")
        return

    rng = handler.headers.get("Range")
    start, end = 0, size - 1
    partial = False
    if rng:
        m = re.match(r"bytes=(\d*)-(\d*)", rng.strip())
        if m:
            a, b = m.group(1), m.group(2)
            if a == "" and b != "":                 # suffix range: last N bytes
                start = max(0, size - int(b))
                end = size - 1
            else:
                start = int(a) if a else 0
                end = int(b) if b else size - 1
            end = min(end, size - 1)
            if start > end or start >= size:
                handler.send_response(416)
                handler.send_header("Content-Range", f"bytes */{size}")
                handler.send_header("Content-Length", "0")
                handler.end_headers()
                f.close()
                return
            partial = True

    length = end - start + 1
    handler.send_response(206 if partial else 200)
    handler.send_header("Content-Type", "application/octet-stream")
    handler.send_header("Accept-Ranges", "bytes")
    handler.send_header("Content-Length", str(length))
    if partial:
        handler.send_header("Content-Range", f"bytes {start}-{end}/{size}")
    handler.send_header("Connection", "close")
    handler.end_headers()

    f.seek(start)
    remaining = length
    try:
        while remaining > 0:
            buf = f.read(min(65536, remaining))
            if not buf:
                break
            handler.wfile.write(buf)
            remaining -= len(buf)
    except (BrokenPipeError, ConnectionResetError):
        pass
    finally:
        f.close()
    kind = f"206 [{start}-{end}/{size}]" if partial else f"200 full {size}B"
    print(f"    >>> SERVE ota.bin {kind}  ({length}B)")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print(f"[{self.client_address[0]}] " + (fmt % args))

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if OTA_FILE_RE.search(path) or path.endswith("/ota.bin"):
            _serve_file(self, FW_PATH)
        elif path.endswith("voc.bin") or path.endswith("img.bin"):
            # not needed for the app flash; 404 so the device skips the resource OTA
            self.send_error(404, "resource image not served")
        else:
            _send_json(self, {"state": True, "code": 0, "data": "ok", "msg": "Success"})

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        body = _read_body(self)
        snippet = body[:400].decode("utf-8", "replace")
        print(f"[{self.client_address[0]}] POST {path}  body={snippet}")
        if path.endswith("/OTA/v1/V1Brush/OTAUpGrade"):
            host = self.headers.get("Host")
            reply = _ota_reply(host)
            print(f"    <<< OTAUpGrade -> isAppDown=True otaFilePath={reply['data']['otaFilePath']}")
            _send_json(self, reply)
        else:
            # UploadBrushRecord wants data=="ok"; everyone else is happy with 200.
            _send_json(self, {"state": True, "code": 0, "data": "ok", "msg": "Success"})


def main():
    global FW_PATH, VERSION, SELF_HOST
    ap = argparse.ArgumentParser(description="Minimal Oclean OTA server (stdlib only).")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--fw", required=True, help="app image to serve as ota.bin "
                                                "(a valid ESP32-S3 image, e.g. oclean_custom_ota.bin)")
    ap.add_argument("--version", default="1.3.3.7", help="newOTAVersion to advertise (cosmetic)")
    ap.add_argument("--self-host", help='how the brush reaches us, "ip:port"; '
                                        "default: the request Host header")
    args = ap.parse_args()

    if not os.path.isfile(args.fw):
        raise SystemExit(f"firmware not found: {args.fw}")
    with open(args.fw, "rb") as f:
        magic = f.read(1)
    if magic != b"\xe9":
        print(f"[!] warning: {args.fw} does not start with 0xE9 (not an ESP32 app image?)")

    FW_PATH = args.fw
    VERSION = args.version
    SELF_HOST = args.self_host
    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"[*] serving {args.fw} ({os.path.getsize(args.fw)} bytes) on "
          f"http://{args.bind}:{args.port}")
    print(f"[*] set the brush host to  http://<this-ip>:{args.port}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[*] bye")


if __name__ == "__main__":
    main()
