# SkySentinel 2.1

**Know what's watching.** A listen-only detector for surveillance gear and everyday radio
devices: Flock ALPR cameras, body cams, AirTags and other trackers, camera glasses, drones
(Remote ID decoded), card skimmers, Wi-Fi attacks and more. It runs on $10 to $40 ESP32
boards, alone or as a cluster, and shows everything in a web console (USB or the board's
own Wi-Fi) and on an optional touch dashboard.

> **2.0 is a ground-up rewrite.** The original single-board CYD sketch (v0.5.9) lives on the
> [`legacy-cyd`](../../tree/legacy-cyd) branch and the `v0.5.9` tag.

**New in 2.1:** MeshCore and a LoRa channel picker on the Heltec, per-node link (LR or
normal) and an EVERYTHING job for outposts, "which node hears it" in the device list,
about 6 to 7 times more Bluetooth heard by C5 and Heltec boards, and much better
recovery from a known ESP-IDF radio bug. See [CHANGELOG.md](CHANGELOG.md).

- **Install from your browser:** https://skysentinel.neyhard.com (no toolchain needed)
- **How-tos:** https://skysentinel.neyhard.com/docs.html
- **License:** PolyForm Noncommercial 1.0.0 (free for personal and noncommercial use), see [LICENSE](LICENSE)

```
 Nodes (any mix)                                        Master                        You
 ESP32-C5 (Wi-Fi 2.4 + 5 GHz, BLE) ─┐
 Heltec Tracker (LoRa, GPS)         ├─ ESP-NOW, ch 6 ─→ C5 / V8 / Heltec / T-Embed ─→ web console (USB or Wi-Fi)
 CYD (Classic BT, touch dashboard)  │   encrypted,       device table, alerts, log    CYD dashboard, SD log
 T-Embed CC1101 (sub-GHz)          ─┘   paired nodes
```

Every board runs the same firmware for its chip; whether it's standalone, the master or a
node is a setting, so changing roles never needs a reflash.

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
| `test/host/` | Host unit tests (1,891 checks, run under ASan/UBSan): `make -C test/host` |

**HELP** in the web UI has the plain-language version of this README: what each
board contributes, the roles, how to read the screen, how devices are identified, the
settings, and what SkySentinel can't see. It's in the Wi-Fi copy too.

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

## Build and flash

Every board gets the same firmware. What it does is chosen afterwards and saved on
the board, so turning a node into a master (or back) never needs a reflash.

```
pio run -e c5 -t upload       # any C5 board (DevKitC-1 today)
pio run -e v8 -t upload       # Marauder V8 (PSRAM build, defaults to master)
pio run -e classic -t upload  # original ESP32 (WROOM-32 DevKit): Classic Bluetooth node
pio run -e cyd -t upload      # CYD / ESP32-32E: Classic node + touch dashboard
pio run -e heltec -t upload   # Heltec Wireless Tracker: LoRa node (Meshtastic, MeshCore, LoRaWAN)
make -C test/host             # host tests
python3 tools/test_ssproto.py # Python vs C++, byte for byte
```

The ESP32-C5-DevKitC-1 has two USB-C ports. The build uses the chip's **native USB**
port (`303a:1001`, `/dev/ttyACM0`) for both flashing and serial, so one cable does
everything and no BOOT/RST buttons are needed.

## Roles

| Role | What it does |
|---|---|
| **standalone** | Everything on one board: hops all 38 channels, keeps the device list, feeds the web UI. Use this with a single C5. |
| **master** | The Core. Pairs nodes over ESP-NOW, assigns work, merges their sightings. Never leaves channel 6 (it has to hear the nodes), so its own radio listens to BLE at 30% plus Wi-Fi on channel 6, as slot 0. `local off` turns that off. |
| **node** | A sensor. Listens, batches, reports to the master. Keeps no device list. |

A freshly flashed board runs as standalone and reports `"setup":false` until a role
is chosen, which makes the web UI open its setup screen. Change it later from
**SETTINGS** in the web UI, or over serial:

```
role standalone | role master | role node      # saves and reboots
setup master mykey                             # role + cluster key in one reboot
setup reset                                    # show the setup screen again
factory                                        # erase all settings
```

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

The radio trade-off: an access point has to stay on one channel (6). On a **master**
that costs nothing, because a master already stays on channel 6 to hear its nodes. On
a **standalone** board, Wi-Fi scanning drops from 38 channels to channel 6 only and
BLE is capped at 50% so the phone gets air time. BLE gear (trackers, body cams,
phones, earbuds, watches) is still covered. For full Wi-Fi coverage with the AP on,
add a node.

The copy of the page on a website (HTTPS) can use USB but not a board's Wi-Fi:
browsers block a secure page from talking to a plain local address. Use the copy the
board serves.

## Pairing

Nodes join a master only after you pair them, so strangers' boards can't join your
cluster and yours won't join theirs.

**Doing it:** on the master's web page, CLUSTER > **PAIR A NODE** (also in SETTINGS),
then follow the steps in that dialog:

