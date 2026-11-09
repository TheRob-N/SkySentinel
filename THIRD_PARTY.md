# Third-party software and data

SkySentinel's own code is under the PolyForm Noncommercial License 1.0.0 (LICENSE).
These parts belong to others and keep their own licenses. Firmware images built
from this source contain the libraries marked "in firmware".

| Part | Used for | License |
|---|---|---|
| Arduino core for ESP32 (Espressif), with ESP-IDF | in firmware: everything | Apache-2.0 (ESP-IDF), LGPL-2.1 (parts of the Arduino core) |
| NimBLE-Arduino (h2zero), Apache NimBLE | in firmware: Bluetooth LE scanning (C5, V8, Heltec) | Apache-2.0 |
| RadioLib (Jan Gromeš) | in firmware: LoRa receiver (Heltec) | MIT |
| Adafruit GFX Library | in firmware: screen drawing (CYD, Heltec) | BSD-2-Clause |
| Adafruit ILI9341 | in firmware: CYD screen | MIT |
| Adafruit ST7735 and ST7789 Library | in firmware: CYD 2USB / Heltec screen | MIT |
| esptool-js (Espressif) | website: the in-browser installer (`site/assets/esptool.js`) | Apache-2.0 |
| IEEE OUI registry, Bluetooth SIG assigned numbers | signature data (`lib/sscore/ss_sigdata.h`) | public registries |

Signature research credits (Flock research, DeFlock, ouispy-detector, flock-you,
GainSec, the Nearby Glasses app, the DJI DroneID research) are listed in
`lib/sscore/ss_sigdata.h`.

The MeshCore packet and advert layout (`lib/sscore/ss_lora.cpp`) follows MeshCore's
published format; MeshCoreSniffer (SarahRoseLives, MIT) was the reference for
reading it. No code from it is included.

The LGPL-2.1 parts of the Arduino core are linked into the firmware. Their source is
at https://github.com/espressif/arduino-esp32, and this project's full source is
published with every release, so you can rebuild the firmware with a changed copy
of that library.
