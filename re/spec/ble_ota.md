# Steering the stock Oclean X Ultra 20 OTA over BLE + an unprivileged LAN HTTP server

> **Erratum (2026-10-02, checked against the decompilation).** Everywhere below that says the
> brush must be **on the charger**: it is the opposite. The power-state field
> `*(int*)(0x3fca4b6c+8)` is 1 = charging, 2 = **on battery**, 3 = full (see
> `led_battery_charge.md` §6.3, `power.md` §1). The update trigger `0x42011978` is called from the
> main loop's 1 Hz block (`0x4201d3bd`) only when that field is 2, at the moment the idle timer
> expires (30 s after a wake, 60 s after a boot), not after a factory reset, with battery >= 20 %
> (`0x42013a78`: `batt > 0x13 && !brushing`) and the cloud link flag `0x3fca3870 == 1` (set once
> the OTAUpGrade reply was parsed). With `isAppDown:true` it queues the download at once
> (return 1); with `isAppDown:false` and the RTC one-shot set it shows the update prompt
> (screen 97) and a short press there starts the download (`0x42011a00`).
> So: brush **off the charger**, battery at least 20 %, wake it and leave it alone until the
> idle timeout. On the charger the 1 Hz block never reaches this code.

Goal: flash custom firmware onto a **stock** brush without ARP-spoofing / root, using only
(a) the BLE command interface and (b) an ordinary user HTTP server on the LAN.

**Verdict: VIABLE.** The stock firmware builds its OTA-check URL from an NVS string
("http_domain") that is fully writable over BLE with no pairing/bonding. Point it at your
own `http://<ip>:<port>`, reboot over BLE, and the brush POSTs its OTA check to you; your
JSON reply names the firmware URL (also plain http on your box), which the brush downloads
with `esp_https_ota` and flashes. No TLS is forced and the OTA-check reply is **not**
signed. Preconditions: brush on charger, battery > 20 %, not brushing, and Wi‑Fi already
provisioned to your LAN.

Every address is from the stock image. Each fact is tagged CONFIRMED (read from
code/data) or INFERRED.

---

## 0. BLE transport: service / characteristic / framing

### Service & characteristics (CONFIRMED)
UUIDs recovered in the custom `main/ble_server.c` (lines 19‑23, 100‑105) and cross-checked
against the **stock** GATT attribute table at `g_3fc9a220` (dumped below):

| Role | UUID | Stock attr idx | perm |
|---|---|---|---|
| Primary service | `8082caa8-41a6-4021-91c6-56f9b954cc18` | 0 | READ 0x01 |
| **Command WRITE (app channel)** | `9d84b9a3-000c-49d8-9183-855b673fbb85` ("BB85") | 5 | **WRITE 0x10** |
| Notify/Read (app responses) | `5f78df94-798c-46f5-990a-855b673fbb86` ("BB86") | 2 | READ+WRITE 0x11, NOTIFY |
| Command WRITE (brush channel) | `5f78df94-798c-46f5-990a-855b673fbb89` ("BB89") | 10 | **WRITE 0x10** |
| Notify/Read | `5f78df94-798c-46f5-990a-855b673fbb90` ("BB90") | 7 | READ+WRITE 0x11, NOTIFY |

- The GATTS write event handler `FUN_4200e2c8` (event `param_1==2`) dispatches a write to
  the command parser **only when the write targets attr-index-5's handle** (`g_3fca2c24+10`),
  i.e. **BB85**. It calls `FUN_4201125c(resp_buf, write.value, ...)`. CONFIRMED.
- **No auth/bond/session check precedes the dispatch.** The write chars carry
  `perm = 0x10 = ESP_GATT_PERM_WRITE` only — **not** `..._ENCRYPTED (0x20)` — so the BLE
  stack does not require pairing/encryption, and the software handler does no binding check.
  **No pairing/bonding/binding token is needed to send these commands.** CONFIRMED
  (attr table `g_3fc9a220` perms + `FUN_4200e2c8`). See §4.
- Responses are notified back on BB86 as `[cat, 'O'(0x4f), 'K'(0x4b)]`-style frames; you do
  not need to read them for the attack, but subscribing to BB86 lets you confirm the writes
  were accepted. CONFIRMED (`FUN_4200ffe4`, `FUN_42010550` push the ring buffer that is sent
  via `FUN_4202dca4`).