1. Get the node ready. A new board (or one just updated from older firmware) waits
   on its own. A board paired elsewhere: hold **BOOT** 5 s (C5 DevKit GPIO 28, Heltec
   and ESP32 GPIO 0), or on a CYD SETTINGS > *Pair with a master* (tap twice), or
   `pair` on its serial console. Waiting boards blink their LED blue slowly or say
   NOT PAIRED YET on their screen.
2. Bring it within arm's reach of the master and press START PAIRING (2 minutes).
   Holding BOOT 5 s on a master opens pairing too.
3. Boards with a screen show a 6-digit code that must match the master's list;
   screenless ones flash their LED white on IDENTIFY.
4. ACCEPT. The node saves the key and the master's address, restarts and joins.

FORGET on a node's card unpairs it (it drops the key). On the master, clearing the
cluster key (SETTINGS > Advanced) makes a new one: every node must be paired again.

**How it works** (`lib/sscore/ss_pair.*`, `ss_crypto.*`, `src/common/pairing.*`):

- A master makes a random 128-bit cluster key (32 hex characters) the first time it
  starts. A node without a key does nothing but send `MSG_PAIR_REQ` (its MAC, board
  type, and a fresh X25519 public key) every 1.5 s on channel 6.
- During the window the master lists requests. Once one is heard at -55 dBm or
  stronger (arm's reach), it answers with `MSG_PAIR_OFFER` (its own fresh X25519 key,
  new for every window). Both sides compute the shared secret, derive a session key
  `k = HMAC(shared, label | node_pub | master_pub)` and a code `HMAC(k, "code") mod 10^6`.
  Someone in the middle substituting keys gets different codes on the two sides.
- ACCEPT sends `MSG_PAIR_KEY`: the cluster key XORed with `HMAC(k, "seal")`, tagged with
  a 128-bit `HMAC(k, target | len | sealed)`. The node checks the tag (and that it's
  addressed to it), saves the key and pins the master's MAC.
- After that, HELLO and WELCOME are broadcast with an HMAC tag from the cluster key
  (the master can't decrypt a node it hasn't added yet), and all unicast traffic is
  ESP-NOW CCMP-encrypted with keys derived from the cluster key. The radio allows 7
  encrypted peers, so a master takes up to 7 nodes.
