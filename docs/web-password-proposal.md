# Web interface password protection — proposal

2026-10-04 · implemented as proposed, plus a passcode for the setup AP; on 2026-10-05 the password became required before the brush joins a Wi-Fi network, and nvs copies are allowed with it (see Implementation notes at the end). The Current state section describes the firmware before this change.

## Summary

Add one optional password to the web UI, checked in one place: `reg()` in `main/web_server.c` wraps every handler in a `guard()`. The login form, curl and `dump_res.py` send the password once. A correct password returns a cookie holding a random token kept in NVS, so a login survives deep sleep, reboots and OTA.

Two rules apply even before a password is set. `Host` must be an IP address, and every POST must be JSON or octet-stream. Together they close DNS rebinding and CSRF. Holding the button for 8 s clears the password.

**Cost (estimate):** about 220 lines of C and JS and 4–6 KB of flash. The crypto code is already in the image.

The setup AP `oclean-setup` is WPA3, with a new 9-digit passcode each time it comes up, shown full-screen on the brush.

**Not included:** HTTPS, OTA signing, user accounts, a login throttle.

## Current state

Anyone who can reach port 80 can do anything the owner can, including flashing their own firmware. There is no login, and everything travels as plain HTTP. All handlers are registered in `web_server_start()` in `main/web_server.c`. This covers the working tree as of this date, including the uncommitted Files tab (`/api/parts`, `/api/res?part=`).

**Who can reach it**

- **Home network:** every device on the LAN, at the brush's DHCP address.
- **Setup AP:** `oclean-setup` is an **open** network (`WIFI_AUTH_OPEN` in `wifi_mgr.c`) at 192.168.4.1. It comes up when no Wi-Fi is saved, and also after 5 failed connection attempts in a row (about a minute, e.g. a router reboot). Anyone in radio range can then join and use the UI.
- **Safe mode:** after 4 failed boots the web server still starts, without the brush logic, so OTA can repair the device.
- **Web pages the owner visits:** a browser on the LAN can be made to send requests to the brush. These are cross-site requests (CSRF), or DNS-rebinding pages that can also read the replies, because the server never checks the `Host` header.

**What each endpoint lets a stranger do**

| Endpoint | What it does | Harm without a password |
| --- | --- | --- |
| `POST /api/ota` | Writes any ESP32-S3 app image to the idle slot and boots it. There is no signature check. | Permanent takeover. Their firmware can then read the Wi-Fi password from NVS. |
| `POST /api/config` | Sets Wi-Fi, MQTT, time zone and panel | Moves the brush to another network or broker. A blank password field means "unchanged", so pointing `mqtt_host` at their own machine makes the brush send the stored MQTT username and password to it in clear. |
| `GET /api/res`, `GET /api/parts` | Raw flash reads of every region except NVS: bootloader, partition table, both app slots, the OEM partitions | Copies firmware and OEM data. Only the NVS subtypes are withheld (`touches_secrets()`). |
| `POST /api/brush` | Start/stop, mode, strength | Runs the motor remotely |
| `POST /api/reboot` | Restarts | Nuisance, repeatable |
| `GET /api/log` | Log tail. `?level=` changes the log level, so this GET changes state. | Leaks SSID, IP addresses and MQTT host from the log |
| `GET /api/config` | Settings, with passwords shown only as `mqtt_pass_set` | Leaks SSID, broker host, port and user |
| `GET /api/status` | Metrics and diagnostics | Leaks brushing habits and battery state |
| `GET /`, `/app.js`, `/style.css` | The UI itself | None (static files) |

**Existing partial protections**

- `/api/ota` accepts only `Content-Type: application/octet-stream`. A browser sends that cross-site only after a CORS preflight, which the server never grants, so ordinary CSRF cannot flash. This does not help against a LAN client or a DNS-rebinding page.
- `/api/config` and `/api/brush` parse the body as JSON whatever its content type. A cross-site `text/plain` POST is a simple request that needs no preflight, so any page the owner opens can change the settings or start the motor today.
- The 8 s button factory reset (`oem_factory_reset()`) resets only stock's settings. It does not clear the `oclean` NVS namespace that holds Wi-Fi and MQTT, so the device has no physical reset path for our settings today.

## Design

Once a password is set, every `/api/*` request needs the cookie or the password. `/`, `/app.js` and `/style.css` stay open so the login view can load.

**One guard.** `reg()` registers `guard()` for every route and passes the real handler in `user_ctx`. `guard()` checks `Host`, then `Content-Type` on POSTs, then auth. A refused request never reaches its handler, so it never counts as activity.

**Auth**