### Command framing (CONFIRMED — dispatcher `FUN_4201125c` @ 0x4201125c)
Every BLE write value is: `byte[0] = category`, `byte[1] = opcode`, `byte[2..] = payload`.
`FUN_4201125c` routes on the category byte:

| category | router fn | table (rodata) | notes |
|---|---|---|---|
| 0x01 | `FUN_42010c64` | — | ack only |
| 0x02 | `FUN_42011118` | `0x3c117c28` (38 entries) | config set (**set-host lives here**) |
| 0x03 | `FUN_42011158` | `0x3c117bb0` (15) | status/session |
| 0x07 | `FUN_42011194` | `0x3c117a40` (46) | diag/control (**reboot lives here**) |
| 0x09 | `FUN_420111d0` | — | `09 ED EF` = factory reset |

Each table entry is `{u32 id, u32 handler}`; the router linearly matches the **opcode byte**
against `id` (low byte) and calls `handler(&value[2], maxlen)` (disasm of `FUN_42011118`:
`loop`, `addx8`, `l32i +4`, `callx8`, arg = value+2). CONFIRMED.

---

## 1. Set the cloud host over BLE  →  `http_domain`

### Command (CONFIRMED)
- Handler `FUN_4200ffe4` = **category 0x02, opcode 0x33** (cat-2 table `0x3c117c28`
  entry [31]: `id=0x33 handler=0x4200ffe4`). CONFIRMED (table dump).
- Full BLE write value to **BB85**:

```
02 33 2A <total_len> <pkt_len> <domain bytes...>          (first/only packet)
02 33    <pkt_len>   <domain bytes...>                    (continuation packets)
```

Framing parsed by `FUN_4200ffe4` (disasm @ 0x4200ffe4) — the handler sees `value[2..]`:
- If `value[2] == 0x2A ('*')`: it `memset(dst,0,0x3c)` (60-byte buffer `g_3fca30e3`),
  resets index `g_3fca311f = 0`, stores `total_len = value[3]` into `g_3fc9a69d`, then copies
  `value[5 + i]` for `i < value[4]` (`pkt_len`) into `dst[idx++]`.
- Else (continuation): first byte `value[2]` is `pkt_len`; copies `value[3 + i]` for
  `i < pkt_len` into `dst[idx++]` (index is **not** reset — you must send the `'*'` packet
  first).
- When `idx == total_len`, calls `FUN_42024b40(dst,0,0x3c)` → NVS. CONFIRMED.

So: `total_len` = length of the domain string in bytes (no NUL), one byte; `pkt_len` = bytes
carried in this packet. For a domain that fits one write, `total_len == pkt_len == len(domain)`.

- **Maximum length: 59 bytes + NUL.** The buffer `g_3fca30e3` is `memset` to `0x3c=60`; the
  NVS store `FUN_42024b40` copies 60 bytes into a 100-byte stack buffer and `nvs_set_blob`s
  100 bytes under namespace `"storage"`, key `"http_domain"`. Remaining bytes stay 0, so the
  string is NUL-terminated as long as `total_len < 60`. CONFIRMED.
- ATT MTU: the frame for `http://192.168.86.39:8080` is 30 bytes (> default 20-byte ATT
  value). Negotiate a larger MTU on connect (bleak/BlueZ do ~517 automatically) **or** split
  across the `'*'` + continuation packets. The flash script does both.

### The URL the host builds — scheme / port / TLS (CONFIRMED)
The OTA-check request is `FUN_42011a60(4, …)` (http_service.c). It:
1. Reads the host: `memset(cStack_f0,0,0x3c); FUN_42024b7c(&cStack_f0,0,0x3c)` (NVS read of
   `http_domain`). `FUN_42024b7c` leaves the buffer untouched on any NVS error, so an absent
   key means the buffer stays all-zero.
2. **Default/fallback:** `if (cStack_f0 != 'h' && cStack_ef != 't') pcVar22 = "https://test.oclean.com";`
   i.e. if the stored string does **not** begin `h…t…` it falls back to the baked-in
   `https://test.oclean.com` (rodata `0x3c11800e`). A value beginning `"http://"` passes the
   check and is used verbatim. CONFIRMED.