- X25519, SHA-256 and HMAC are in `ss_crypto` (TweetNaCl's field arithmetic), host
  tested against RFC 7748, FIPS 180 and RFC 4231 vectors and against OpenSSL; the
  handshake, sealing, tampering and a key-swapping attacker are tested in `test/host`.

Typing the same key on boards by hand (`key <text>`, SETTINGS > Advanced) still works.

## How work is split

The Core turns the enabled categories into a scan plan (same rules as the
Categories screen) and assigns roles:

- **One node:** does everything.
- **Split mode (default):** with both radios needed, one node goes BLE full time
  (preferring one without 5 GHz), the rest share Wi-Fi channels. 2.4 GHz is split
  across all Wi-Fi nodes, 5 GHz only across 5 GHz-capable ones.
- **Mirror mode:** every node runs the same plan, so the same device is heard by
  all of them and signal strength can be compared for sector direction.
- **BLE-only categories** (e.g. body cams only): Wi-Fi hopping turns off and every
  node scans BLE 100%.
- **EVERYTHING** (a node's JOB, CLUSTER → EDIT): that node scans the whole plan on its
  own, outside the split; the others share the work as if it weren't there. For a node in
  another room or out at the edge.

**Sharing one radio between Wi-Fi and Bluetooth.** An ESP32 has one radio, so a board
doing both gives Bluetooth a share (e.g. 35%). On the S3 (Heltec) and classic ESP32 that's
the usual BLE scan window (35 ms of every 100). **The ESP32-C5 hears almost nothing that
way** (2 to 3% of full-time, measured on two boards with `bletest`), so a C5 gives
Bluetooth its share as blocks of time instead: full-time listening for 3.5 s of every 10,
with Wi-Fi parked on channel 6, then Wi-Fi hops with the radio to itself. A C5 master gets
1.5 s of every 10, since node frames that arrive during its Bluetooth go unanswered.

Nodes hop channels, then every 5 seconds (a dashboard CYD: every second) return to
channel 6, send batched sightings (9 per frame, repeats collapsed; sooner if the batch
fills), names, flood events, and a heartbeat every 5 s. Each heartbeat gets the CONFIG
back, which doubles as the keep-alive.

## Serial commands

Every role: `role <r>` · `setup <r> [key]` · `setup done|reset` · `key <text>|-` ·
`wifi on|off <password|-> <name>` (standalone and master) ·
`status` · `reboot` · `factory` · `json on|off` · `ui on|off`
Standalone and master: `cats all` · `cats surv` · `cats <hex>` · `devices` · `aps <id>`
Master: `mode split|mirror` · `local on|off`. Node: `core <mac>|-` (pin to one master).

Category bits: flock (ALPR) 0x2, bodycam 0x4, tracker 0x8, glasses 0x10, drone 0x20,
skimmer 0x40, pentest 0x80, phone 0x100, audio 0x200, wearable 0x400, other_ble
0x800, other_wifi 0x1000, lora 0x2000. Example: body cams + trackers only = `cats c`.

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
`{"t":"event",...}` for deauth/disassoc floods, `{"t":"node",...}` on join/loss.

## Address rotation (fingerprinting)

Each sighting carries a 32-bit fingerprint built from what a device model always
sends the same way: BLE advertising structure (AD types and lengths, company ID,
Apple message type, service UUIDs) or Wi-Fi probe-request capabilities (rates, HT,
VHT, HE, extended capabilities, vendor element IDs). SSIDs, names and rolling bytes
are excluded. The Core links a new random address to an old one when the old one has
gone quiet, the hand-off happened within a few seconds, the fingerprint matches,
and the RSSI is close. If two candidates are equally close it does **not** merge.

## Signature notes

Flock/Raven data comes from the original SkySentinel `flock.h` (credits kept in
`ss_sigdata.h`). Three changes, flagged for review:

- **00:25:DF** was in the Flock BLE list; it is Axon/TASER's OUI, so it now maps to Body cams.
- **A4:CF:12** and **3C:71:BF** in the Flock Wi-Fi list are Espressif chip OUIs (the
  chips SkySentinel itself runs on). Kept but scored as weak vendor matches. The full
  lists should be checked against the IEEE registry for other generic chip vendors.
- OUI rules now apply only to globally unique addresses. A random address's first
  bytes are random, so matching them against an OUI list causes false hits.

Raven needs two of its version-specific UUIDs (0x3100–0x3500, exact values) for a
strong match; a single one is weak, and the standard SIG UUIDs are not used at all.

### Body cams, glasses, drones (October 2026)

Built from public data: the IEEE OUI registry, the Bluetooth SIG company and service
UUID lists, the Nearby Glasses app, and the DJI DroneID research. Every OUI was checked
against IEEE before it went in. Details and credits are in `ss_sigdata.h`.

| Category | Strong (alerts) | Weaker (noted) |
|---|---|---|
| Body cams | Axon name, Axon / TASER service UUIDs (FC81, FE6B, FE6C), TASER maker ID 0x034D | OUIs: Axon, Digital Ally, WatchGuard Video, Utility (BodyWorn), Zepcam. Motorola Solutions maker ID, UUIDs and OUIs (police radios too) |
| Glasses | Luxottica maker ID 0x0D53 and OUI 98:59:49 (Ray-Ban / Oakley Meta), Snap 0x03C2 / FE45, Meta 0x058E / FD5F (also Quest headsets), Vuzix 0x060C | Meta Platforms 0x01AB, Meta and Vuzix OUIs |
| Drones | Remote ID (BLE and Wi-Fi), DJI DroneID vendor element 26:37:12 | DJI maker ID 0x08AA (also Osmo cameras), DJI, Parrot and Skydio OUIs, drone hotspot names (TELLO-, DJI-, Mavic, Anafi, Bebop, Skydio) |
| ALPR | Flock Safety's own OUI B4:1E:52 | Ring OUIs (weak, never alert alone) |

The "Flock BLE OUI" list from ouispy-detector turned out to be mostly other makers when
run through IEEE: Ring, DJI, Parrot, Skydio, Luxottica, Apple and HPE. Those moved to
their real categories; Apple and HPE were dropped. Eight unregistered entries stay as
"Flock-listed BLE OUI (unverified)".

Checked and left out: D4:BB:E6, 3C:61:05 and 00:0E:58, published elsewhere as Flock or
Vigilant OUIs, belong to Huawei, Espressif and Sonos. 00:01:21 and 00:90:7F are
WatchGuard Technologies (firewalls), not WatchGuard Video.

## Device details: what the radios give away

A device's detail window shows everything its broadcasts say openly:

- **Bluetooth**: AirPods / Beats battery levels (left, right, case, charging, in ear),
  Apple activity (on a call, driving, playing audio) and nearby actions, Find My status,
  iBeacon and Eddystone IDs and URLs, Google Fast Pair, thermometers and BTHome sensors.
  ASK connects for a few seconds and reads name, maker, model, firmware and battery.
- **Wi-Fi access points**: security (open, WEP, WPA2, WPA3, Enterprise, Enhanced Open),
  Wi-Fi generation (4 to 7) and channel width, devices connected and how busy the channel
  is (when the router reports it), and the make / model from its WPS setup info
  ("NETGEAR R7000"). Nodes send these once per router every few minutes, with its name.
- **Networks phones look for** (opt-in, SETTINGS): phones call out by name for networks
  they joined before. Off by default; when on, the master keeps them in memory only
  (never in the log or on an SD card), and turning it off forgets them. Serial:
  `probes on|off`, `info <id>`.

## Connected devices, vehicles, makers

- **Devices already on a network.** Phones and tablets that are connected to Wi-Fi
  rarely send probe requests, so the C5 / V8 / Heltec also listen to Wi-Fi data frames
  (a client talking to its router; at most one report per client every 3 s). A client
  with a private address (iOS, Android, GrapheneOS: per-network or per-connection
  random) counts as a phone, tablet or laptop; a fixed address is named by its maker
  when it's a phone or tablet maker (IEEE registry, `tools/update_oui_makers.py`), and
  is otherwise "Wi-Fi device, connected". Details show **CONNECTED TO** (the router),
  and a router's details list the devices heard talking to it. The CYD skips data
  frames (no RAM to spare).
- **Makers on Wi-Fi.** A fixed address from Apple, Samsung, Google, Motorola, OnePlus,
  Xiaomi, Huawei, Honor, OPPO, vivo, Realme, Sony, HMD, Nothing, Fairphone or Amazon
  fills in the MAKER column.
- **Vehicles** (new category, on by default): Tesla's phone-key Bluetooth ("S" + 16 hex
  + a letter), car makers' Wi-Fi hotspots (myChevrolet, Audi_MMI, MB Hotspot, VW
  WLAN...), car head units by name or Classic Bluetooth class (car audio), and OBD-II
  dongles. In Mobile mode a car that stays behind you raises a following alert.
