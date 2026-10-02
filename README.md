# Oclean X Ultra 20 — custom firmware (ESP32-S3)

Replacement firmware for the Oclean X Ultra 20 that re-implements the stock (OEM)
behaviour — the picture-based screen with its pages, touch swipes, brushing modes,
intensity, LEDs, charging display and automatic sleep / wake — and adds Wi-Fi, a web UI,
MQTT / Home Assistant and a BLE service. The OEM behaviour was recovered by decompiling
the stock image; the specs are in `re/spec/`.

## Status

The OEM port builds and runs in QEMU (`re/spec/NOTES_glue.md` has the evidence: boot, wake
page, mode pages, scripted sessions with pause / resume, intensity, lock popup, swipes,
screen-off stage, wake, factory reset). Every core module has a host test under
`re/tools/uisim/`. **It has not run on the brush yet**: nothing that needs the real
peripherals or the radios (panel, touch strip, force sensor, IMU, LEDs, motor, charging,
deep sleep and wake, Wi-Fi / BLE / MQTT around sleep, safe mode) has been exercised.

Before and during the first boot read the "watch on first boot" sections of every
`re/spec/NOTES_*.md`; the web UI's Logs tab and the diagnostics on its Brush tab show what
they refer to. Things to expect that are stock behaviour, not faults: the brush turns its
screen off about 36 s after a wake and deep-sleeps 30 s later (wake it with the button, a
pick-up or the charger; loading the web page restarts the window); on the charger it never
sleeps and only the backlight times out; the clock page is empty without weather data.
Set the time zone once in Settings.

## Features
- **OEM parity** — stock screens (wake page, mode pages, brushing countdown, intensity,
  pause, score / history, charging, low battery, info, update, lock popup) composed from
  the brush's own picture partition; touch swipes (mode up/down, side pages left/right);
  button (short press start / pause, 2 s lock, 5 s info, 8 s factory reset); six brushing
  modes with the stock motor waveforms, zone cue and auto-stop; anti-splash and
  over-pressure handling from the force sensor; the four LEDs and backlight patterns;
  battery gauge, charging state and thermal cutoff; idle screen-off, deep sleep and wake on
  button / pick-up / charger. Not ported: voice clips (MP3), the IMU zone tracker (the
  score is time-based until zone data exists), OEM cloud, factory / shop-demo modes.
- **Web UI** (port 80) — dashboard, Brush tab (start / stop, mode, intensity,
  diagnostics), MQTT / Wi-Fi / panel settings, firmware update (shows the OEM update
  screens), live log, read-only file browser, picture-partition dump (`/api/res`).
- **MQTT + Home Assistant** — auto-discovery of every metric; Brushing switch, Mode and
  Intensity numbers.
- **Wi-Fi** — joins the configured network with backoff; open `oclean-setup` AP
  (http://192.168.4.1) when there are no credentials or after a minute of failures.
- **BLE GATT server** — Oclean service `8082caa8…` for the phone app (best effort).
- **Safety** — crash-loop guard (safe mode with web UI after 4 failed boots, revert to
  the other OTA slot after 8), stock NVS never erased wholesale, deep sleep refused on the
  charger or when the button wake cannot be armed.

## Hardware (from the stock firmware; `re/HARDWARE_MAP.md` has the older, partly wrong map)
| Function | Pins | Notes |
|---|---|---|
| Display | ST7735S-class 80×160 on SPI2: MOSI40 SCLK39 CS38 DC41 RST42 | init table chosen by the stock panel id in NVS; backlight = LEDC ch4 / GPIO21 active-low; LCD power switch GPIO37 |
| Touch strip | Azoteq IQS7222D, addr 0x44 on bit-bang I2C SCL13 / SDA14, RDY GPIO12 | four swipes, no tap |
| Force sensor | AW8686X, addr 0x6A, same bus | calibration from NVS `aw8686x_config` |
| IMU | QMI8658 on SPI3: MISO4 MOSI5 SCLK6 CS7, INT1 → GPIO8 | any-motion wake, temperature |
| Button | GPIO3, active-low | |
| Charger present | **GPIO9, active-low** | charge block GPIO26 (high = blocked, input = allowed); GPIO45 always 0 |
| Battery | ADC1_CH0 = GPIO1, ×2 divider | |
| LEDs | LEDC ch0..3 on GPIO17..20 | ch0/1 inverted outputs |
| Motor | voice coil on I2S0 BCK33 WS47 DOUT34, 24 kHz, right slot; amp enable GPIO48 | |

## Build
```
. $IDF_PATH/export.sh        # ESP-IDF v5.1.1
idf.py set-target esp32s3
idf.py build                 # -> build/oclean_custom.bin
```
Host tests of the core modules: the build lines are in the headers of
`re/tools/uisim/sim_*.c`; `re/tools/esp_syntax.sh main/<file>.c` checks ESP-side files
with the cross compiler. `re/tools/uisim/mkqemu.py` builds a flash image for
`qemu-system-xtensa -machine esp32s3` (with a stand-in picture partition from
`mkres.py`); the firmware detects QEMU and skips the peripherals it cannot emulate.

## Flashing
**Never flash `partitions.csv` or a merged full image over UART**: the OEM pictures live
only in a flash partition (type 0x40) that this table would overwrite, and the stock
partition table is what the firmware expects. Use app-only updates:

- **Custom → custom:** web UI Firmware tab, or
  `curl -H 'Content-Type: application/octet-stream' --data-binary @build/oclean_custom.bin http://<ip>/api/ota`
  (refused below 20 % battery and while brushing, as stock).
- **Stock → custom without root:** `re/spec/ble_ota.md` — set the stock firmware's cloud
  host over BLE (`re/spec/ble_ota_flash.py`), reboot it, and answer its update check from
  `re/spec/ota_http_server.py` on the LAN (needs the brush already provisioned to your
  Wi-Fi, **off the charger**, battery at least 20 %: the stock firmware starts the download
  when its idle timer expires on battery). Not yet tried on a device. The older ARP-spoofing MITM route is in the git
  history of this file.
- **Back to stock:** flash the genuine `ota.bin` through `/api/ota`.

Back up the pictures once the custom firmware runs:
`python3 re/tools/uisim/dump_res.py http://<ip> res_dump.bin` (then `re/tools/ui_extract.py`
renders every picture to PNG and the UI simulator can use the real art).

## Reverse-engineering tooling
`re/tools/decompile.sh ota.bin` turns the stock image into readable C under `re/work/`
(Ghidra headless + library function naming by matching a reference ESP-IDF build);
`re/tools/fd.py` prints annotated disassembly; `re/tools/extract_ui_tables.py` regenerates
`main/stock_ui_tables.h`. The behaviour specs derived from it are `re/spec/*.md`.