3. Builds `url = domain + "/OTA/v1/V1Brush/OTAUpGrade"` (`strcat`×2), then
   `esp_http_client_init(cfg)` with `cfg.url = url`, `cfg.method = 1 (POST)`,
   `cfg.event_handler = FUN_420115e8`, `cfg.transport_type = 2 (OVER_SSL)`, `cfg.user_data =
   &g_3fca346c`; the rest of the struct is `memset` to 0 (**no `cert_pem`, no
   `crt_bundle_attach`, no global CA store**). CONFIRMED (disasm @ 0x42011af0‑0x42011b5c;
   struct offsets 0x00 url, 0x44 method, 0x58 event_handler, 0x5c transport_type, 0x68 user_data).

**TLS is NOT forced and the host is NOT validated.** In esp-idf 5.1.1
`esp_http_client_set_url` parses the URL scheme and overrides `transport_type`: a `http://`
URL sets scheme `"http"`, port 80; at connect `client->transport =
esp_transport_list_get_transport(list, scheme)` picks the plaintext TCP transport
(`esp_http_client.c` lines 49‑60, 994‑996, 1362). An explicit `:port` in the URL is honored
(`UF_PORT`, lines 59‑60). The `transport_type=2` default only applies when the URL has **no**
scheme. (Even the production default is HTTPS *without* cert verification, since no CA is
configured.) CONFIRMED (IDF source).

**⇒ Set `http_domain = "http://<your-ip>:<your-port>"`** (must start with `http://` so (a) the
fallback is skipped and (b) the scheme forces plaintext). Then the OTA check POSTs to
`http://<ip>:<port>/OTA/v1/V1Brush/OTAUpGrade` — a plain unprivileged HTTP server answers it.

> Side effect: `http_domain` is the base host for **all** cloud endpoints the brush uses
> (see §4), e.g. `UploadBrushRecord` (param 0), `UploadingMacWiFi` (param 5),
> `OTAUpReceipt`/`OTAStatistics`. While redirected, those also hit your server (just 200/`{}`
> them). CONFIRMED (`FUN_42011a60` path table).

---

## 2. Reboot over BLE, the RTC "arm" flag, and the other preconditions

### Reboot command (CONFIRMED)
- Handler `FUN_42010550` = **category 0x07, opcode 0x07** (cat-7 table `0x3c117a40`
  entry [6]: `id=0x07 handler=0x42010550`). CONFIRMED.
- BLE write value to BB85: **`07 07 00`**. `FUN_42010550` stores `value[2]` into `g_3fca4d8d`,
  calls `FUN_4201821c()` (saves brushing-time RTC vars) and `FUN_420179b8()` (writes a 3-byte
  NVS blob), then **`if (value[2]==0) esp_restart();`**. So the payload byte must be `0x00`.
  CONFIRMED.

### Is that reboot a "true reset" that arms the one-shot OTA flag? (CONFIRMED mechanism / INFERRED initializer)
- The one-shot flag is **`rtc_50001001`** (RTC_SLOW_MEM @ 0x50001001). It is read only in
  `FUN_42011978` and set to 0 only in `FUN_42011968`. CONFIRMED (whole-corpus grep).
- `esp_restart()` is a **software CPU reset**, not deep-sleep wake. On any reset whose reason
  is not `DEEPSLEEP_RESET`, the 2nd-stage bootloader reloads the `.rtc.data` segment, so a
  `RTC_DATA_ATTR` variable is re-initialised to its load value; on a deep-sleep wake the
  bootloader skips that reload and the value persists. This exactly matches the README's
  "set only by a true reset … a normal power off/on is deep sleep and does not re-arm it."
  Because `rtc_50001001` is only ever *cleared* in the app and is *read* as armed, its load
  value must be non-zero (≈1). **⇒ the BLE reboot (`esp_restart`) re-arms it, identically to
  the reboot long-press / factory reset.** Mechanism CONFIRMED; the exact `.rtc.data`
  initializer value is INFERRED (the `.rtc.data` segment is not in the supplied seg0‑4 dumps).
- The reboot long-press path itself is `FUN_4201cab8` ("BTN_REBOOT_PRESS", "get_ota_start_status",
  "button_long_reset_flag"): on charger it calls `FUN_4201bee8` → `FUN_4200bb5c` → Wi‑Fi start
  + queue the OTA check (`FUN_420118f0`, code 4). CONFIRMED. The BLE reboot reaches the same
  post-boot path.

