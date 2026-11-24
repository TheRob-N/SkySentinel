# Console, serial commands and logs

The web console, the serial commands, logging a cluster and the detection log. Back to
the [README](../README.md).

## Web UI

`webui/index.html` is one self-contained file with no internet needed. It talks to
the board over USB with Web Serial, so it needs **Chrome, Edge or Firefox 151+ on a
desktop** (Linux, Windows, macOS). Safari doesn't have Web Serial.

1. Flash a board, plug it in, and close any serial monitor that has the port open
   (only one program can hold it).
2. Open `webui/index.html` in Chrome (double-click works, or
   `python3 -m http.server -d webui` and browse to `localhost:8000`).
3. Press **CONNECT USB** and pick the `USB JTAG/serial debug unit` port.
4. First time: the setup screen asks for the role. Pick STANDALONE for a lone C5.

What it shows: radar (distance from centre = signal strength; one board has no
bearing, so angles are only spread for readability), NOW / SESSION / ALERTS,
category tiles (now / total), the live device list with full MAC addresses,
maker (Bluetooth company), model names for AirPods / Beats and Swift Pair devices,
device types that devices announce themselves (earbuds, headset, watch...),
a detail view for any row (click it) with a live, decoded view of what a BLE device
is broadcasting,
address behavior (fixed, random, or rotations stitched), access point SSIDs (click "+N networks" for every network a router broadcasts), alert banner and event log. CATEGORIES sets
what to scan for and previews the scan plan (click a single tile to turn just that category on or off). SETTINGS changes role, cluster key and
master options. DAY / NIGHT switches theme (night is red, remembered per browser).

On Linux, if the port isn't listed or won't open: `sudo usermod -aG dialout $USER`,
then log out and in. After a role change the board reboots; the page waits and
reconnects by itself.

### Over Wi-Fi (no cable, any phone)

The board can run its own Wi-Fi network and serve the page itself, so a phone,
tablet or laptop can use it with no cable, no app and no internet. Safari on an
iPhone works too, since nothing needs Web Serial.

1. Connect once over USB, press **SETTINGS**, set **Wi-Fi access point** to ON, give it
   a name and a password (8+ characters, or blank for an open network), and save.
   The board reboots with the network up. Over serial: `wifi on <password|-> <name>`.
2. On the phone, join that network. Most phones pop up the page by themselves
   ("sign in to network"); if not, browse to `http://192.168.4.1`.

**Bluetooth scanning** (SETTINGS): *Listen only* (default) never transmits. *Ask*
(active scanning) sends each device a scan request, and most answer with a scan
response that often holds their name. Devices can see they were asked. Serial:
`scan active|passive`.

**Ask one device** (detail view → ASK): connects to just that device for a few
seconds and reads what it publishes about itself (name, maker, model, firmware,
battery, services), the same thing phone apps like nRF Connect do. Scanning pauses
meanwhile, and the device can see the connection. Only works on devices that accept
connections; broadcast-only ones (trackers, beacons) share everything in their adverts.
Serial: `query <mac> r|p` (random or public address).

Up to 3 browsers can watch at once, and anything on the page works over Wi-Fi the
same as over USB, including SETTINGS and CATEGORIES.

The radio trade-off: an access point has to stay on one channel (the link channel). On a **master**
that costs nothing, because a master already stays on the link channel to hear its nodes. On
a **standalone** board, Wi-Fi scanning drops from 38 channels to that one and
BLE is capped at 50% so the phone gets air time. BLE gear (trackers, body cams,
phones, earbuds, watches) is still covered. For full Wi-Fi coverage with the AP on,
add a node.

The copy of the page on a website (HTTPS) can use USB but not a board's Wi-Fi:
browsers block a secure page from talking to a plain local address. Use the copy the
board serves.

## Installing firmware from the web UI

**FIRMWARE** (top right, USB only) installs or updates any board from Chrome or Edge,
no PlatformIO needed. Pick a `.bin` from a build folder:

- `firmware.bin` = **update**: replaces the app, keeps role, keys, Wi-Fi and categories
- `firmware.factory.bin` = **fresh install**: everything, settings erased, setup runs again

It reads which chip the file was built for and refuses to flash the wrong board, and
shows step-by-step flashing instructions for that kind of board (port, buttons, what
to do after).
For a hosted copy (your website), `python3 tools/release_firmware.py` puts the
latest builds and a list next to the page, and FIRMWARE offers them as buttons.
The copy the board serves over Wi-Fi leaves the installer out (a phone can't flash
over Wi-Fi). Flashing uses Espressif's esptool-js (Apache-2.0), bundled in the page.

## Serial commands