- **Windows devices** say what they are in Microsoft's Connected Devices beacon:
  Windows laptop, desktop, tablet, Xbox, Surface Hub.
- **Matter devices in setup mode** show their vendor and product IDs.
- **Meshtastic:** name card, position, battery, and the last packet type (or "private
  channel", "direct message") when the contents can't be read.
- **MeshCore:** companions, repeaters, room servers and sensors from their adverts:
  name, kind, position (when shared) and how many repeater hops away.
- **LIVE DEVICES** shows the last 1 min, 5 min, 15 min or 1 hour (`list window <s>`).
- **Drone Remote ID, decoded** (Bluetooth and Wi-Fi, C5 / V8 / Heltec): the serial or
  registration number, aircraft type, where the drone is (map link), height, speed and
  heading, where the pilot's controller is and how far that is from the drone, and the
  self-ID text. All of it is public by FAA rule.
- **Rough distance** under SIGNAL, as a range (open air to indoors), using the device's
  own transmit power when it broadcasts it. Not shown for drones (their position is
  better) or LoRa (the signal may be a relay's).
- **Phone hotspots:** routers with a phone's default hotspot name ("Rob's iPhone",
  "Galaxy S24", "AndroidAP_1234", "Pixel_5678"), or an iPhone / iPad hotspot's Apple
  element on a private address, count as phones.
- **Wi-Fi beacon spam:** a burst of new networks at one signal level (10 random-address
  networks in 5 s within 8 dB, or 25 fixed-address) raises "Wi-Fi spam"; the fakes are
  marked spoofed, kept out of counts and the list, and evicted first when the table is full.
  The console's banner counts them up while the flood lasts.
- **Evil twin / Pineapple-style:** one BSSID answering as 3+ different network names
  within a minute (beacons or probe responses) raises "evil twin", once per 5 minutes per
  radio. Real routers give each network its own BSSID, so this holds whatever address the
  attacker uses. Needs the Pentest category on.
- **Router uptime** (UP FOR) from the beacon's own clock, which runs from the router's
  last restart. Some routers reset or randomise it.

## Deployment and following alerts

SETTINGS → Deployment (or `deploy stationary|mobile|portable`), saved on the board.
The V8 defaults to mobile, other boards to stationary.

- **Mobile** (vehicle) / **Portable** (on foot): about once a minute the board compares
  the Wi-Fi access points it heard with the ones at the last "place"; when most are
  new, that's a new place (header: `MOBILE · place 7`). Every device counts the places
  it was heard in. Alerts (once per device):
  - trackers: 3 places and 10 min (mobile) / 15 min (portable)
  - anything else: 5 places and 20 min / 30 min, so an unknown device riding along
    is caught too
- **Stationary**: surveillance gear (or a tracker) present for 20 minutes raises a
  "lingering" note.
- **THIS IS MINE** in a device's details silences it in that browser (fixed
  addresses permanently, rotating ones for the session), for your own phone, watch
  and earbuds.
- Open country with almost no Wi-Fi can't show movement, so it waits. Boards with GPS
  (Heltec, V8) will feed distance travelled instead (`FollowMonitor::notePlaceChange`).

The logic is in `lib/sscore/ss_follow.*` with simulated drives in the host tests
(tracker alerts once, passenger's phone after 20 min, passer-by never, parked = no
places, stationary lingering).

## How consumer devices are filed

In order, first match wins: specific signatures (AirPods / Beats, Fast Pair, Find My,
Garmin, Polar, Bose, Harman, Sony...), then what the device says it is in its own
advert (earbuds, headset, headphones, speaker, hearing aid, watch, fitness sensor,
phone, computer, keyboard/mouse), then LE Audio and heart-rate services, then makers
that only make one kind of thing:

- **Audio:** Beats, GN Hearing, Raycon, Sennheiser, Skullcandy, JLab
- **Wearables:** Garmin, Polar, Suunto, Amazfit / Zepp (Huami), Wahoo, Oura, Withings

**Phones** counts only what's very likely a phone: an iPhone's Nearby Info / Handoff
adverts, a device that calls itself a phone, and Wi-Fi probes from randomized
addresses (phones randomize; a phone that changes address mid-session is stitched back
together by its fingerprint where possible). Not phones any more: Wi-Fi probes from a
fixed address (printers, TVs, cameras, smart plugs, laptops; filed under Other Wi-Fi)
and Samsung / Google Bluetooth adverts from a fixed (public) address (TVs, appliances,
Chromecasts; Other Bluetooth). From a rotating random address they stay Phones.