> **Simpler alternative that side-steps the RTC flag:** reply with `isAppDown:true` (§3).
> With `isAppDown` true, `FUN_42011978` queues the download (code 6) directly from the 1 Hz
> task and the RTC flag is irrelevant. You still want the reboot to force a fresh Wi‑Fi
> connect + OTA poll and to clear the per-boot OTA lock. CONFIRMED (`FUN_42011978`).

### Full precondition list for the OTA check + download (CONFIRMED unless noted)
The 1 Hz task `FUN_4201d3bd` (pxp_reporter_task) runs the OTA gate each second. Reaching the
download requires ALL of:
1. **On charger / dock:** `*(int*)(g_3fca4b6c+8) == 2` (the power-state field). The gate block
   is inside `if ((g_3fca4b6c+8)==2)`. Also `FUN_4201cab8`/`FUN_4201bee8` only start Wi‑Fi+OTA
   on the charger path. CONFIRMED.
2. **Battery > 19 %:** gate `FUN_42013a78` does `if (0x13 < FUN_42018244())` where
   `FUN_42018244` returns `g_3fca4b6c[0xe]` (battery %). I.e. **battery ≥ 20**. CONFIRMED.
   (Matches README "> 20 %".)
3. **Not busy / not brushing:** `FUN_42013a78` returns `!FUN_420197dc()`, and `FUN_420197dc`
   returns `g_3fca4d5b` (the brushing-active flag). Must be 0. CONFIRMED.
4. **Per-boot OTA lock clear:** `FUN_42013a50` (`get_ota_start_status`) returns `g_3fca4194`,
   set to 1 by `FUN_42013a64(1)` once an attempt starts; must be 0 to begin. It is in .bss
   (zero at boot) → one attempt per boot. CONFIRMED.
5. **`button_long_reset_flag` clear:** `g_3fca4ea0 == 0` in `FUN_4201d3bd`. CONFIRMED.
6. **Wi‑Fi connected to your LAN:** the OTA check (code 4) is queued on Wi‑Fi bring-up
   (`FUN_4200bb5c`). The brush must already have saved Wi‑Fi credentials for your network
   (it is the owner's brush on the owner's LAN). CONFIRMED (code path) / provisioning assumed.
7. **A rate/scheduling gate** `FUN_420142d0` (time-since-last + state) must allow it; a reboot
   resets its counters. CONFIRMED (exists) / exact timing INFERRED.
8. **OTA check must have completed** with a non-empty `otaFilePath`: `g_3fca3870 == 1` and
   `g_3fca3ac8[0] != 0` (set by the §3 reply). CONFIRMED.
9. BLE connection present/absent is **not** a gate for these commands or for the OTA poll.
   INFERRED (no such check in `FUN_4201125c`, `FUN_4201d3bd`, `FUN_42011978`).

---

## 3. The request the brush makes and the reply it needs

### Request (CONFIRMED)
`POST http://<your-host>/OTA/v1/V1Brush/OTAUpGrade`, `Content-Type: application/json`, body
(JSON, built by `FUN_42011a60` param 4):
```json
{"mac":"aa:bb:cc:dd:ee:ff","model":"OCLEANV20","oldlan":<n>,"newlan":<n>,
 "ota":"0.0.1.6","voice":"0.0.1.6","img":"0.0.1.6"}
```
(The advertised versions are never compared by the device — README + no compare in code.)

### Minimal reply to start a device-side download (CONFIRMED fields)
`FUN_42011a60` param-4 response parser requires, in order:
- top-level `"state"` must be a JSON **boolean `true`**
  (`FUN_42103684` = cJSON_IsBool(state) must be true, then `FUN_4210365c` = cJSON_IsFalse(state)
  must be false). CONFIRMED (disasm of the two helpers: they test `node->type` bits 1/2).
- `"data"` object present; inside it:
  - `"isAppDown"` boolean → `g_3fca386c` (`FUN_42103670` = cJSON_IsTrue). CONFIRMED.
  - `"otaFilePath"` string → `strcpy g_3fca3ac8` (the app-image URL). CONFIRMED.
  - `"newOTAVersion"` string → `strcpy g_3fca3b90`. CONFIRMED.

**There is NO signature / HMAC / integrity check on this reply** — the only validators are
the cJSON type checks above and the accumulating event handler `FUN_420115e8` (which merely
buffers up to 1024 bytes). CONFIRMED. (The MITM addon's note that a rewritten reply "did not
trigger a download, which points to an integrity-checked reply" is **not** borne out by the
code; the likely real cause was `isAppDown:false` *without* the RTC flag armed, which
`FUN_42011978` turns into a no-op — see below.)

