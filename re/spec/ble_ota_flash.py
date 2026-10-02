#!/usr/bin/env python3
"""
ble_ota_flash.py  --  steer a STOCK Oclean X Ultra 20 to a chosen OTA host over BLE.

DO NOT RUN BLIND. This connects to the brush and REBOOTS it. Read spec/ble_ota.md first.
Pre-req: run ota_http_server.py on <ip>:<port> on the same LAN the brush is joined to,
keep the brush OFF the charger with battery >= 20 % (see the erratum in ble_ota.md), and know its BLE address.

What it does (all over the custom command characteristic BB85, no pairing needed):
  1. write cat=0x02 op=0x33  ->  set NVS "http_domain" = "http://<ip>:<port>"
  2. write cat=0x07 op=0x07 00 ->  esp_restart() (a true reset; re-arms the OTA flag)
After the reboot the brush rejoins Wi-Fi and POSTs its OTA check to your server.

Everything below is from reverse engineering the stock image (see spec/ble_ota.md):
  - service   8082caa8-41a6-4021-91c6-56f9b954cc18
  - BB85 cmd  9d84b9a3-000c-49d8-9183-855b673fbb85   (WRITE, perm 0x10, no encryption)
  - BB86 resp 5f78df94-798c-46f5-990a-855b673fbb86   (NOTIFY; optional, for OK echoes)

Requires: pip install bleak
"""

import argparse
import asyncio

from bleak import BleakClient, BleakScanner

CMD_CHAR    = "9d84b9a3-000c-49d8-9183-855b673fbb85"   # BB85 write
RESP_CHAR   = "5f78df94-798c-46f5-990a-855b673fbb86"   # BB86 notify (OK echoes)
SERVICE     = "8082caa8-41a6-4021-91c6-56f9b954cc18"

CAT_CONFIG  = 0x02
OP_SET_HOST = 0x33
CAT_CONTROL = 0x07
OP_REBOOT   = 0x07

MAX_DOMAIN  = 59          # dst buffer is 60 bytes incl NUL  (FUN_4200ffe4 memset 0x3c)


def build_set_host_frames(domain: str, chunk: int = 15):
    """Return the list of BLE write payloads that set http_domain = domain.

    Framing recovered from FUN_4200ffe4:
      first packet : 02 33 2A <total_len> <pkt_len> <data...>
      continuation : 02 33 <pkt_len> <data...>
    total_len = len(domain) (no NUL); the handler accumulates until idx==total_len,
    then writes NVS. 'chunk' keeps each frame small enough for a 23-byte ATT MTU
    (<=20 value bytes); raise it if you negotiate a bigger MTU.
    """
    data = domain.encode("ascii")
    if len(data) > MAX_DOMAIN:
        raise ValueError(f"domain too long: {len(data)} > {MAX_DOMAIN} bytes")
    total = len(data)
    frames = []
    # first packet carries the '*' marker, total length, and this packet's length
    first = data[:chunk]
    frames.append(bytes([CAT_CONFIG, OP_SET_HOST, 0x2A, total, len(first)]) + first)
    # continuations: 02 33 <pkt_len> <data...>
    i = chunk
    while i < total:
        part = data[i:i + chunk]
        frames.append(bytes([CAT_CONFIG, OP_SET_HOST, len(part)]) + part)
        i += len(part)
    return frames


def build_reboot_frame():
    """07 07 00  ->  FUN_42010550: value[2]==0 => esp_restart()."""
    return bytes([CAT_CONTROL, OP_REBOOT, 0x00])


async def run(address: str, domain: str, do_reboot: bool, mtu: int, settle: float):
    def on_resp(_handle, data: bytearray):
        print(f"  <- notify {bytes(data).hex()}")

    print(f"[*] connecting to {address} ...")
    async with BleakClient(address) as client:
        print(f"[*] connected: {client.is_connected}")
        # Best-effort MTU bump (BlueZ negotiates automatically; some backends expose this).
        try:
            if hasattr(client, "_backend") and hasattr(client._backend, "_acquire_mtu"):
                await client._backend._acquire_mtu()
            print(f"[*] MTU ~= {getattr(client, 'mtu_size', 'unknown')}")
        except Exception as e:
            print(f"[!] MTU query skipped: {e}")

        try:
            await client.start_notify(RESP_CHAR, on_resp)
            print("[*] subscribed to BB86 for OK echoes")
        except Exception as e:
            print(f"[!] could not subscribe to BB86 (continuing): {e}")

        # 1) set host  --------------------------------------------------------
        frames = build_set_host_frames(domain, chunk=min(15, max(1, mtu - 5)))
        print(f"[*] set http_domain = {domain!r}  ({len(frames)} frame(s))")
        for n, f in enumerate(frames):
            print(f"    -> frame {n}: {f.hex()}")
            # response=True so we learn if the write was rejected (perm/handle).
            await client.write_gatt_char(CMD_CHAR, f, response=True)
            await asyncio.sleep(0.2)

        await asyncio.sleep(settle)   # let it commit NVS

        # 2) reboot  ----------------------------------------------------------
        if do_reboot:
            rb = build_reboot_frame()
            print(f"[*] reboot: {rb.hex()}  (brush will disconnect & restart)")
            try:
                # WRITE_NO_RSP: the device resets mid-write, so don't wait for a response.
                await client.write_gatt_char(CMD_CHAR, rb, response=False)
            except Exception as e:
                print(f"[!] reboot write errored (expected if it reset first): {e}")
        else:
            print("[*] --no-reboot: host set; reboot it yourself (or long-press reboot).")

    print("[*] done. Keep the brush OFF the charger and leave it alone: the update check runs when its idle timer expires (30-60 s). Watch ota_http_server.py logs.")


def main():
    ap = argparse.ArgumentParser(description="Set Oclean OTA host over BLE, then reboot.")
    ap.add_argument("--address", help="brush BLE address (skip to scan)")
    ap.add_argument("--host", required=True,
                    help='OTA host to inject, e.g. "http://192.168.86.39:8080" '
                         '(must start with http:// and be <= 59 bytes)')
    ap.add_argument("--no-reboot", action="store_true", help="set host but do not reboot")
    ap.add_argument("--mtu", type=int, default=20, help="assumed ATT value size (default 20)")
    ap.add_argument("--settle", type=float, default=1.0, help="seconds to wait after set-host")
    ap.add_argument("--scan", action="store_true", help="just scan and list devices")
    args = ap.parse_args()

    if not args.host.startswith("http://"):
        raise SystemExit('refusing: --host must start with "http://" (forces plaintext; '
                         'otherwise the firmware falls back to https://test.oclean.com)')

    async def _scan():
        print("[*] scanning 8s ...")
        for d in await BleakScanner.discover(timeout=8.0):
            print(f"    {d.address}  {d.name}")

    if args.scan or not args.address:
        asyncio.run(_scan())
        if not args.address:
            raise SystemExit("re-run with --address <addr>")
        return

    asyncio.run(run(args.address, args.host, not args.no_reboot, args.mtu, args.settle))


if __name__ == "__main__":
    main()
