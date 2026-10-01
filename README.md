# Oclean X Ultra 20 — custom firmware (ESP32-S3)

Full replacement firmware for the Oclean X Ultra 20, reverse-engineered from the
stock `ota.bin` (see `re/HARDWARE_MAP.md`). It keeps the brush functional **and**
adds Wi-Fi, a web UI, and MQTT / Home Assistant.

## Features
- **Toothbrush function** — motor (I2S voice-coil), buttons (short press = start/stop,
  long press = cycle intensity), indicator LEDs, charge control with the stock
  72 °C/67 °C thermal cutoff, and the SPI LCD showing a battery/brushing/gear/status screen.
- **Sensors → metrics** — battery (ADC1_CH0 ×2), brush temperature (QMI8658 IMU),
  pressure (AW8686X), brushing/gear state.
- **MQTT + Home Assistant** — auto-discovery of every metric as HA entities, plus
  controllable entities: Brushing (switch), Cleaning Intensity (number), Reset Brush Head.
- **Web UI** (port 80) — live dashboard, MQTT/Wi-Fi settings, a read-only
  filesystem browser (view/download), and firmware update.
- **Wi-Fi** — joins the configured network and keeps retrying with backoff if it
  drops. With no credentials, or after about a minute of failed attempts, the open
  `oclean-setup` AP comes up (http://192.168.4.1) so the settings stay reachable; it
  goes away again once the network is joined.
- **BLE GATT server** — serves the Oclean service (`8082caa8…`) so the phone app can
  connect (best-effort protocol parity: status / sessions / control opcodes).

## Hardware map (from RE — see re/HARDWARE_MAP.md for confidence levels)
| Function | Pin(s) | Confidence |
|---|---|---|
| Battery sense | ADC1_CH0 = GPIO1 (×2 divider) | confirmed |
| I2C0 (AW8686X 0x6A, QMI8658) | SDA=36, SCL=35 | confirmed |
| Primary button | GPIO3 (pull-up, active-low) | confirmed |
| Charger detect / gyro wake | GPIO8 / GPIO9 | likely |
| Indicator LEDs (LEDC) | GPIO17–21 | confirmed |
| Display (ST7735S, SPI2) | MOSI40 SCLK39 CS38 DC41 RST42 | confirmed |
| Charge enable / WLC enable | GPIO26 / GPIO45 | confirmed / likely |
| **Motor (I2S)** | BCK33 WS47 DOUT34 | **likely — verify** |
| Motor amp-enable | **unknown** | **not located** |
| Display backlight | **unknown** (GPIO12?) | **not located** |

## Build
```
. $IDF_PATH/export.sh        # ESP-IDF v5.1.1
idf.py set-target esp32s3
idf.py build
```
(or `bash ~/esp/build_oclean.sh`). Output: `build/oclean_custom.bin` + bootloader + partition table.

## Flash (UART — do a safe bring-up)
Enter ROM download mode (hold BOOT/GPIO0 at reset) and, with a 3.3 V USB-serial adapter:
```
# 1) BACK UP STOCK FIRST (your only safety net):
esptool.py -p <port> read_flash 0 0x1000000 stock_full_backup.bin
# 2) (optional) capture the real partition table for reference:
esptool.py -p <port> read_flash 0x8000 0xC00 stock_ptable.bin
# 3) flash the custom firmware (full image, uses our partition table):
idf.py -p <port> flash monitor
```

### ⚠️ Bring-up cautions (read before first boot)
Several pins are inferred, not confirmed, and this firmware has not run on real
hardware. The RISKY ones are the **motor I2S pins**, the **(unlocated) amp-enable
GPIO**, and the **charge rails (GPIO26/45)** — a wrong pin can damage the device or
the battery. Recommended first boot: **power from the UART adapter / a current-limited
supply, not the battery**, watch the serial log, and confirm battery read + I2C
WHOAMIs + buttons before letting it drive the motor or enable charging. Everything is
recoverable over UART as long as you keep the stock backup and don't erase the
bootloader (0x0) / partition table (0x8000).

The firmware itself does **not** hold these back: the I2S clocks run and the charge
rails are driven as soon as it boots, and the motor starts on the first brushing
command. Charging is on by default; it is cut at 72 °C (back on below 67 °C) and
whenever the IMU temperature, once it has been read, goes missing. If the IMU is never
detected the brush still charges, but without a thermal cutoff (an error is logged).

## Update over Wi-Fi
Once the custom firmware is running, later firmware can be flashed from the web UI's
**Firmware** tab, or from a shell:
```
curl -H 'Content-Type: application/octet-stream' --data-binary @build/oclean_custom.bin http://<device-ip>/api/ota
```
The image is written to the idle OTA slot, verified, and booted. Any ESP32-S3
application image this bootloader can boot is accepted: it does not have to be a build
of this project, or an ESP-IDF app at all. The merged full-flash file (bootloader +
partition table + app) is not an application image and cannot boot from an OTA slot.

- **Builds of this project** keep rollback protection: if the new image crashes before
  it finishes starting up, the bootloader falls back to the previous one.
- **Any other firmware** is marked valid as soon as it is flashed, because it cannot be
  assumed to confirm itself after booting (the bootloader would otherwise revert it at
  its second boot). It stays installed, but there is no automatic rollback: if it does
  not work, reflash over UART.

Like the rest of the web UI this has no authentication: anyone who can reach port 80
can flash the device.

## Known-unknown / TODO
- Confirm motor I2S pins + find the amp-enable GPIO; confirm the backlight pin.
- Battery voltage→% curve is approximate (stock table not recovered).
- AW8686X force register is best-effort (0x06); refine from the datasheet.
- Session history uses a simple NVS counter (stock `brushdata` raw-partition record
  format + cloud/phone sync not reimplemented).
