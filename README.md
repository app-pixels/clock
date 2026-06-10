> Part of [**app-pixels.com**](https://www.app-pixels.com) — browse + flash this app at [`/apps/clock`](https://www.app-pixels.com/apps/clock).

# clock

**Clock** · v1.0.0

Dot-matrix departure-board clock, NTP-synced over WiFi.

**Hardware:** Waveshare ESP32-S3 1.8" AMOLED Touch

**Tags:** `#time` `#wifi`

NTP-synced flip-clock display: every digit and letter is drawn as a 5×7 LED dot matrix on a black grid, mimicking the airport departure-board look (Solari Mareus split-flap successor era).

Shows time, weekday, day + month, year, and ISO week number. Auto-handles daylight saving once you set your `TIMEZONE`.

## Controls
- **BOOT** — rotate display (portrait ↔ landscape)
- **PWR** — cycle brightness

## `setup.txt` keys
**Mandatory**
- `SSID` / `PASSWORD` — for the NTP sync
- `TIMEZONE` — POSIX TZ string (e.g. `CET-1CEST,M3.5.0,M10.5.0/3` for Vienna)

**Optional**
- `SSID2` / `PASSWORD2`, `SSID3` / `PASSWORD3` — WiFi fallbacks
- `CLOCK_COLOR` — RGB888 hex, e.g. `#FE6000` for amber. Default white.

## Editing `setup.txt`
The device reads `/setup/setup.txt` from the SD card on boot. [Download a working sample](https://sosbxffigpteqilpgxwn.supabase.co/storage/v1/object/public/app-assets/setup/setup.txt) — covers every app — and edit the keys you need.

Don't want to eject the card? Use the [**USB Stick**](/apps/usb-stick) app (mounts the SD card as a USB drive over USB-C) or the [**Filehub**](/apps/filehub) app (edit over WiFi).

## Build

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x.
2. Add the ESP32 board package (≥ 3.1.0):

   ```
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Install the required Arduino libraries:

   - Adafruit XCA9554
   - GFX Library for Arduino (moononournation)
   - XPowersLib (lewishe)

4. Compile and upload:

   ```
   FQBN='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,LoopCore=1,EventsCore=1'
   arduino-cli compile -b "$FQBN" --build-path /tmp/clock_build .
   arduino-cli upload  -b "$FQBN" --input-dir /tmp/clock_build -p /dev/ttyACM0 .
   ```

   For browser flashing without a build environment, use the [pre-built binary](https://www.app-pixels.com/apps/clock).

## License

MIT — see [LICENSE](LICENSE). Do whatever you want with it.

---

Part of the [app-pixels.com](https://www.app-pixels.com) catalogue · live listing: https://www.app-pixels.com/apps/clock