A phone searching for Wi-Fi picks a new random address for each search burst, every 30 s
to a few minutes. The table joins a new address to the one that went quiet before it
when the fingerprint matches, the signal is close, and the gap is under 3 minutes
(Bluetooth rotations must hand off within 4 s). Two same-model phones at the same
signal are left apart rather than guessed.

Classic Bluetooth (most headphones while streaming) is invisible to the C5, whose
radio is BLE-only. See the Classic node below.

## Classic Bluetooth node (original ESP32)

Only the original ESP32 (ESP32-WROOM-32 / -32E: ESP32-DevKitC, generic "ESP32 DevKit"
boards, the CYD) has Classic Bluetooth; S2/S3/C3/C5/C6 are BLE-only. Flash it with
`pio run -e classic -t upload` (or FIRMWARE in the web UI) and set a C5 to
**master**. It's always a node:

- **Classic inquiry**, about every 15 s: finds devices that are *discoverable* (pairing
  mode, cars, many speakers, older headsets) with name, type from the Class of Device
  (headphones, headset, car audio, phone, watch, computer) and signal. HC-05 / HC-06
  serial modules, the usual gas-pump skimmer radio, are flagged as **Skimmers**.
  Inquiry transmits; the board itself stays non-discoverable.
- **2.4 GHz Wi-Fi** on the channels the master gives it. With a C5 node in the
  cluster, the ESP32 takes 2.4 GHz and the C5 takes 5 GHz + BLE.
- No BLE on this node (the C5s cover it).

It can't see Classic devices that are already paired and streaming: they don't answer
inquiries. That takes a passive sniffer (Ubertooth One), a candidate for the SBC master.

## CYD touch dashboard

`pio run -e cyd -t upload` (or **CYD** in FIRMWARE) turns a CYD into the Classic node
above with a 2.8" touch dashboard of what the master sees. Two boards work:

| | ESP32-2432S028 ("CYD") | LCDWiki ESP32-32E 2.8" (E32R28T) |
|---|---|---|
| Ports | micro-USB + USB-C (or micro-USB only) | USB-C, BOOT and RESET buttons, battery plug |
| Display | ILI9341 or ST7789 (2-port boards: ST7789) | ST7789 (some ILI9341) |
| RGB LED | red 4, green 16, blue 17 (low = on) | red 22, green 16, blue 17 |
| GPIO 34 | light sensor | battery voltage (shown in the header) |
| Speaker | GPIO 26 | GPIO 26, amp enable GPIO 4 (low = on) |

Display (CS 15, DC 2, SCK 14, MOSI 13, MISO 12, backlight 21) and touch (XPT2046 on
CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36) are the same on both.

**First start** walks through three screens: tap three crosses (touch calibration,
works whichever way the panel is wired), pick the board, then a color check with
*Flip dark/light*, *Swap red/blue* and *Upside down* before **LOOKS GOOD**. The display
chip is detected by reading its ID; if the screen stays white or garbled, hold
**BOOT** for 3 s to switch chip and restart. Serial: `cyd setup`,
`cyd driver auto|ili9341|st7789`, `cyd invert on|off|auto`.

**The dashboard** (`MSG_DASH`, sent by the master after each CONFIG, so about 1 Hz;
the CYD checks in every second):

- Header: the node's name, deployment, nodes online, battery. Tap for settings
  (deployment, beep, touch setup, display check, board, upside down).
- Banner: latest alert (red: Find My tag away from its owner, strong ALPR or body-cam
  match, following, lingering, deauth flood, BLE spam) or notice (amber: anything
  else in a surveillance category at 40%+). Tap to clear. Alerts blink the LED and beep.
- Tiles: ALPR, body cams, trackers, glasses, drones, skimmers, pentest, phones, heard in
  the last 30 s. Tap one to switch that category on or off for the whole cluster
  (`MSG_DASH_CMD`, same as CATEGORIES in the web UI).
- Closest: the five nearest surveillance devices, following first.

Tabs along the bottom switch screens (the CYD tells the master which one is open,
`DC_VIEW`, and the master sends that screen's data with each dashboard update,
`MSG_DASH_VIEW`):

- **DASH**: the dashboard above.
- **DEVICES**: the live device list, six to a page (arrows on the right), best match
  first like the web page. Tap a row for its details: address, maker, signal now and
  peak, confidence, when first and last heard, radio and channel, address changes,
  places it has been with you.
- **EVENTS**: alerts, notices and nodes joining or dropping out, newest first (the
  master keeps the last 30).
- **CLUSTER**: the master and every node: online, link, packet loss, uptime, battery,
  GPS, and a warning if it last restarted from a crash, a hang or a brownout.

**Night theme** (SETTINGS > Theme): red on black with the backlight dimmed, for the
truck at night.

NODE SCREENS OFF on the master darkens it; a tap wakes it for 30 s.

### GPS on a CYD (optional)

