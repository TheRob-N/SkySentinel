# Development

Code layout, building and testing, and the website. Back to the [README](../README.md).

## Layout

| Path | What |
|---|---|
| `lib/sscore/` | Pure C++ (no Arduino): wire protocol, COBS, scan plans, parsers, fingerprints, signatures, device table, node assignment. Runs on ESP32, a Linux SBC, or the host. |
| `src/common/espnow_link.*` | ESP-NOW transport, cluster key, HMAC pairing tags |
| `src/app/main.cpp` | The firmware. One image; the board's role (standalone, master, node) is a saved setting |
| `src/common/core_common.*` | Device table setup and the JSON-lines output |
| `webui/index.html` | Web UI: radar, live device list, categories, board setup. One offline file, also built into the firmware |
| `src/common/web_portal.*` | Wi-Fi access point: serves the web UI, live stream, captive-portal popup |
| `tools/embed_webui.py` | Gzips the web UI into the firmware on every build (runs automatically) |
| `tools/update_bt_names.py` | Refreshes the Bluetooth company, device-type and service names inside the web UI |
| `tools/ssproto.py` | Python decoder for the same frames (SBC master, debugging) |
| `tools/ssmon.py` | Live console view of the JSON stream (solo board or Core) |
| `src/common/cyd_ui.*` | CYD touch dashboard (display detect, touch, setup screens) |
| `src/common/tembed_display.cpp`, `node_display.*` | T-Embed and Heltec screens |
| `src/common/subghz_node.*`, `lora_node.*`, `zb_node.*` | CC1101 band watch, LoRa listener, 802.15.4 (Zigbee / Thread) listener |
| `src/classic/main.cpp` | Firmware for the original ESP32 (CYD, DevKit): Classic Bluetooth node |
| `test/host/` | Host unit tests (2,500+ checks, run under ASan/UBSan): `make -C test/host` |

**HELP** in the web UI has the plain-language version of this README: what each
board contributes, the roles, how to read the screen, how devices are identified, the
settings, and what SkySentinel can't see. It's in the Wi-Fi copy too.

## Build and flash

Every board gets the same firmware. What it does is chosen afterwards and saved on
the board, so turning a node into a master (or back) never needs a reflash.

```
pio run -e c5 -t upload       # any C5 board (DevKitC-1 today)
pio run -e v8 -t upload       # Marauder V8 (PSRAM build, defaults to master)
pio run -e classic -t upload  # original ESP32 (WROOM-32 DevKit): Classic Bluetooth node
pio run -e cyd -t upload      # CYD / ESP32-32E: Classic node + touch dashboard
pio run -e heltec -t upload   # Heltec Wireless Tracker: LoRa node (Meshtastic, MeshCore, LoRaWAN)
pio run -e tembed -t upload   # LilyGO T-Embed CC1101: sub-GHz node
make -C test/host             # host tests
python3 tools/test_ssproto.py # Python vs C++, byte for byte
```

The ESP32-C5-DevKitC-1 has two USB-C ports. The build uses the chip's **native USB**
port (`303a:1001`, `/dev/ttyACM0`) for both flashing and serial, so one cable does
everything and no BOOT/RST buttons are needed.

## Website (skysentinel.neyhard.com)

`site/` is the public site: home page with the supported-board grid and comparison
table, an installer (pick a board, update or fresh install, flash over Web Serial with
esptool-js), how-tos, and the console itself under `app/`. The install page and the
console warn when the browser can't do USB (Safari, Firefox before 151, phones) and point to the
Wi-Fi console instead (on a phone, only those two pages show it).

    pio run -e c5 -e v8 -e heltec -e cyd -e classic -e tembed
    python3 tools/build_site.py --version 2.1.7 --git https://github.com/TheRob-N/SkySentinel
    -> dist/site/ (upload its contents) and dist/site.zip

It's all static files, nothing loaded from other sites. The server needs HTTPS (Web
Serial only works on secure pages). `downloads/` gets a source archive and SHA256SUMS
with every build, so the code is published even without a public git host. Board
list: `site/assets/boards.js`.

## Not built yet

- COBS serial transport (implemented and tested in `ss_proto` and `ssproto.py`) for
  USB-hub clusters and a single-board-computer master
- Distance travelled from GPS for following alerts (`FollowMonitor::notePlaceChange`)
- Tire-pressure sensor and fob IDs on the T-Embed