Recommended reply (device downloads itself, no RTC-flag dependency):
```json
{"state":true,"code":0,"msg":"Success",
 "data":{"isAppDown":true,
         "otaFilePath":"http://<your-ip>:<port>/ota.bin",
         "newOTAVersion":"1.3.3.7"}}
```
- `isAppDown:true` → `FUN_42011978` (gate ok) queues **code 6**; the http task's
  `uVar8==6` branch calls `FUN_42013a94(g_3fca3ac8, 0)` = `trig_app_ota(otaFilePath)`.
  CONFIRMED.
- `otaFilePath` is used **verbatim** as the OTA URL — point it at your own box with
  `http://` so `esp_https_ota`/`esp_http_client` uses plaintext (same scheme logic as §1).
  It need not share the host with `http_domain`. CONFIRMED.
- (`isAppDown:false` is the README/MITM choice; it needs the RTC flag armed and goes through
  the `FUN_4201a1c0` UI path — avoid it for the pure-BLE route.)

### Download behaviour & image validation (CONFIRMED)
- App image path `FUN_42013a94` → `esp_https_ota(&cfg)` with `cfg.http.url = otaFilePath`,
  `transport_type=2` (overridden to http by the URL scheme), buffer `0x8000`. It streams the
  body and `esp_ota_write`s it. **Range (HTTP 206) requests are used / must be supported** —
  the README observed `SERVE ota.bin [0-4095/<size>]` (4096-byte chunks); serve 206 with
  `Content-Range` (and `Accept-Ranges: bytes`, `Content-Length`). A plain 200 with the whole
  body also works as a fallback. CONFIRMED (README observation + `esp_https_ota`).
- The resource path `FUN_42013b40` (voice/img/vic — not needed for the app flash) uses
  `esp_http_client_open` + `esp_http_client_fetch_headers` (reads `Content-Length`) + 512-byte
  `esp_http_client_read` chunks, writing to a partition with per-chunk read-back verify
  (`FUN_420138d4`, `memcmp`). CONFIRMED.
- **Validation before boot:** `esp_https_ota` → `esp_ota_end`/`esp_image_verify` checks the
  ESP32-S3 app-image format (magic `0xE9`, chip id, segment checksums, appended SHA-256). No
  version/anti-rollback check (rollback is off per README) and no secure-boot signature
  (custom images flash successfully). So the served file must be a valid unmodified
  `build/oclean_custom.bin` that fits the stock OTA partition. CONFIRMED (image format) /
  no-signature INFERRED from README success.

---

## 4. Pairing / bonding / binding, and side effects of writing `http_domain`

- **No pairing/bonding required.** Write chars BB85/BB89 are `ESP_GATT_PERM_WRITE (0x10)`,
  not encrypted (attr table `g_3fc9a220`), and the write handler `FUN_4200e2c8` dispatches
  with no auth/session/binding gate. The command handlers `FUN_4200ffe4` (set-host) and
  `FUN_42010550` (reboot) themselves check no binding token. CONFIRMED.
  - Caveat: the device also exposes BluFi (from `ulp_riscv_blufi_example`) for Wi‑Fi
    provisioning; that is a *separate* service with its own security and is **not** on the path
    for these commands. INFERRED.
- **`http_domain` side effects.** It is the base host for every cloud call in `FUN_42011a60`:
  `UploadBrushRecord` (param 0, path `/OTA/v4/V1Brush/UploadBrushRecord`), `OTAUpGrade`
  (4, `/OTA/v1/V1Brush/OTAUpGrade`), `UploadingMacWiFi` (5), `OTAUpReceipt` (10),
  `OTAStatistics` (0xE), `GetOTACounterMode` (0x15), `WeatherKit` (0xF), etc. A few endpoints
  use *hard-coded* hosts regardless of `http_domain`: `mall.oclean.com` (param 0x10) and
  `beta-core.oclean.com` (0x11,0x12), and the demo images use `xjjota.oclean.com`. CONFIRMED.
  - So while redirected, the brush will also try to POST **brushing records** and telemetry to
    your server. Answer them `200 {}` (or `{"data":"ok"}` for UploadBrushRecord, which the
    code checks for `data=="ok"`). No harm; just be aware the brush is talking only to you.
