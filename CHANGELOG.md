# Changelog

## 2.1.3 (2026-11-13)

**T-Embed CC1101 is no longer beta.** Every board now reports exactly which build it runs,
the cluster picks its own channel, and the master keeps up with a flood and with a CYD.
Every board needs this release: nodes now follow the master between channels 1, 6 and 11,
and an older node only ever looks on 6. Flash the nodes first, the master last.

Known issues (both recover on their own):
- The Heltec Wireless Tracker restarts itself when frames stick in the Wi-Fi driver,
  about every 1.5 to 2.5 hours (the T-Embed and C5 boards don't). Suspects: its LoRa or GPS.
- A DevKit at the edge of the link loses frames in waves of 20 to 40 minutes; it no
  longer drops off the cluster over it.

- **T-Embed: six pages.** New **ALERTS** (the cluster's latest alert, what's of concern
  nearby, recent events; from the master with each report), **HISTORY** (the last 10
  minutes of each sub-GHz band) and **FOXHUNT** (pick a sub-GHz band or a Bluetooth device
  this board hears, then walk toward it: a big live signal number, a peak-hold bar and
  the LED ring as a meter). HOME shows the cluster's latest alert too.
- **T-Embed night theme:** red on black like the CYD's, with its own screen brightness
  and a dimmer LED ring (SETTINGS > Theme).
- **T-Embed LED ring:** a new alert from the cluster pulses it (its own color) for as long as
  SETTINGS > Alert flash says: 10 s, 30 s (default), 1 min, 5 min or until a button; any
  button press stops it, so it doubles as "seen".
- **Cluster cards redone:** each board's card is a glance (status dot, link bars, loss and
  memory meters, its job as chips, and a warning strip only when something needs a look);
  click a card for everything about it in a window like EDIT's: link, health, radio
  driver, scanning, and the EDIT, IDENTIFY, REBOOT and FORGET buttons.
- **Loss over the last 10 minutes:** the card's loss meter and warning use the last 10
  minutes, so a board that lost frames early and has been clean since stops looking bad.
  The details window shows the last 10 minutes, the last hour and the total since the
  master started. The master keeps these windows itself and puts them in its status line
  (`l10`, `l60`, tenths of a percent, with `l10s` / `l60s` seconds of history), so they
  survive a page reload, land in the logger's master.log, and `sscheck.sh` prints them as
  "lately". With an older master the page works them out from what it sees.
- **Which build is on each board:** every firmware is stamped with when it was built
  (tools/build_stamp.py, run by PlatformIO). Boards send their release and build stamp in
  the heartbeat and status line; the details window shows "firmware 2.1.3 · built
  20261113" (orange when it differs from the master's) and `sscheck.sh` prints it.
- **CYD and DevKit report their health:** the classic firmware now sends free memory, why
  it last started and how many times it lost the master, like the C5 and S3 boards
  always did (the page showed "not sent").
- **The cluster picks its own channel:** the link (ESP-NOW and the master's Wi-Fi page) can
  now be on channel 1, 6 or 11 instead of always 6. The master surveys what the nodes hear
  on 2.4 GHz and weighs it the way it hurts the link: a strong network on an overlapping
  channel (a hotspot on 7 next to 6) counts most, one on the same channel much less.
  Automatic by default: it checks every 10 minutes (the first after 10 minutes up), moves
  at most once every 6 hours, and only when another channel is clearly better (a lossy
  link makes it readier). LINK CH on the cluster panel shows the survey, what's crowding
  each channel, the last move, and buttons for AUTOMATIC, CH 1 / 6 / 11 (stay there) and
  SURVEY NOW. The master tells the nodes, moves a few seconds later, and its Wi-Fi page
  moves with it. A node that misses the move, or restarts, looks for the master on each
  channel in turn (the last one first); a new board being paired does the same. Every
  board needs this firmware: an older node only ever looks on 6.
  The window answers a click at once ("Asked the master to move to channel 1…"), then
  shows the master's reply and "Done" when the move finishes.
- **Routers filed under their real channel:** a strong router is also heard on the
  channels next to it, and it was filed under whichever channel the board was tuned to
  (a hotspot on 7 showed as 9). Its beacon states its channel, and that's what's used now;
  its other frames heard next door no longer move it. The link survey also uses the board
  that hears each one best.
- **Steadier channel choice:** the first night flipped the link 11 -> 6 -> 11 -> 6, once
  every 6 hours, because a 15-minute survey swung wildly (one channel scored 32 to 130
  in a day). Now the survey is averaged over about an hour, a better channel has to win
  three checks in a row (30 minutes), and the master remembers the link's measured loss
  on each channel it has used and won't move back to one where the link did worse. LINK
  CH shows the hour average, the latest snapshot, the measured loss per channel, which
  way it's leaning, and anything it turned down.
- **Classic nodes wait for the master's answer:** after each heartbeat the CYD and DevKit
  stay on the master's channel until its answer arrives (up to 400 ms, like the C5/S3
  nodes) instead of 60 ms. A DevKit missed every answer for 45 s about once an hour and
  re-paired, though the master heard it fine. After 15 s without hearing the master they
  also stop hopping and pause Classic Bluetooth until they do.
