# What it detects, and how

Signatures and where they came from, what each radio gives away, following alerts and
how devices are filed. Back to the [README](../README.md).

## Signature notes

Per-category details and sources follow.

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
| Glasses | Luxottica maker ID 0x0D53 and OUI 98:59:49 (Ray-Ban / Oakley Meta; surest with a Meta service UUID alongside), Snap 0x03C2 / FE45, Meta 0x058E / FD5F (also Quest headsets), Vuzix 0x060C | Meta Platforms 0x01AB, Facebook service UUIDs FEB7 / FEB8, Meta and Vuzix OUIs |
| Drones | Remote ID (BLE and Wi-Fi), DJI DroneID vendor element 26:37:12 | DJI maker ID 0x08AA (also Osmo cameras), DJI, Parrot and Skydio OUIs, drone hotspot names (TELLO-, DJI-, Mavic, Anafi, Bebop, Skydio) |
| ALPR | Flock Safety's own OUI B4:1E:52, Flock accessory BLE service e8ccbb38-9532-46a8-9fe5-1814df172e6f | Genetec (AutoVu ALPR maker) OUIs (weak, never alert alone) |
| Pentest | Flipper Zero by any of: its case-color service UUID (0x3080–0x3083), Flipper Devices maker ID 0x0E29, OUI 0C:FA:22 (public address), the "Flipper " name. Two of them together (or with its appearance 0x8600) is surer still | |
| Cameras | | Camera makers by OUI, named in the details: Ring, Hikvision, Verkada, Wyze, Arlo, Axis, Blink. Their own category since 2.1.6 (weak, never alert alone), so neighbors' doorbells and NVRs don't fill the ALPR count. Not counted as surveillance |

### From Dragon Detector (November 2026)

The Flock accessory UUID, the Flipper identifiers, the camera-maker list, Meta FEB7 / FEB8
and Apple's Find My accessory service (FD44, a weak tracker hint) come from
[Dragon Detector](https://github.com/valleytechsolutions/Dragon-Detector) by Valley Tech
Solutions. Identifiers only (no code); the camera OUIs were pulled fresh from the IEEE
registry. 00:0A:B1 belongs to GENETEC Corporation of Japan, not Genetec Inc., and is left
out. Avigilon has no OUI of its own. Its "Biscuit" UUID (4fafc201-...) is the stock ESP32
Arduino BLE example's, so it would flag every hobby board and is left out.

The "Flock BLE OUI" list from ouispy-detector turned out to be mostly other makers when
run through IEEE: Ring, DJI, Parrot, Skydio, Luxottica, Apple and HPE. Those moved to
their real categories; Apple and HPE were dropped. Eight unregistered entries stay as
"Flock-listed BLE OUI (unverified)".

Checked and left out: D4:BB:E6, 3C:61:05 and 00:0E:58, published elsewhere as Flock or
Vigilant OUIs, belong to Huawei, Espressif and Sonos. 00:01:21 and 00:90:7F are
WatchGuard Technologies (firewalls), not WatchGuard Video.

## More detection sources (2.1.6)

- **Wi-Fi Direct and Apple AWDL.** Every Wi-Fi node now reads two kinds of action frame
  it used to skip. AWDL (Apple's vendor action frame, 00:17:F2 type 8) is how iPhones,
  iPads and Macs do AirDrop, AirPlay and Sidecar: filed as Phones, "Apple device using
  AWDL". Its address is private and changes. Wi-Fi Direct (the WFA P2P element, or a P2P
  public action frame) is printers, TVs, phones and cameras sharing straight to each
  other: filed as Other Wi-Fi, often with a "DIRECT-xx-..." network name.
- **More Bluetooth makers** (company ids from the Bluetooth SIG list): GoPro (also its
  service UUIDs 0xFEA5 / 0xFEA6) and Wyze / Arlo as Cameras; GM, VW, Mercedes, BMW,
  Ford, Hyundai, Honda, Rivian, Toyota, Subaru, Nissan, Harley-Davidson and Polaris as
  Vehicles (phone-as-key, app links); Segway-Ninebot, VanMoof and Shimano as e-bikes and
  scooters (Vehicles); Plantronics / Poly, GN Hearing and Sonos as Audio; Amazon as
  Other BLE.
- **Smart home mesh (Zigbee, Thread / Matter).** The C5's radio has an 802.15.4 receiver.
  With the Smart home mesh category on, each C5 node (never the master, which keeps the
  link) lends it the radio for 1.5 s of every 15 s, one channel (11 to 26) per block:
  every other block sweeps on, the rest go back to the busiest channel so far. Receive
  only. Zigbee is told by its readable network header (protocol version 2, often the
  sender's 64-bit IEEE address) and coordinator beacons; Thread by its 6LoWPAN headers
  and MAC-layer encryption (Zigbee never encrypts there). Devices are keyed by their
  IEEE address (maker block + last 3 bytes) or, when only a short address was sent, by
  network + short address (flagged as changeable). The name shows the network: "Zigbee
  PAN 1A2B". **Off by default:** it takes 10% of the radio from Wi-Fi and Bluetooth on
  every C5 node, and it hasn't been run on hardware yet. A node's status line shows
  `zb` (blocks, frames, reported, dropped, channel).
- **GPS jamming and spoofing** (Heltec Wireless Tracker, its own GPS). Once a second it
  looks at the satellites. *Jamming:* after learning the normal sky for a minute or
  more, the four strongest satellites fall 10 dB or more under normal (or nearly all
  vanish) for 10 s while the receiver keeps talking. *Spoofing:* the position jumps
  faster than 1,200 km/h, the GPS clock jumps more than 5 s against the board's own, six
  or more satellites arrive within 3 dB of each other (one transmitter), or they're
  stronger than orbit allows (56 dB-Hz and up). Each raises a red banner and goes in the
  detection log (`gps_jam`, `gps_spoof`); one of each kind per 10 minutes. A garage,
  tunnel or heavy roof also hides the sky, so a parked board is the reliable judge; a
  careful spoofer that takes the receiver over slowly won't trip it.

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
- **Vehicles** (on by default): Tesla's phone-key Bluetooth ("S" + 16 hex
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

- **Audio:** Beats, GN Hearing, Raycon, Sennheiser, Skullcandy, JLab, Poly / Plantronics, Sonos
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

## Address rotation (fingerprinting)

Each sighting carries a 32-bit fingerprint built from what a device model always
sends the same way: BLE advertising structure (AD types and lengths, company ID,
Apple message type, service UUIDs) or Wi-Fi probe-request capabilities (rates, HT,
VHT, HE, extended capabilities, vendor element IDs). SSIDs, names and rolling bytes
are excluded. The Core links a new random address to an old one when the old one has
gone quiet, the hand-off happened within a few seconds, the fingerprint matches,
and the RSSI is close. If two candidates are equally close it does **not** merge.