- The NVS key `"http_domain"` is **only** written by `FUN_42024b40` (the BLE set-host path)
  and **only** read by `FUN_42024b7c`. No other code sets it. CONFIRMED.

---

## 5. Restoring the original host afterwards

- The production host the brush normally uses (per the MITM capture notes: `hwapicore.oclean.com`,
  contacted every heartbeat) is **not** a string in the firmware image — it lives only in the
  `http_domain` NVS value set at the factory/app. The only baked-in host is the fallback
  `https://test.oclean.com` (rodata `0x3c11800e`). CONFIRMED (string search + `FUN_42011a60`).
- Consequences:
  - **Best restore:** write the original value back with the same BLE command, e.g.
    `02 33 2A <len> <len> "https://hwapicore.oclean.com"` (≤ 59 bytes — fits). Capture the
    exact original string from your MITM logs *before* overwriting, since the image does not
    contain it. (`https://hwapicore.oclean.com` = 28 bytes.)
  - **Factory reset (`09 ED EF`, `FUN_420111d0`→`FUN_4201fd54`)** is *not* a safe restore: if
    it clears the `"storage"` NVS namespace, `http_domain` becomes absent and the brush falls
    back to `https://test.oclean.com`, **not** the production host. Needs on-device
    confirmation of what it erases. INFERRED.
- There is no observed BLE "get http_domain" opcode in the cat-2 table to read the current
  value back (not found). Capture it from the network instead. INFERRED (absence).

---

## 6. End-to-end procedure (pure BLE + unprivileged HTTP)

1. Brush already joined to your LAN; place it **on the charger**, battery **> 20 %**, idle.
2. Start `ota_http_server.py` on `<ip>:<port>` (e.g. `8080`) serving `oclean_custom_ota.bin`.
3. BLE connect (no pairing). Write to **BB85** (`9d84…bb85`):
   - set host: `02 33 2A 19 19` + `"http://<ip>:8080"` (`total=pkt=len(domain)`), MTU-split if needed.
   - reboot:   `07 07 00`.
4. Brush reboots, rejoins Wi‑Fi, POSTs `…/OTA/v1/V1Brush/OTAUpGrade` to your server; server
   replies with `isAppDown:true` + `otaFilePath=http://<ip>:8080/ota.bin`.
5. Brush GETs `ota.bin` (ranged), `esp_https_ota` flashes it, reboots into the custom image.

---

## 7. What could still block it (honest risks)

- **Wi‑Fi provisioning.** The route assumes the brush already has your SSID saved. If not, you
  must first provision Wi‑Fi over **BluFi** (separate security handshake, not analysed here).
  This is the single biggest practical dependency.
- **`.rtc.data` initializer unverified.** The "true-reset arms the flag" conclusion is sound
  from the reset-reason semantics, but the exact armed value of `rtc_50001001` is not in the
  dumped segments. Using `isAppDown:true` removes this dependency entirely.
- **The OTA poll must fire post-reboot.** It depends on charger detection (`g_3fca4b6c+8==2`)
  and the `FUN_420142d0` scheduler; worst case it polls on its normal cadence rather than
  immediately. Keep it on the charger and wait.
- **`otaFilePath` length / parsing.** `g_3fca3ac8` is `strcpy`'d from the reply with no length
  guard shown; keep the URL short and well-formed.
- **Restore.** You must know the original `http_domain` string to put it back cleanly (§5).

---

## 8. Open questions
1. Exact `.rtc.data` initializer of `rtc_50001001` (armed value) — not in seg0‑4.
2. Whether `isAppDown:false`'s `FUN_4201a1c0`/UI-code-`0x61` path auto-downloads once the RTC
   flag is armed, or needs a button confirmation. (Avoided by `isAppDown:true`.)
3. Exact timing of the `FUN_420142d0` OTA scheduler (how soon after boot the poll fires).
4. Whether factory reset (`09 ED EF`) erases the `"storage"` NVS namespace (and thus wipes
   `http_domain`).
5. Precise meaning of the `state`/`data` success shape the production cloud returns vs. the
   minimal `{"state":true,...}` the parser accepts (parser is permissive; production may be
   richer).
6. Whether BB89 ("brush channel") accepts the same cat/opcodes as BB85 in stock (custom FW
   treats them equivalently; stock dispatch was confirmed only for BB85/idx-5).