- **Stuck-frame restart limit 3 s (was 1.5 s)** on the C5/S3 boards outside Long Range:
  a Heltec doing LoRa, GPS and Bluetooth tripped it about four times a night.
- **The master no longer chokes on the CYD's dashboard:** the "live devices" page it
  builds for a CYD on every heartbeat (once a second) ranked each listed device by walking
  the whole table again: about 70 ms of slow PSRAM reads per heartbeat, so frames from
  the nodes piled up and got dropped, worst for the CYD itself. It now keeps the top rows
  in one pass. The dashboard's per-category counts are one pass instead of one per
  category. The master's status carries `rxt`: milliseconds spent on node frames since
  the last status (receive, heartbeats with their replies, sightings, other), frame
  counts, and passes over the filing budget.
- **Device lookups are fast:** finding a device walked the whole table (2,048 entries in
  slow PSRAM on the master), twice per sighting, so each one cost around a millisecond and
  the master fell behind its nodes under a flood and for minutes after every start (24
  frames dropped, 4,245 sightings skipped). An address index in internal RAM (8 KB on the
  master, 1 KB on boards without PSRAM) makes it a lookup or two. When the table is full,
  eviction now frees the stalest 1/64 of it in one pass (fakes first, as before) instead
  of walking the table for every new device.
- **A flood no longer blinds the cluster:** a beacon / Bluetooth spam tool next to the
  master (every fake a new device, from every node at once) stalled the master for about
  a second per pass; its queue from the nodes overflowed (419 frames) and every node's
  loss spiked. The master now files sightings for at most 40 ms a pass and skips the
  rest (counted: "flood skipped" in its details, `shed` in its status, a log line every
  10 s), while heartbeats and everything else are still handled. Strong matches, anything
  50%+ confident and sub-GHz bursts are always filed; only ordinary sightings (reported
  again within seconds) are skipped. The first minute or two after the master starts
  looks the same (every device is new to it), so a few hundred skipped then is normal. Nodes pace their
  reports to 10 a second when their buffer fills. The spam alerts still fire.
- **Every sub-GHz burst is logged:** the master writes one line per burst (time, band,
  strength, which board heard it), page connected or not, so an overnight master.log keeps
  a timeline of fob presses, sensors and passing vehicles. `sscheck.sh` sums them per band
  with the last one.
- **The website shows each board's build:** the flash page and downloads list the release
  and build time for every board ("2.1.3 · built 20261113"), the same line the
  console's details window shows for a running board, so you can tell whether a board is
  on the published build. The console's FIRMWARE list shows it too.
- **T-Embed CC1101 out of beta** on the site, in the docs and the README.
- **Hover help:** the card meters, job chips, warnings, and every row and section of the
  details window explain themselves on hover (dotted underline = there's help).
- **Wi-Fi driver numbers from every node:** the heartbeat carries the slowest send the
  driver confirmed and how long frames were stuck before the last stuck-frames restart
  (esp-idf #19269). On each node's card on the page and in `sscheck.sh`, for nodes far
  from a laptop.

## 2.1.2 (November 10, 2026)
Polish on top of 2.1.1.

- **Website:** the T-Embed CC1101's card and how-to describe the 314.35 MHz band, the
  settings and power menus, the LED ring and the Sub-GHz category (still beta while it's
  being tested).
- **T-Embed header:** the SkySentinel logo stays top left when the node has a name; the
  name shows on the right, before the battery (it had replaced the logo).
- **T-Embed pairing screens:** centered, with the logo; the hint no longer runs off the
  screen; drawn once per change instead of every second (it flickered).
- **Port list:** the browser's serial port picker (console, FIRMWARE page and web
  installer) lists only ESP32 boards: Espressif's own USB, CH340 / CH9102, CP210x and FTDI.
  The `ttyS` entries are gone.
- **DevKit flashing:** `pio run -e classic -t upload` uses 115200 baud; the CP2102 kept
  failing at the default speed.
- **Heltec screen:** the SkySentinel logo (as on the CYD, the T-Embed and the web console)
  on its start screen and, at half size, in its header and on the pairing screen.

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