Every role: `role <r>` · `setup <r> [key]` · `setup done|reset` · `key <text>|-` ·
`wifi on|off <password|-> <name>` (standalone and master) ·
`status` · `reboot` · `factory` · `json on|off` · `ui on|off`
Standalone and master: `cats all` · `cats surv` · `cats <hex>` · `devices` · `aps <id>`
Master: `mode split|mirror` · `local on|off` · `linkch auto|survey|1|6|11`. Node: `core <mac>|-` (pin to one master).

Category bits (32-bit since 2.1.6): flock (ALPR) 0x2, bodycam 0x4, tracker 0x8, glasses
0x10, drone 0x20, skimmer 0x40, pentest 0x80, phone 0x100, audio 0x200, wearable 0x400,
other_ble 0x800, other_wifi 0x1000, lora 0x2000, vehicle 0x4000, subghz 0x8000, camera
0x10000, smarthome 0x20000. Example: body cams + trackers only = `cats c`.

`python3 tools/ssmon.py /dev/ttyACM0` (needs `pip install pyserial`) is a plain
terminal view of the same stream.

**Logging a cluster:** `tools/sslog.py <port> ~/name.log` keeps a board's output in a file
(status once a minute, everything else as it comes) and reconnects by itself. To send that
board a command, write it to the logger's mailbox, `~/name.cmd`
(`echo status >> ~/master.cmd`), instead of opening the port again: on the C5's built-in
USB a second program on the port has left the board's USB output stuck until a reset.
`tools/sscheck.sh` reads the logs (`master.log`, `devkit.log`, `cyd.log` in your home
folder) and prints one health summary: per-node loss, restarts and why, the master's USB
and memory. `bletest` (any C5 / S3 board) runs a 3-minute Bluetooth self-test and prints
how much it hears with Wi-Fi parked, hopping and scanning; use it on a new board.

JSON lines: `{"t":"status",...}` every second (role, setup, scan plan, nodes, now/total
per category, link stats), `{"t":"devs",...}` the live device list while the web UI is
attached (`ui on`), `{"t":"dev",...}` when an interesting device appears or upgrades (MAC,
category, signature, confidence, RSSI per node, rotation count, fingerprint, name),
`{"t":"event",...}` for floods, spam, evil twins and jamming (`rf_jam`, `gps_jam`,
`gps_spoof`), `{"t":"node",...}` on join/loss.

## Detection log

The master records what it found, when and where:

- **What:** every device that raises an alert or a notice (a surveillance-category
  match at 40 or more, or a strong one) when first seen or when its match gets stronger,
  and again the first time it's heard on each later UTC day (`detail` = `new`,
  `upgraded`, `seen today`), so a device that keeps coming back shows on every day it
  did. Plus following and lingering alerts (any category, tires and cars included),
  deauth / disassoc floods, BLE and Wi-Fi spam, evil twins, fake channel switches, WPA3
  floods, and sub-GHz and GPS jamming or spoofing. Phones, earbuds and other ordinary
  devices are not logged (see the constraint below).
- **When and where:** from a GPS node (the Heltec). Its heartbeat now carries its
  position and the GPS clock. Without a GPS node, records have the master's uptime only.
  While the position is fresh, a `track` row is added every minute the cluster moves
  more than about 50 m, so the file also draws your route.
- **Master flash:** the C5's spare flash (896 KB) keeps two files of up to 384 KB; when
  the newer one fills, the older one is dropped. That's several thousand records, and
  they survive power cuts.
- **CYD SD card:** the master forwards each record to every CYD in the cluster, which
  appends it to `/SkySentinel/YYYY-MM-DD.csv` on a FAT32 card (UTC date; `undated.csv`
  until the GPS has given the time). A CYD only gets records while it's online.
  SETTINGS on the CYD and its CLUSTER card show **SD logging**, **SD no card** or
  **SD error**. The card can go in or come out while it runs (it checks every 30 s).
  The CYD's own touch pins are now read in software, because the ESP32's second SPI
  bus belongs to the SD card. The ESP32-32E's touch shares the display bus, so it's
  unaffected.
- **Download:** SETTINGS → DETECTION LOG → DOWNLOAD LOG (CSV). Over USB this sends
  `log dump`; over the board's Wi-Fi it fetches `/log.csv`.

CSV columns: `time_utc, uptime_s, lat, lon, kind, category, signature, mac, name, rssi,
conf, strong, detail`. `kind` is `start`, `device`, `following`, `lingering`,
`deauth_flood`, `disassoc_flood`, `ble_spam`, `wifi_spam`, `evil_twin`, `rf_jam`,
`gps_jam`, `gps_spoof` or `track`. Google My Maps imports it
directly (choose lat / lon as the position columns).

Serial: `log` (status), `log dump`, `log clear`, `log on|off`; on a CYD, `sd`, `sd ls`,
`sd get <file>`.

