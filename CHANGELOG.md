# Changelog

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
