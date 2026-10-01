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
- **Web UI** (port 80) — live dashboard, MQTT/Wi-Fi/brush settings, and a read-only
  filesystem browser (view/download).
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

## Known-unknown / TODO
- Confirm motor I2S pins + find the amp-enable GPIO; confirm the backlight pin.
- Battery voltage→% curve is approximate (stock table not recovered).
- AW8686X force register is best-effort (0x06); refine from the datasheet.
- Session history uses a simple NVS counter (stock `brushdata` raw-partition record
  format + cloud/phone sync not reimplemented).