**Constraint: the CYD logs alerts and notices, not every device.** Logging every device
would mean the master sending a summary of its whole device table to the CYD every few
minutes and the CYD writing it to the card. The CYD is the tightest board for memory
(about 23 KB free, its largest free block about 14 KB), and SD writes stall for 50 to
200 ms at times, so it would risk crashes and a laggy dashboard. Full logging is planned
for a Marauder V8 master, which has PSRAM and its own SD slot and would write its own
device table with nothing extra over the radio (waits on V8 SD support).

### HISTORY

The **HISTORY** button swaps the live view for a full-width history page (LIVE VIEW, the
SKY SENTINEL logo, or HISTORY again goes back; the live view keeps updating meanwhile).
Clicking the HISTORY header folds the days and totals away (arrow up: open, down: folded;
remembered), leaving the table:

- **From:** Automatic (the default: a CYD's SD card when one is logging, since it has
  every day, otherwise the master's log), a node's SD card (pulled through the master),
  or the master's own flash log (the most recent few thousand records). The choice is
  remembered in the browser. Sources pulled before stay listed,
  so history can be read with nothing connected.
- **Which days:** LAST 24 H, LAST 48 H, or PICK DAYS on a calendar (your local days;
  outlined = on the card, dot = already in this browser; UNDATED = records from before
  the GPS gave the date). The card keeps one file per UTC day, so a local day can need
  two files; the page works out which.
- **DOWNLOAD** says what it will fetch and about how long, and asks first: while it sends,
  the CYD stays on the master's channel and pauses Classic Bluetooth, so it scans less.
  It keeps logging. Pulled files are kept in the browser (IndexedDB); a day comes over
  once, and today's file only sends its new end. CANCEL keeps what arrived; DOWNLOAD again
  carries on. FORGET DOWNLOADS empties the browser's copy (the card keeps its own).
- **DEVICES / RECORDS:** DEVICES (the default) groups the range by device, laid out like
  LIVE DEVICES: type, what it is (with STRONG / FOLLOWING / LINGERING tags), MAC, peak
  signal, confidence, first and last seen, the days it was seen in everything
  downloaded, its record count and last place; headers sort, a row opens its details.
  Attacks, jamming and restarts go in EVENTS below. RECORDS is the raw log, one row per
  record.
- **What it shows:** totals (records, devices, following / lingering, attacks,
  restarts, the busiest categories), records per hour (per day over more than 3 days;
  red where something alerted), and the records, newest first, with a search box and
  kind and category filters (the kind filter starts on "All but track", since a moving
  cluster adds a track point a minute). A MAC opens that device across everything pulled: the
  days it was seen, first and last time, its names. Places link to OpenStreetMap.
  EXPORT CSV saves what's shown, in the log's own columns.

**SD files from the web page:** plug the CYD into the computer, CONNECT USB, SETTINGS →
SD CARD → SHOW FILES, then DOWNLOAD next to a file. It comes over the CYD's USB serial at
about 10 KB a second, a few lines at a time so the CYD keeps working meanwhile. For big
files, pulling the card is faster.

**SD files through the master (no cable to the node):** on the master's page, CLUSTER,
EDIT on the node's card, SD CARD, SHOW FILES, then DOWNLOAD. The master pulls the file
over the cluster link in 200-byte pieces, 8 at a time, and asks again for anything that
went missing, so a weak link is slower but the file still arrives whole. Expect about 10
to 20 KB a second (a day's log is usually a few hundred KB). The button shows the
percentage and doubles as CANCEL. While a download runs, that node stays on the master's
channel and pauses Classic Bluetooth inquiries, so it scans less until it's done. One
download at a time per cluster. It works for any node with an SD card (MSG_FILE_REQ /
MSG_FILE, `lib/sscore/ss_fileserve.h`). Serial on the master: `node <ID> sd ls`,
`node <ID> sd get <file> [offset]`, `node <ID> sd cancel`.

### Formatting an SD card

Every board with a card slot can erase and format its own card as FAT32 (the CYD now,
the Marauder V8 when it's added). It erases everything on the card, logs included, so
each way of doing it asks first:

- **On the CYD:** SETTINGS, tap the SD line at the bottom, then FORMAT SD CARD and
  TAP AGAIN TO ERASE ALL DATA within 4 seconds.
- **From the master's page:** CLUSTER, EDIT on the CYD's card, SD CARD, FORMAT SD CARD
  (a warning dialog first). The CYD shows FORMATTING... while it works.
- **CYD on USB:** SETTINGS, SD CARD, FORMAT SD CARD, or the serial command
  `sd format yes`.

It takes a few seconds to a minute. Cards over 32 GB are formatted FAT32 too. Logging
picks up again by itself afterwards.