- A cookie `oclean=<token>` that matches the stored token passes without hashing (constant-time compare).
- Otherwise the guard hashes the password from `Authorization: Basic` and compares all 32 bytes in constant time. The user name is ignored; clients send `oclean`. A match also sets the cookie.
- Anything else gets 401 without `WWW-Authenticate`. The browser shows no dialog of its own, and `app.js` shows its login view.

**Storage.** One 68-byte NVS blob, `web_auth` in namespace `oclean`. It holds the iteration count, a 16-byte salt, the 32-byte PBKDF2-HMAC-SHA256 hash and a 16-byte token.

- 10,000 iterations take an estimated 0.4–1.0 s on this chip.
- The 600,000 that [OWASP](https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html) recommends would take about a minute.
- Salt and token come from `esp_fill_random()` while Wi-Fi is up.
- No record means no password.

**Session.** The cookie is `oclean=<token>; Path=/; HttpOnly; SameSite=Strict; Max-Age=31536000`. It has no `Secure` flag, because browsers drop a `Secure` cookie set over plain HTTP ([rfc6265bis](https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-22.txt)). The token lives in NVS, so it survives every wake, reboot and OTA. A new password makes a new token, which logs out every other browser.

**Two always-on rules** (also while no password is set):

- `Host` must be absent or an IP address with an optional `:port`, otherwise 403. A DNS-rebinding page always sends its own host name ([NCC Group](https://github.com/nccgroup/singularity/wiki/Preventing-DNS-Rebinding-Attacks)).
- A POST's `Content-Type`, up to the first `;`, must be exactly `application/json` or `application/octet-stream`, otherwise 400. Another site's page can send these only after a CORS preflight ([Fetch](https://fetch.spec.whatwg.org/#cors-safelisted-request-header)). That preflight fails, because the server has no OPTIONS handler.
- `GET /api/log?level=` becomes `POST /api/log`.

**No throttle.** Each guess costs one hash on the single server task, about 1–2 guesses a second (estimate). A lockout would let any LAN client lock the owner out.

**Why not plain Basic auth or HTTPS.** Basic auth would hash the password on every 1.5–5 s poll and has no logout. HTTPS costs tens of KB of heap per connection and shows certificate warnings.

## Setup, password change and recovery

As built, the password is required before the brush joins a Wi-Fi network (see Implementation notes; this section describes the original proposal, in which it was optional). The two always-on rules and the setup AP's WPA3 passcode (phones from Android 10 / iOS 13 on; in safe mode the AP stays open) apply in any case.

**Trade-off.** Until a password is set, anyone on the LAN or the setup AP can flash an image or set the password first. A required password has the same first-come window, and it also breaks existing scripts. (As built, a brush without a password is on no network but its setup AP, whose passcode only the screen shows, so the first-come window needs the brush in hand.)

**Set or change it** in Settings, with a new `web_pass` field sent in the existing `POST /api/config`. Blank means unchanged. It takes 12–64 printable ASCII characters. Over the setup AP it is protected by the AP's WPA3 passcode, which only the brush's screen shows (in safe mode the AP is open, so it travels in clear).

**Forgotten password:** hold the button for 8 s. Only `web_auth` is erased; the Wi-Fi and MQTT settings stay.

- **Normal mode:** the hold may start while the brush is asleep (see Implementation notes). The existing factory-reset hold (`oem_factory_reset()` in `main/oem_app.c`) also calls a new `hal_forget_web_password()`. Stock's side effects stay: brushing history resets. On battery the brush also goes into shipping sleep until you press the button or dock it.
- **Safe mode:** no button driver runs there, so the idle loop in `app_main()` polls GPIO3 itself. After 8 s of continuous press it clears the password. The screen stays dark.

In every other respect safe mode asks for the same password. A browser that logged in earlier keeps its cookie.

## Changes for clients

Each client sends the password once or keeps the cookie. Home Assistant talks MQTT, so it is unaffected.

- **`main/www/app.js`** (about 45 lines):
  - One `api()` wrapper and the OTA upload switch to a login view on 401 and stop the status and log polls.
  - The login form sends the password as Basic to `GET /api/config`, then reloads.
  - Reboot and the log level become JSON POSTs. After an OTA, a 401 also counts as "back".
  - Save stays disabled until `/api/config` has loaded, since saving the blank form would write an empty Wi-Fi name.
  - `web_pass` works like the other password fields: blank means unchanged, and it is cleared after Save.
- **`main/www/index.html`** (about 15 lines): the login view, the `web_pass` field, and the hint "Forgot it? Hold the button 8 s."
- **curl OTA** in `README.md` and `.github/workflows/firmware.yml`: add `-u oclean`, and curl asks for the password ([everything curl](https://everything.curl.dev/http/auth.html)). A brush without a password ignores the header.
- **`re/tools/uisim/dump_res.py`** (about 5 lines): a cookie jar, plus a Basic header built from `OCLEAN_PASS`.
- **Docs** (`README.md`, `flash/README.md`):
  - The brush joins its network only once a password is set; once set, OTA and going back to stock need it.
  - Open the brush by its IP address.
  - The 8 s hold resets the password.
  - Stock firmware leaves `web_auth` in NVS, so a later reinstall still knows the old password.

## Implementation plan

About a day of work. Nothing is built or flashed until you agree.

1. **`main/web_server.c`** (about 140 lines):
   - `guard()` and its helpers: `host_ok()`, `ct_ok()`, `cookie_ok()`, `basic_ok()`, `pw_hash()`.
   - `auth_load()`, `auth_set()` and `web_auth_forget()`.
   - `reg()` wraps every handler, and `POST /api/log` is registered.
   - `h_config_post()` reads `web_pass`; `h_config_get()` reports `web_pass_set`.
2. **`main/CMakeLists.txt`:** add `mbedtls` to `REQUIRES`.
3. **Recovery** (about 20 lines):
   - `hal_forget_web_password()` in `main/oem_hal.h` and `main/oem_glue.c`, called from `oem_factory_reset()`.
   - A stub in `re/tools/uisim/sim_app.c`.
   - The safe-mode GPIO3 poll in `main/app_main.c`.
4. **UI, `dump_res.py` and docs** (about 65 lines).
5. **Host test** of `host_ok()`, `ct_ok()` and the cookie and Basic parsers, built with gcc like the `re/tools/uisim` sims. It feeds in the exact headers Chrome, Firefox, curl and urllib send, plus `text/plain; x=application/json`.
6. **QEMU:** inject the 8 s hold with `re/tools/uisim/qemu_inject.c` and check that the password is cleared. QEMU starts no radios, so this is the only part it can test.
7. **On the brush, only with your go-ahead:**
   1. Re-flash a known-good image with `curl -u` first. This proves OTA still gets through the guard.
   2. Time the hash.
   3. Check login, a wake from deep sleep, `dump_res.py` and the 8 s hold.

## Risks and open questions

The main remaining risk is sniffing. Over plain HTTP the password is visible at login, and the cookie on every request.

- **A guard that refuses everyone could not be undone.** The image does not crash, so the crash-loop guard never reverts it. The stock bootloader has no rollback, and the brush has no serial port. Plan steps 5 and 7.1 exist for this.
- **Setup AP:** jamming the home Wi-Fi for about a minute brings up `oclean-setup`, but joining it needs the passcode on the brush's screen. WPA3 (SAE) leaves a recorded join nothing to crack offline; with WPA2, 9 digits would fall to minutes of GPU time. In safe mode, which has no screen, the AP is still open.
- **Flash dump:** the token and the hash sit unencrypted in NVS, and the token logs in directly.
- **Debug log level:** the server's own two log tags (`httpd_parse`, `httpd_txrx`) stay at info whatever level is chosen, because at debug they would record `Authorization`, `Cookie` and `Set-Cookie`.
- **Anyone with the password** can still flash unsigned images and redirect the MQTT credentials (see Current state).
- **SAE load:** while the setup AP is up, anyone in range can send WPA3 join attempts without the passcode. IDF handles each in a priority-19 task, several elliptic-curve multiplications in software, so a deliberate flood can stall the brush logic and screen meanwhile. Accepted: the AP is up only when the home network is unreachable, and jamming disrupts the brush's Wi-Fi anyway.

**Open questions**

- [x] Optional password (proposed), or required on first visit? Implemented: optional at first; since 2026-10-05 required before the brush joins a Wi-Fi network (see Implementation notes).
- [x] Minimum length 12 (proposed), or 15 as [NIST SP 800-63B-4](https://pages.nist.gov/800-63-4/sp800-63b.html) asks for a password used alone? Implemented: 12.
- [x] Is it fine that recovery also runs stock's factory reset, which clears brushing history? Implemented as proposed; the UI says so.
- [x] Refuse host names in `Host` (proposed), or also allow one name you configure? Implemented: IP addresses only.

## Implementation notes

Built on 2026-10-04 as above, with these differences:

- **Setup AP passcode (added at the owner's request).** `start_softap()` in `main/wifi_mgr.c` makes the AP WPA3 (SAE, so phones from Android 10 / iOS 13 on) with 9 random digits, new each time it comes up and never logged. The Wi-Fi driver keeps its settings in RAM, and the AP config it loads at boot is replaced before the radio starts, so an old open AP is never beaconed. `oem_ui.c` shows them in place of every screen except brushing and update screens, as three rows of three large 7-segment digits on black in the middle of the screen; they are plain rectangles, so they show even without the OEM pictures. (The first version drew two rows of five small digits over the bottom of the current screen; on the brush the panel's rounded corners cut off the outer digits and the menu showed above them, so it was changed to this.) On battery the screen is woken when the AP comes up; on the dock the backlight goes dark 30 s after docking and nothing is drawn behind it, so a button press shows the code. Safe mode keeps the AP open, because nothing can be drawn there.
- **Header parsers in their own file.** `main/web_auth.c` holds the `Host`, `Content-Type`, cookie and Basic parsers without ESP-IDF, so `re/tools/uisim/sim_auth.c` tests them on the host. Storage, hashing and `guard()` are in `main/web_server.c`.
- **Hash speed.** The hash runs with a `CPU_FREQ_MAX` PM lock, since the APB lock alone leaves the CPU at 80 MHz. The time is logged (`password hash (10000 iterations): N ms`), which covers plan step 7.2.
- **Hold from sleep (found on the brush).** Stock never sees the press that wakes the brush, so a hold started from sleep did nothing, and the owner's first two tries failed. `oem_button_init()` in `main/hw_button.c` now arms only the 8 s timer when the button is already down at a wake from deep sleep or a power-on, counted from the boot; the timer acts only if the button is still down then. Not after a software restart, so a hold through the factory reset's own restart does not reset again, and not under emulation, where QEMU reads the pin low.
- **Password before Wi-Fi (owner's request, 2026-10-05).** Without a web password the brush joins no network: `wifi_mgr_start()` holds the stored one back and brings up the setup AP, also after an update from an older build and after the 8 s hold. Setting the password there makes it join the stored network. `h_config_post()` also refuses a new or changed network (400) while no password is set, unless the same save sets one, and refuses a first password in safe mode. Safe mode, and any boot whose brush logic does not run, has no screen for the passcode: without a password it also stays off the network, and its setup AP is open, so a repair without the password needs someone in radio range rather than anyone on the home network. So a first password always comes in over the passcode-protected AP. (A second review found the first version of this, in which a password-less safe mode joined the home network, left OTA open to that whole network.) The page says so under the Wi-Fi fields, checks the same rule before saving, and opens Settings while no password is set. A first version only refused new networks and let a brush already on its network keep it; review showed that anyone on that network could then set the first password and copy nvs, Wi-Fi password included.
- **nvs copies (owner's request, 2026-10-05).** The Files tab copies nvs only with the web password; without one it stays withheld. Since the first password needs the brush in hand (above), that is the owner. The copy holds the Wi-Fi and MQTT passwords in clear and the web password's hash and login token (it can log in until the password changes): keep it private.
- **Safe-mode reset.** `app_main()` polls GPIO3 every 100 ms in safe mode and clears the password after 8 s of continuous press, then restarts without `boot_guard_clean_exit()`, so the restart counts as one more failed boot: the brush comes back in safe mode, now off the network as the open setup AP. If that restart is the 8th failed boot in a row, the crash-loop guard boots the other OTA slot instead, in normal mode (right after a first install, that slot holds stock).
- **Cookie bound to the address.** The cookie is HMAC-SHA256(token, the brush's own address on that connection), so a login at the setup AP's 192.168.4.1, an address many other gadgets use, is no key to the brush's LAN address.
- **Page loads.** A page load counts as activity through the one `GET /api/config` the page sends when it opens, which needs the login once a password is set; `GET /` itself no longer counts, so a stranger cannot keep a locked brush awake.
- **Going back to stock.** The Wi-Fi driver keeps its settings in RAM only after the boot-time network is set, which it still saves to flash, so stock joins the network the brush last booted with.

Tested: `sim_auth` (also under ASan/UBSan), `sim_ui` with the `l_apcode` group (passcode screen), `sim_app`, `sim_glue`, `sim_input_hw` (held-from-wake case), a firmware build without warnings (1,197,344 bytes), and QEMU with a scripted 8 s hold, which logs `web password cleared` and restarts. On the brush (2026-10-05): OTA through the guard with `curl -u`; the Host, Content-Type and OPTIONS refusals; the 8 s hold from sleep (factory reset); a brush without a password leaving the LAN for the WPA3 `oclean-setup` AP; the owner setting the password over it from a phone, after which the brush rejoined the LAN and the API answers 401 without it. Not yet on the brush: the safe-mode paths.
