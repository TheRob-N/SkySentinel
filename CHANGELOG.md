# Changelog

## 2.1.1 (November 10, 2026)

### Important fix
- **Web installer:** a Fresh install on an ESP32-S3 board (Heltec, T-Embed) wrote the full
  image at the wrong address after erasing the chip, leaving the board with no bootloader
  (dark screen, nothing runs; a USB flash recovers it). The installer and the console's
  FIRMWARE page now recognize full images by their partition table, and refuse to write
  a file that doesn't match the kind picked.

### Sub-GHz (T-Embed CC1101)
- **In LIVE DEVICES:** bursts reach the master and show as one row per band ("Sub-GHz
  bursts: Ford fobs, tire sensors", ADDRESS 315 MHz) in a new **Sub-GHz** category, on by
  default. A row is a band, not a device: bursts carry no ID until they're decoded.
- **Radio filter** next to the search box: all, Wi-Fi, Bluetooth LE, Classic BT, LoRa or
  Sub-GHz.
- **314.35 MHz band:** Toyota / Lexus smart keys (e.g. HYQ14FBA) send at 314.3 MHz,
  outside the 315 MHz look. Bands are labeled with what usually uses them (Ford fobs at
  315 MHz, e.g. N5F-A08TAA).
- **Far fewer false bursts:** a burst must now be loud *and* last: by default 30 dB over
  the band's quiet level for at least 15 ms. Single 2.5 ms spikes (the board's own radio,
  the screen, USB) had shown as bursts on every band every few seconds. Both are settings
  (10 to 30 dB; 2.5, 7.5, 15, 20 or 30 ms).

### T-Embed CC1101
- **Screen:** no more flicker (each strip is drawn in memory, then sent whole), the
  SkySentinel logo in the header as on the CYD and the web console, and one click of the
  wheel per page (was 2 to 3).
- **POWER menu** (hold BACK 2 s), as in Bruce: Screen off (keeps scanning), Deep sleep
  (BACK wakes it), Power off (battery disconnected, USB turns it on), Restart. A press of
  BACK still turns the screen off and on.
- **SETTINGS menu** (hold the wheel 1 s): brightness, screen timeout, start page, turn the
  screen around, LED ring alerts (a color per alert, or off) and their brightness, burst
  loudness and length, bands to watch, jamming alerts to the master.
- **LED ring = alert light:** solid while jammed (red), a flash per burst (blue), a slow
  blink while looking for the master (yellow).

### Reliability
- **Nodes at the edge of range** (link -78 dBm or worse when the master was lost) no
  longer restart every minute while looking for it: they keep calling and restart only
  after 5 minutes. A sudden loss on a strong link still gets the quick restart.
- **Long Range at the edge:** the stuck-frames restart waits 5 s on a Long Range link (was
  1.5 s): at -88 to -94 dBm the driver retries each frame many times at the slowest rate,
  and frames that were only slow restarted a working node every minute or two. Status
  shows `tx_wait_max`. (Far-side test: the restarts dropped from about 1 every 3 minutes
  to about 1 every 1.5 hours; the far side still proved past this board's reach.)
- **Classic boards use less memory:** the sighting queue is 32 on a CYD (was 64) and 48 on
  a DevKit (was 96), and the loop stack is 6 KB (was 8 KB; the CYD's loop never used more
  than about 3.2 KB in a day). The CYD's lowest free memory over a day rose from about
  12.3 KB to 17.5 KB, plus the 2 KB from the stack. Status shows `loop_stack_min`.

### Fixed
- **MAKER for trackers:** a Chipolo showed as "Apple" (its Find My adverts carry Apple's
  company number). Trackers the signature identifies now show their own brand (Chipolo,
  Tile, Samsung), and the details say whose company number the advert carries.
- CYD: the Vehicles category had no short name or color on the dashboard (blank label).

## 2.1 (November 2026)

### New
- **MeshCore** on the Heltec LoRa node: companions, repeaters, room servers and sensors
  from their adverts (name, kind, position if shared, repeater hops). Other MeshCore
  traffic is counted.
- **LoRa channel picker** (CLUSTER → EDIT → LORA, or `node <ID> lora <mode> [slot]`):
  AUTO takes turns (LongFast, MediumFast, MeshCore, LoRaWAN), or pin the radio to any
  Meshtastic preset (with an optional frequency slot), MeshCore or LoRaWAN.
- **Per-node link** (CLUSTER → EDIT → LINK TO THE MASTER, or `node <ID> link
  auto|normal|lr250|lr500`): mix normal and Long Range nodes in one cluster.
- **EVERYTHING job** for a node in another room or at the edge: it scans the whole plan on
  its own, and the other nodes split the work as if it weren't there.
- **Which node hears it:** a NODE column in the device list (the board that hears it best,
  and how many hear it), "heard by" in new-device events, and HEARD BY in device details.
- **Logging tools:** `tools/sslog.py` now has a command mailbox (`~/name.cmd`) so nothing
  else opens a board's port; `tools/sscheck.sh` prints a one-screen cluster health check.
- **`bletest`** serial command: a 3-minute Bluetooth self-test for any C5 / S3 board.

### Much better Bluetooth
- **ESP32-C5:** part-time BLE scan windows (e.g. 35 ms of 100) hear almost nothing on a C5
  (2 to 3% of full-time, measured on two boards). C5 boards now listen full-time in blocks:
  3.5 s of every 10 on a node, 1.5 s of every 10 on a master. A C5 master hears about 6x
  more Bluetooth than in 2.0; a C5 node on BOTH / EVERYTHING went from nothing to hundreds
  of adverts a minute.
- **Heltec (ESP32-S3):** its scan was stopped and restarted around every report to the
  master (once a second), which left it hearing about 5% of what it could. Bluetooth now
  steps aside only while frames leave: about 7x more Bluetooth heard.

### Reliability
- **esp-idf #19269 (frames stuck in the Wi-Fi driver):**
  - the C5/S3 recovery now covers the normal link too;
  - classic ESP32 boards (CYD, DevKit) get the same recovery (a CYD had been slowly losing
    send slots over a day);
  - fewer occurrences: Bluetooth pauses briefly for each send, and nodes report every 5 s
    (dashboards 1 s). The Heltec went from several restarts a day to about one in two days.
- **Long Range rejoin:** nodes remember "use LR" across their own restarts and keep
  calling in both modes; the master answers a joining node in both. A node at the far side
  no longer gets stranded when it restarts while the master is away.
- **Classic nodes** wait 45 s (was 20 s) without hearing the master before re-pairing: the
  DevKit went from re-pairing every 20 to 30 minutes to a few times a day.
- **Master USB self-heal:** the C5 master's USB output could stop (scanning carried on)
  until a reset; it now detects a stuck send buffer and restarts the USB transmitter.
- **GPS events** debounced: a fix that comes and goes indoors is reported once.
- Restart reasons: every board logs `[boot] reset reason N, restart tag ... -> why`;
  stuck-frames restarts show as `stuck_tx`.

### Fixed
- The master's own Bluetooth no longer crowds out its nodes' frames (classic nodes had
  been losing about a quarter of theirs right after the C5 Bluetooth change).
- A dashboard CYD no longer falls back to "Waiting for dashboard data" now and then.

## 2.0 (November 3, 2026)
Ground-up rewrite: multi-board clusters over encrypted ESP-NOW, web console, CYD touch
dashboard, LoRa (Meshtastic, LoRaWAN), Long Range link, detection log, web installer.
The original single-board CYD sketch is v0.5.9 on the `legacy-cyd` branch.
