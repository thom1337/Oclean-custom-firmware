# Stock → custom, over the air: the OTA man-in-the-middle method

This is how the custom firmware was first installed on a **stock** Oclean X Ultra 20
**without opening the device or using a UART**. The X Ultra 20 has no easily accessible
serial port, and the stock firmware only pulls new firmware from Oclean's cloud OTA. So you
stand in the middle of that OTA check on your own LAN and serve your own image in place of
the cloud's.

It is for flashing **your own brush on your own network.** Once the custom firmware is
running you never need this again — all later updates go through the web UI / `/api/ota`
(see the repo's top-level README, "Flashing"). A root-free alternative that steers the OTA
host over BLE instead of ARP-spoofing is written up in
[`../re/spec/ble_ota.md`](../re/spec/ble_ota.md); it was not needed and is untried on a
device. **This** method is the one that actually worked.

> Nothing here touches the bootloader or the partition table, and nothing is flashed over
> UART. The brush writes the image itself through its own `esp_ota` path, exactly as it
> would a cloud update, so the OEM picture partition and the stock partition layout are left
> intact. A failed or interrupted transfer cannot brick the device: `esp_ota` verifies the
> image's checksum + appended SHA-256 before it marks the new slot bootable, so a bad
> download just leaves the brush running stock (see [Recovery](#recovery)).

## Why it works (the reverse-engineered facts)

Three findings from capturing the stock firmware's OTA traffic (`oclean_recon_addon.py`
produced the capture):

1. **No TLS pinning.** The brush accepts a transparent mitmproxy's own certificate, so its
   HTTPS OTA traffic is readable and rewritable without installing anything on the brush.
2. **The OTA check is a simple JSON handshake.** The brush polls
   `POST https://hwapicore.oclean.com/OTA/v1/V1Brush/OTAUpGrade` with the versions it is
   running; the cloud replies with the version to install and a URL to fetch. When the brush
   is **already on the cloud's latest** (it was, at `0.0.1.6`) the reply carries an **empty**
   `otaFilePath` — there is nothing newer to download:

   ```
   # request (brush -> cloud)                # genuine response, brush already up to date
   {                                          {"state":true,"code":0,"msg":"Success","data":{
     "mac":   "AA:BB:CC:DD:EE:FF",              "isAppDown": true,
     "model": "OCLEANV20",                      "newOTAVersion": "0.0.1.6",
     "ota":   "0.0.1.6",                        "otaFilePath": "",          # <- nothing to flash
     "voice": "0.0.1.6",                        "voiceFilePath": "",
     "img":   "0.0.1.6"                         "imgFilePath": ""
   }                                          }}
   ```
3. **So "already up to date" is the only obstacle.** Rewriting just the version numbers in
   that reply leaves the brush with no URL to fetch, and nothing happens. (That dead end is
   exactly what the first attempts hit.)

**The move that worked:** *downgrade the version the brush reports in its request* to
`0.0.0.1`. The genuine, signed cloud reply then offers a real upgrade with a populated path
and `isAppDown:false`:

```
# request rewritten on the way out            # genuine reply to the downgraded request
  "ota": "0.0.0.1", …                           "isAppDown": false,
                                                 "newOTAVersion": "0.0.1.1",
                                                 "otaFilePath": "https://…aliyuncs.com/ota/v20011.bin", …
```

We pass that genuine reply through but **rewrite** it so (a) the version reads high
(`1.3.3.7`, so the brush treats it as an upgrade and won't immediately re-offer) and (b)
every file path points at a plain-`http://` host the brush resolves, with a path ending in
`/ota.bin`. `iptables` has redirected the brush's port 80 to our proxy, and the addon serves
the local custom image for anything ending in `/ota.bin`. Range requests are supported,
though the successful flash was a single full GET.

The served image must be a valid ESP32-S3 app image (magic `0xE9`, correct checksum +
appended SHA-256) that fits the stock OTA partition. `oclean_custom_ota.bin`
(== `build/oclean_custom.bin`) satisfies this; `espimg.py` verifies it.

## What's in this directory

| File | What it is |
|---|---|
| `oclean_mitm.py` | Root orchestrator: auto-detects your iface/gateway, enables IP forwarding, ARP-spoofs the brush↔gateway pair, redirects the brush's :80/:443 to a local mitmproxy, and runs `mitmdump` with an addon. Restores everything on Ctrl-C. |
| `oclean_flash_addon.py` | The mitmproxy addon that does the request-downgrade + response-rewrite above and serves your image. **This is the one that flashes.** |
| `oclean_recon_addon.py` | Capture-only addon. Run it first to confirm the brush accepts the proxy (no TLS pinning) and to see / save its real OTA handshake. Rewrites nothing. |
| `espimg.py` | Verify (and, if you hand-patch one, re-checksum/re-hash) an ESP-IDF app image. |

Not shipped, on purpose: the OEM `ota.bin` / `voc.bin` / `img.bin` (Oclean's firmware, not
ours) and the `captures/` directory (it holds your device MAC and per-session cloud tokens).
`captures/`, `fw/` and `venv/` under `flash/` are git-ignored.

## Prerequisites

- A **Linux host on the same LAN as the brush**, with **layer-2 reachability** to it (this
  is ARP spoofing — it needs the host and brush on one subnet). Turn **off** any Wi-Fi
  AP/client isolation or guest-network separation; with those on, the brush keeps reaching
  the real cloud, its `OTAUpGrade` never shows up in `captures/flash.log`, and there is no
  error to tell you why. A wired host on the brush's subnet is the most reliable. The host
  also needs working internet of its own — the method *relies* on reaching the genuine
  Oclean cloud to get a signed upgrade offer.
- **Python with `scapy` and `mitmproxy`**, plus the `mitmdump` binary. Because the tool runs
  under `sudo`, install them where **root** can see them:
  ```
  sudo pip3 install scapy mitmproxy        # system-wide: a plain `sudo python3 …` then works
  ```
  If you prefer a venv, remember `sudo` uses the system Python/PATH, so call the venv's
  interpreter and point the tool at the venv's `mitmdump`:
  ```
  sudo flash/venv/bin/python3 oclean_mitm.py --brush <IP> --mitmdump flash/venv/bin/mitmdump
  ```
  `dumpcap`/`tshark` are optional (for the pcap).
- **`oclean_custom_ota.bin` at the repo root.** It is a build artifact (git-ignored), so
  build it first. From the **repo root** (one level above `flash/`), with ESP-IDF set up as
  in the top-level README's Build section:
  ```
  . $IDF_PATH/export.sh            # ESP-IDF v5.1.1
  idf.py set-target esp32s3
  idf.py build
  cp build/oclean_custom.bin oclean_custom_ota.bin
  python3 flash/espimg.py verify oclean_custom_ota.bin    # expect checksum + sha256 MATCH
  ```
  (`oclean_flash_addon.py`'s default `OTA_FILE` is this repo-root file.)
- The brush **provisioned to your Wi-Fi**, **on its charger**, **battery > 20 %**. The flash
  that worked was done with the brush docked; keep it docked. (The BLE write-up claims
  off-charger is required — that contradicts what actually worked here, so go by this.)
- The brush's **IP** (from your router's DHCP table). Oclean devices use the OUI prefix
  `e8:06:90:…`, which helps you spot it in the lease list; the custom firmware's own Wi-Fi
  MAC is shown on the brush's web dashboard once it's running.

## Do the flash

Everything runs from this `flash/` directory (adjust the `python3` invocation per the venv
note above if you didn't install system-wide).

**1 — (recommended) recon pass.** Confirm the proxy is accepted and watch the real handshake:

```
sudo python3 oclean_mitm.py --brush <BRUSH_IP> --addon oclean_recon_addon.py
```

Trigger an update check from the Oclean app, or just wait — the brush re-polls on its own
every ~45–90 s while awake on the charger. In `captures/flows.log` you want to see the
`OTAUpGrade` request and a decrypted JSON reply. If instead you see a `[TLS] brush refused
our cert` line, the brush is validating TLS on that endpoint and this method won't work as is
— stop here. (On this unit it was accepted.) Ctrl-C to stop.

**2 — the flash.** Run the flash addon (the default) and trigger an OTA poll the same way:

```
sudo python3 oclean_mitm.py --brush <BRUSH_IP>         # addon defaults to oclean_flash_addon.py
```

When you see `TRIGGER THE OTA NOW`, start the firmware upgrade from the Oclean app, or reboot
the brush on its charger so it re-polls. Watch `captures/flash.log`; a successful flash logs
the downgrade, the rewrite, the serve, and the brush's receipt (one real run, MAC redacted):

```
[BRUSH ->] POST …/OTA/v1/V1Brush/OTAUpGrade
    req-body: {"mac":"AA:BB:CC:DD:EE:FF","model":"OCLEANV20","ota":"0.0.1.6", …}
    <<< request downgraded: {'ota': '0.0.1.6', …} -> 0.0.0.1
    OTAUpGrade resp (after request-downgrade): {…"isAppDown":false,"newOTAVersion":"0.0.1.1",
        "otaFilePath":"https://…aliyuncs.com/ota/v20011.bin", …}
    >>> OTAUpGrade rewritten: {'newOTAVersion':'0.0.1.1', …} -> v1.3.3.7, paths forced
        serving ota=http://hwapicore.oclean.com/upload/ota/OCLEANV20/0.0.1.6/ota.bin
    >>> SERVE ota.bin FULL 1237600B from oclean_custom_ota.bin (magic=e906024f)
[BRUSH ->] POST …/OTA/v1/V1Brush/OTAUpReceipt           # <- brush acknowledges, then reboots
```

(The served size just tracks your build — runs here ranged ~1.24–1.36 MB as the firmware
grew.) `SERVE ota.bin … → OTAUpReceipt → reboot into the custom image.` The stock bootloader
has rollback disabled, so a fully-flashed valid image is permanent. Ctrl-C the tool; it tears
down the ARP spoof and the iptables/ip-rule changes it made.

## Running under a policy-routing VPN (Tailscale, etc.)

If the host runs a VPN that installs its own routing table (Tailscale uses table 52),
forwarded brush traffic can get captured by it and never reach the real cloud. Add
`--vpn-bypass` (or `OCLEAN_VPN_BYPASS=1`) to insert `ip rule add from <brush> lookup main` so
the brush's packets use the main table. It's harmless if you have no such VPN.

## Failure modes seen

- **Download never starts; reply has empty `otaFilePath`.** The brush is already on the
  cloud's latest, so only the request-downgrade gets a real URL back. Make sure
  `DOWNGRADE_REQ` is on (it is by default).
- **Rewrote the response but nothing downloads.** The rewritten path must be on a host the
  **brush** can resolve and must be plain `http://` (so the :80 redirect catches it). A
  fabricated OSS hostname the brush can't resolve fails silently; `hwapicore.oclean.com`
  (which the brush already talks to) works.
- **Screen shows "firmware upgrade failed" after a full download.** The served image's
  checksum/SHA-256 was broken (usually a hand-edit without re-hashing). Re-verify with
  `espimg.py verify` and serve a clean image.
- **The brush's `OTAUpGrade` never appears in the log.** You don't have layer-2 reachability
  — AP/client isolation or a guest network is in the way (see Prerequisites).
- **`[TLS] brush refused our cert`.** That endpoint pins/validates TLS; the body can't be
  rewritten. Not observed on this unit.

## Recovery

No serial is needed to recover, and a bad transfer cannot brick the brush — `esp_ota`
validates the image before booting it, so a truncated/corrupt download simply leaves stock
running and shows "firmware upgrade failed". Re-verify with `espimg.py verify` and trigger
another poll to retry; there is no stuck state to get into.

A freshly-flashed brush with no saved Wi-Fi comes up as the `oclean-setup` AP at
`http://192.168.4.1/`, with the WPA3 passcode on its screen (press the button if it is dark: on the dock the screen goes off 30 s after boot; in safe mode, which has no
screen, the AP is open); its **Firmware** tab (and **Logs** tab) work, so you can reflash
from there. Its **Settings** tab takes your Wi-Fi network together with a web UI password:
the brush joins no network without one (it then stays the setup AP).

To go **back to stock**, flash the genuine OEM `ota.bin` through `/api/ota` on the running
custom firmware (`curl -u oclean …` once a web password is set). That image is Oclean's and is not in this repo, and you can't fetch it once
the brush is "up to date" (empty `otaFilePath`) — so **capture it beforehand**: with the
request-downgrade on, the cloud serves a genuine signed image (`v20011.bin`, 0.0.1.1), which
the recon addon saves to `captures/`. Keep that file if you ever want to revert.

## Legal / ethical note

This modifies a device you own, on a network you control, by serving your own firmware to
your own brush. It does not attack Oclean's servers or anyone else's devices, and it
distributes no Oclean firmware — the OEM images and your captured traffic stay off this
repository. Not affiliated with or endorsed by Oclean.
