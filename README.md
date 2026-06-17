# SkySentinel

Passive RF detector for Flock Safety ALPR/Raven cameras and FAA Remote ID drone broadcasts, built on a $15 "Cheap Yellow Display" ESP32 board. No transmitting, no active probing — it only listens.

![status](https://img.shields.io/badge/status-active%20development-yellow)
![platform](https://img.shields.io/badge/platform-ESP32--2432S028-blue)

<!-- Add a photo of the running device here once you've got a good shot, e.g.: -->
<!-- ![SkySentinel running](docs/photo.jpg) -->

## What it does

SkySentinel passively monitors BLE and WiFi traffic for signatures associated with:

- **Flock Safety cameras** — BLE OUI/manufacturer-ID/name matches, Raven GATT service UUIDs, and WiFi OUI/SSID patterns
- **FAA Remote ID drones** — BLE broadcasts under the ASTM F3411 standard (service UUID `0xFFFA`), decoding Basic ID, Location, and System messages

When something matches, the display shows a live DRONE / FLOCK CAM split-panel view with signal strength, time since last seen, and an optional one-shot audio alert. Detections are logged to the SD card in WiGLE CSV format, the same format used by the [WiGLE](https://wigle.net) wardriving community, so flock cam locations can be reviewed, mapped, or shared the same way wardriving data is.

It is entirely passive. It transmits nothing and probes nothing — it only listens to what's already being broadcast.

## Hardware

- **Board:** ESP32-2432S028 ("Cheap Yellow Display" / CYD) — ILI9341 2.8" 240×320 resistive touchscreen, SD card slot, WiFi + BLE, ~$12–15 on AliExpress/Amazon
- **GPS (optional, recommended):** NM-ATGM336H GPS/BDS module, wired to the board's P3 connector (GPIO27 RX)
- **Speaker (optional):** for audio alerts — not required for full functionality

| Function | GPIO | Notes |
|---|---|---|
| TFT (HSPI) | MOSI 13 / MISO 12 / SCLK 14 / CS 15 / DC 2 | `ILI9341_2_DRIVER`, not `ILI9341_DRIVER` |
| Touch | handled internally | via SoftSPI in the ggaljoen TFT_eSPI fork — see below |
| SD (VSPI) | MOSI 23 / MISO 19 / SCLK 18 / CS 5 | |
| Backlight | GPIO 21 | PWM |
| Boot button | GPIO 0 | active LOW — short press cycles brightness, long press (2s) cycles volume |
| GPS (P3 connector) | GPIO 27 (RX) | not yet wired in your build until a GPS module is attached |
| RGB status LED | R 4 / G 16 / B 17 | active LOW |

## Firmware setup

**Arduino IDE**, board: `ESP32 Dev Module`, partition scheme: **Huge APP (3MB No OTA / 1MB SPIFFS)** — the default partition is too small for this sketch.

### Required library: ggaljoen's TFT_eSPI fork

This project depends on a specific fork of TFT_eSPI: **[ggaljoen/TFT_eSPI](https://github.com/ggaljoen/TFT_eSPI)**, not the mainline Bodmer version. The fork drives the CYD's touch controller internally via SoftSPI, which avoids an SPI bus conflict between the touch controller and the SD card that otherwise breaks one or the other.

1. Delete any existing `TFT_eSPI` folder from your Arduino `libraries` directory
2. Download/clone `ggaljoen/TFT_eSPI` into `libraries/TFT_eSPI`
3. Copy `User_Setups/Setup303_CYD_ESP32-2432S028R.h` to `libraries/TFT_eSPI/User_Setup.h`
4. Confirm `libraries/TFT_eSPI/User_Setup_Select.h` has `#include <User_Setup.h>` uncommented (default)

**Watch out for:** a file named `TFT_config.h` may ship in the library root for ESP-IDF/PlatformIO Kconfig support. It unconditionally defines `USER_SETUP_LOADED`, which silently prevents your real `User_Setup.h` from ever being included. If you hit `'class TFT_eSPI' has no member named 'getTouch'` at compile time, this is almost certainly why — rename it to `TFT_config.h.disabled`.

Also check your **sketch folder itself** doesn't contain a stray `User_Setup.h` — Arduino's local-include-path priority means a sketch-local copy silently wins over the library's, even if the library one is correct.

### Touch calibration

Touch calibration is hardware-specific and must be measured per orientation — don't guess at the 5th calibration byte (the rotation/orientation indicator); it is not a simple flip. Run `tft.calibrateTouch()` once per rotation you plan to support and hardcode the results. This repo's `display.h` already contains measured values for both normal and flipped orientation on the build it was developed against, but your panel will likely differ slightly.

## Project structure

| File | Purpose |
|---|---|
| `SkySentinel.ino` | Setup/loop, FreeRTOS task spawning, button handling, brightness/alert logic |
| `display.h` | All UI drawing, touch handling, page navigation |
| `ble_scan.h` | BLE scan callback — Flock + Remote ID matching |
| `wifi_scan.h` | Async WiFi scan — Flock OUI/SSID matching |
| `flock.h` | Flock Safety OUI/name/manufacturer-ID/Raven-UUID/SSID reference lists |
| `sd_log.h` | SD card logging in WiGLE CSV format |
| `settings.h` | NVS-backed settings (brightness, volume, SD logging, display flip) |
| `gps.h` | GPS state + (stub) NMEA parsing — fill in once a GPS module is wired |
| `drone_id.h` | Reserved for future WiFi NAN Remote ID decoding |

## Detection coverage

- **BLE:** Flock OUI matches, name substring matches, manufacturer ID `0x09C8`, Raven GATT service UUIDs; FAA Remote ID via service UUID `0xFFFA`
- **WiFi:** Flock OUI and SSID pattern matches (2.4GHz only — see note below)
- **Hardware limitation:** the ESP32's WiFi radio is 2.4GHz-only at the silicon level. Research into Flock camera hardware suggests this isn't a meaningful detection gap: Flock's WiFi radio appears to be active mainly during installation/field-service rather than normal operation, with BLE/Raven as the more reliable continuous signal — and FAA Remote ID (ASTM F3411) is itself defined over BLE and 2.4GHz WiFi, with no 5GHz variant.

## Status

Actively in development. UI, touch, SD logging, and BLE/WiFi Flock + Remote ID detection are working on hardware. GPS integration and a physical enclosure are next. A field test against a known Flock camera is planned.

## Known reference point

A confirmed Flock ALPR camera location is logged for field-testing detection logic during development (Wellsboro, PA area) — see commit history / project notes for coordinates if continuing this work.

## Credits / prior art

- [justcallmekoko/ESP32Marauder](https://github.com/justcallmekoko/ESP32Marauder) — reference for Flock OUI/detection patterns
- [ggaljoen/TFT_eSPI](https://github.com/ggaljoen/TFT_eSPI) — CYD-compatible TFT_eSPI fork
- HaleHound-CYD, nyanBOX — reference firmware for CYD-based RF detection projects
- [WiGLE](https://wigle.net) — wardriving database and CSV log format

## Disclaimer

This project is for personal situational awareness and research purposes. It passively listens to publicly broadcast RF signals and does not transmit, jam, spoof, or interfere with any device. Always comply with local laws regarding RF monitoring and privacy in your jurisdiction.