Any NMEA GPS module (ATGM336H, NEO-6M / M8N, BN-220...) works. It's receive only, so
just three wires go to the 3-pin expansion plug (ESP32-32E: GND, 3.3V, IO35; the
ESP32-2432S028's P3 has IO35 too):

| GPS pad | CYD |
|---|---|
| VCC | 3.3V |
| GND | GND |
| TXD | IO35 |
| RXD | not connected |

Follow the labels printed on both boards, not the wire colors. Don't use the 4-pin
serial plug: that's the USB serial port (IO1 / IO3), and the GPS would garble flashing
and the console. The CYD finds the baud rate by itself (9600, 115200, 38400), says
`[gps] GPS found` in its log, and from then on sends its position and clock to the master
like the Heltec does. Serial: `gps` (status), `gps forget` (module removed for good).

**GPS window:** click any node's **GPS** chip on the CLUSTER card for a sky plot of the
satellites (filled = used in the fix, colored by system: GPS, GLONASS, Galileo, BeiDou),
signal strength per satellite, position (copy, or open on OpenStreetMap), accuracy
estimate, altitude, speed and heading, and GPS time. GPS nodes send these details every
5 s (MSG_GPS); the master answers `gps <node id>`. The CYD's header shows `GPS 12`
(green, locked) or `GPS --` (searching), and the master posts "GPS lock" / "GPS lost its
fix" to the events list and the CYDs.

## LoRa node (Heltec Wireless Tracker)

`pio run -e heltec -t upload`. The Heltec Wireless Tracker is an ESP32-S3 with an
SX1262 LoRa radio and GNSS. It runs the same firmware as the C5 (Bluetooth LE and
2.4 GHz Wi-Fi, any role, defaults to **node**) plus a 915 MHz listener that never
transmits. One LoRa radio hears one setting at a time, so on **AUTO** (the default)
it takes turns:

| Slice | Listens for | Settings |
|---|---|---|
| 20 s | Meshtastic LongFast (US default) | 906.875 MHz, SF11, 250 kHz, sync 0x2B |
| 10 s | Meshtastic MediumFast | 913.125 MHz, SF9, 250 kHz |
| 10 s | MeshCore (US/Canada recommended) | 910.525 MHz, SF7, 62.5 kHz, CR 4/5, sync 0x12 |
| 10 s | LoRaWAN uplinks (sensors, trackers) | 903.9 to 905.3 MHz (US915 sub-band 2), SF7 to 10, activity scan |

Or pin it to one, to hear everything there: on the master's page CLUSTER > EDIT >
**LORA** (saved on the master, sent to the node), or `node <ID> lora <mode> [slot]` in
the console. A standalone or master Heltec takes `lora <mode> [slot]` directly (or
SETTINGS > THIS BOARD'S LORA RADIO). Modes: `auto`, the Meshtastic presets
`longfast`, `mediumfast`, `mediumslow`, `longslow`, `longmod`, `shortfast`,
`shortslow`, `shortturbo`, then `meshcore` and `lorawan`. For a Meshtastic preset,
`slot` is the frequency slot a mesh moved to (1-based, as the Meshtastic app shows
it); leave it out for the preset's usual one.

| Preset | Default slot | US frequency |
|---|---|---|
| LongFast | 20 | 906.875 MHz |
| MediumFast | 45 | 913.125 MHz |
| MediumSlow | 52 | 914.875 MHz |
| ShortFast | 68 | 918.875 MHz |
| ShortSlow | 75 | 920.625 MHz |
| ShortTurbo | 50 | 926.750 MHz (500 kHz) |
| LongSlow | 27 | 905.3125 MHz (125 kHz) |
| LongModerate | 6 | 902.6875 MHz (125 kHz) |

MeshCore nodes show by the first 6 bytes of their public key (MeshCore shows the
same prefix) once they send an **advert**: the name, kind (companion, repeater, room
server, sensor), position if the owner shares it, and repeater hops away. Repeaters
advert every few hours; companions when the owner sends one. Other MeshCore traffic
only carries 1-byte hashes, so it is counted (status, RADIO line) but not turned into
devices. The advert signature isn't checked.

Meshtastic nodes show as `!a1b2c3d4` with how many hops away they are ("direct" means
the signal strength is theirs; relayed means it's the relay's; "via internet" means it
arrived over MQTT and isn't nearby). LoRaWAN devices show their DevAddr, or their
permanent DevEUI when they join a network. Listening needs the **LoRa / Meshtastic**
category on.

On the public default channel (LongFast or MediumFast with the published default key,
which every Meshtastic radio ships with and anyone can read), the Heltec also decrypts
**node names** ("Rob's Truck (RTK)") and the **positions** nodes broadcast, shown with
a map link in the node's details. Text messages are recognised but never shown or
logged. Private channels and LoRaWAN data stay unreadable.

The preset frequencies come from Meshtastic's channel-name hash (the same
calculation gives LongFast's documented 906.875 MHz). LongFast and MediumFast are
tested on the air; the other presets and MeshCore are built from the protocols'
published settings. Pins follow Meshtastic's
variant for this board (SX1262 NSS 8, SCK 9, MOSI 10, MISO 11, RST 12, BUSY 13,
DIO1 14, TCXO 1.8 V).

## Sub-GHz node (LilyGO T-Embed CC1101), beta

`pio run -e tembed -t upload` (replaces Bruce or the factory demo). The C5's firmware
(BLE + 2.4 GHz Wi-Fi, defaults to **node**) plus:

- **Band watch** on the CC1101, receive only: 315, 345, 390, 433.92 and 915 MHz in turn,
  about 100 ms each (40 signal samples), with the antenna switch set per band. Per band:
  a noise floor (follows quiet down fast, loud up slowly), **bursts** (a fob press, a
  sensor, a remote: a sample 15 dB over the floor) and **jamming** (the whole band 20 dB
  over its floor for 3 s: `EV_RF_JAM`, alert on the master, `rf_jam` in the log, once a
  minute per band). Runs while the Pentest gear or Vehicles category is on.
- **Screen and jog wheel:** turn for pages: HOME (master link, scanning, battery,
  sub-GHz state), BANDS (level now, floor, peak, bursts, JAM) and RECENT (latest bursts).
  Pressing the wheel also flips pages; the back button turns the screen off and on.
  Pairing and IDENTIFY show full screen. The LED ring's first LED glows red while jammed.
- **Battery** from its BQ27220 fuel gauge.
- Next: tire-pressure sensor IDs (following alerts on a car), fob and remote IDs, replay
  and brute-force patterns. Pins: SPI 11 / 9 / 10 shared by screen (CS 41, DC 16, RST 40,
  BL 21), CC1101 (CS 12, GDO0 3, GDO2 38, antenna switch 47 / 48) and SD (CS 13); power
  enable 15; jog wheel 4 / 5, press 0, back 6; I2C 8 / 18.

## Long Range link (Espressif LR)

SETTINGS → MASTER → **Long Range link to nodes**, off by default. LR is Espressif's own
slow mode: the receiver can pick out a much weaker signal, so a node reaches several times
further, or through a wall that was too much. Use it for nodes that need the reach; at
normal distances the regular link is faster and has no known issues. Every board always
listens for LR; the setting switches what's *sent*:

- The master uses LR only with nodes whose heartbeat says they can (their CLUSTER card
  shows **LR**); the others stay on the normal link.
- A node told to use LR calls for a lost master in both modes, in turns, so turning the
  setting off (or a different master) never strands it. It remembers this across its own
  restarts, so a node out at the edge that restarts while the master is away still calls
  over LR; the master answers a joining node in both modes too.
- If the master's own radio can't do LR, the setting says so and stays off.

**Speed:** **500 kbit/s** (the default) or **250 kbit/s** (the farthest reach), under
Long Range speed in the same panel, or `link lr rate 500|250`. The master sends the choice
to every node with its orders. Serial: `link lr on|off`.

**Per node:** CLUSTER → EDIT → **LINK TO THE MASTER** (or `node <ID> link
auto|normal|lr250|lr500`) overrides this for one node: FOLLOW MASTER (the default),
NORMAL, LR 250 or LR 500. Mix them freely, e.g. the office nodes on NORMAL, a node across
the house on LR 500 and one out by the road on LR 250. The master sends each node at its
own rate; every board hears both LR speeds and normal frames.

**Known issue: brief automatic reconnects.** ESP-IDF's Wi-Fi driver sometimes drops an
outgoing ESP-NOW frame without reporting it ([esp-idf #19269](https://github.com/espressif/esp-idf/issues/19269),
open as of ESP-IDF 5.5.5 / Arduino-ESP32 3.3.12). Each one leaks a transmit buffer; after
four, the link refuses to send. Slow LR frames spend longer in the driver and hit it far
more often. Measured on a Heltec Wireless Tracker (ESP32-S3), with the node scanning
Bluetooth and Wi-Fi as normal:

| Link | Stuck-link events |
|---|---|
| Normal (LR off) | rare; see below |
| LR 500 kbit/s | about one every several hours |
| LR 250 kbit/s | one every 10 to 45 minutes |

The normal link isn't immune. On the Heltec it depended on whether Bluetooth was scanning
while the node sent: about one an hour with Bluetooth running during sends, about one every
5 hours when it paused for the whole reply window (which cost most of its Bluetooth). Since
2.1, Bluetooth steps aside only while the frames leave (a few ms) and nodes report every
5 s: about one a day or less.

A node notices within 1.5 s (frames handed to the radio that never finish) and restarts
itself; it's back in the cluster a few seconds later, and its log says
`[link] N frames stuck in the Wi-Fi driver ... restarting`. Restarting only ESP-NOW, or even
the whole Wi-Fi driver, was tried and didn't recover cleanly (the refusals continued and
memory leaked), so a quick restart it is, until Espressif fixes the driver. Nodes also never
change Wi-Fi channel while frames to the master are still in flight: leaving with LR
frames waiting made the problem several times worse. The card shows the restart as
**last start: stuck_tx**.

Classic ESP32 boards (CYD, DevKit) get it too, more slowly: each lost frame keeps a slot,
and a CYD went a day losing more and more of its sends (9% loss) without ever restarting.
Since 2.1 they restart after 5 s of stuck frames (longer than the C5/S3's 1.5 s, because
Classic Bluetooth inquiry can hold the radio for over a second), and their status shows
`in_flight`.

**Which link for which node:** NORMAL for anything near the master, LR only for nodes
that need the reach. LR gains a nearby node nothing.

## Cluster view and node screens

Boards with GPS (the Heltec) show their lock on their CLUSTER card: **GPS 3D · 9 sats**
(green), **GPS searching** (amber, no fix yet; give it sky and a few minutes after a cold
start) or **GPS no data** (red, the receiver isn't talking). The Heltec's screen shows the
same on its second line (`GPS 9`, `GPS --`, `GPS off`). While a board is plugged into a
computer it shows **USB power** instead of a battery percentage: the battery pin then reads
the charger's output (about 4.1 V, battery or not), so any percentage would be made up.
On a phone charger with no battery fitted it can't tell, and may show a high percentage.

On a master the web UI shows a **CLUSTER** panel: the master plus one card per node with
its board type, ONLINE / GETTING ORDERS / OFFLINE, its current job (Bluetooth duty,
Wi-Fi channels, LoRa, Classic), link signal, last heard, packets and loss, uptime,
counts and battery.

The Heltec's 0.96" screen shows SKYSENTINEL, the role and battery, the master it's
linked to with link signal (OK once it has its orders), what it's scanning, LoRa
status and packets, and sightings sent and uptime. SETTINGS → Master → **Node
screens** turns every node's screen and backlight off or on (sent with the master's
orders; `screens on|off`). A Heltec running standalone or as master has its own
`screen on|off`. Pins come from Meshtastic's variant for the V1.1 board (ST7735S CS 38,
DC 40, MOSI 42, SCK 41, RST 39, backlight 21, offsets 26/1); if colors come up
inverted or the picture is shifted, `SS_TFT_INVERT` and `SS_TFT_COL_OFFSET` /
`SS_TFT_ROW_OFFSET` in `src/common/node_display.cpp` fix it.

### Editing nodes from the master

**EDIT** on a node's card (or `node <ID> ...` on the master's console):

| Setting | Command | What it does |
|---|---|---|
| Name | `node ACDC label Truck rear` (`-` clears) | Shown on its card and its own screen |
| Job | `node ACDC role auto\|ble\|wifi\|both\|all` | Pin its job; AUTO lets the master decide. EVERYTHING (`all`): scans every channel and Bluetooth on its own; the other nodes share the work as if it weren't there. For a node in another room or out at the edge of range |
| Link | `node ACDC link auto\|normal\|lr250\|lr500` | Its link to the master (see Long Range); `auto` follows the master's setting |
| LoRa | `node ACDC lora <mode> [slot]` | What a LoRa board listens to (see LoRa node) |
| Screen | `node ACDC screen on\|off\|auto` | Overrides the master's node-screens setting |
| Identify | `node ACDC identify` | Screen flashes THIS ONE (C5: RGB LED blinks) for 10 s |
| Reboot | `node ACDC reboot` | Restarts it |
| Forget | `node ACDC forget` | Removes it from the list and forgets its settings |

The master keeps these by node ID (they survive reboots on both sides) and sends the
name with every CONFIG, so a node that rejoins gets it back.

### Which node hears it

With nodes in different rooms (or out at the edge), the strongest one is nearest the
device. The device list's **NODE** column shows the board that hears it best right now
and its signal ("Remote C5 (-62) +2" = two more boards hear it too); new-device events
say "heard by ..."; a device's details list every board that has heard it, strongest
first, with how long ago.

## Detection log

The master records what it found, when and where:

- **What:** every surveillance-category device when first seen (or when it becomes a
  confident match), following and lingering alerts, deauth / disassoc floods and BLE spam.
  Phones, earbuds and other consumer gear are not logged.
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
`deauth_flood`, `disassoc_flood`, `ble_spam` or `track`. Google My Maps imports it
directly (choose lat / lon as the position columns).

Serial: `log` (status), `log dump`, `log clear`, `log on|off`; on a CYD, `sd`, `sd ls`,
`sd get <file>`.

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

## Website (skysentinel.neyhard.com)

`site/` is the public site: home page with the supported-board grid and comparison
table, an installer (pick a board, update or fresh install, flash over Web Serial with
esptool-js), how-tos, and the console itself under `app/`. Every page warns when the
browser can't do USB (Safari, Firefox before 151, phones) and points to the Wi-Fi console instead.

    pio run -e c5 -e v8 -e heltec -e cyd -e classic
    python3 tools/build_site.py --version 1.0
    -> dist/site/ (upload its contents) and dist/site.zip

It's all static files, nothing loaded from other sites. The server needs HTTPS (Web
Serial only works on secure pages). `downloads/` gets a source archive and SHA256SUMS
with every build, so the code is published even without a public git host. Board
list: `site/assets/boards.js`.

## Reserved for later

- `KIND_BT_CLASSIC`: a classic-ESP32 (CYD) node for Classic Bluetooth skimmers
- Heltec GNSS: use the position for following alerts (distance traveled) and logs
- COBS serial transport (already implemented and tested in `ss_proto` and
  `ssproto.py`) for USB-hub clusters and an SBC master

## License

Copyright 2026 Robert Neyhard. SkySentinel is free for non-commercial use under the
PolyForm Noncommercial License 1.0.0 (`LICENSE`, SPDX `PolyForm-Noncommercial-1.0.0`):
use it, study it, change it, share it. Selling it or using it in a business needs
written permission. The libraries it uses keep their own licenses (`THIRD_PARTY.md`).
