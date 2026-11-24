# Board guides

What each kind of board adds and how to set it up. Back to the [README](../README.md).

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

## Sub-GHz node (LilyGO T-Embed CC1101)

`pio run -e tembed -t upload` (replaces Bruce or the factory demo). The C5's firmware
(BLE + 2.4 GHz Wi-Fi, defaults to **node**) plus:

- **Band watch** on the CC1101, receive only: 314.35 (Toyota / Lexus smart keys), 315,
  345, 390, 433.92 and 915 MHz in turn, about 100 ms each (40 signal samples), with the antenna switch set per band. Per band:
  a noise floor (follows quiet down fast, loud up slowly), **bursts** (a fob press, a
  sensor, a remote: a sample 15 dB over the floor) and **jamming** (the whole band 20 dB
  over its floor for 3 s: `EV_RF_JAM`, alert on the master, `rf_jam` in the log, once a
  minute per band). Runs while the Sub-GHz, Pentest gear or Vehicles category is on.
- **Tire pressure sensors (TPMS):** after each sweep the CC1101 listens on 315 MHz
  (433.92 in Europe: SETTINGS > Tire sensors) for about 0.7 s in FSK and 0.5 s in OOK.
  Its carrier sense (GDO2) starts a capture of the demodulated data (GDO0); each capture
  is decoded for Toyota / Lexus (Pacific PMV-C210), Ford, Hyundai / Kia (Continental
  VDO), Jeep / Chrysler (Continental), Hyundai Elantra / Honda Civic (TRW), Kia, Honda
  (TRW PPA-GF33), GM (Schrader EG53MA4) and other Schrader sensors. Each tire is a device
  in **Vehicles** with its fixed ID, maker and pressure ("Toyota tire 1A2B3C4D 34 psi"),
  so the usual following check catches a car that goes where you go. Sensors send about
  once a minute while the car moves (a parked car rarely). Your own car's four tires
  will "follow" you too: press THIS IS MINE on each. The HOME page shows
  `tires decoded/heard`. Wire formats from the rtl_433 project's captures; this decoder
  is our own. **Not yet tried on hardware.**
- **Boot screen:** the logo and what started (radios, CC1101, battery, pairing, build),
  for 3 s or until a button is touched.
- **Screen and jog wheel:** turn for six pages:
  - **HOME:** master link, scanning, battery, sub-GHz state, the cluster's latest alert.
  - **ALERTS:** the cluster's latest alert, what's of concern nearby (FOLLOWING in red)
    and the recent events, from the master with every report (about every 5 s; the node
    asks for them in its heartbeat).
  - **BANDS:** level now, floor, peak, bursts, JAM.
  - **HISTORY:** the last 10 minutes of each band, a bar per 10 s (how far its loudest
    moment rose over the quiet level), burst slots in color.
  - **RECENT:** the latest bursts.
  - **FOXHUNT:** press the wheel to pick a target, a sub-GHz band or a Bluetooth device this
    board hears itself (surveillance and trackers first), then walk toward it: one big
    signal number, a bar with a 3 s peak hold, and the LED ring as a meter (more LEDs,
    blue to red, as it gets louder). A band is watched alone, about 10 readings a second;
    for a Bluetooth device the board listens full-time during a foxhunt. BACK stops the foxhunt.
  Pressing the wheel also flips pages; a press of BACK turns the screen off and on.
- **POWER menu:** hold BACK 2 s. Turn the wheel to choose, press it to do it, BACK
  cancels. The same choices as Bruce: **Screen off** (keeps scanning), **Deep sleep**
  (radios, LEDs and screen off; BACK wakes it), **Power off** (the BQ25896 charger
  disconnects the battery, a true off; plugging in USB turns it on; on USB power it says
  so and suggests Deep sleep) and **Restart**. After Deep sleep or Power off the master
  shows the node as lost until it comes back.
- **SETTINGS menu:** hold the wheel 1 s. Press the wheel to step a setting to its next
  value (saved on the board right away), BACK when done: theme (**Day**, or **Night**: red
  on black like the CYD's, with a dimmer screen and LED ring), brightness (its own for
  each theme), screen off after
  (never / 1 / 5 / 15 min; the first touch then only wakes it), start page, turn the
  screen around (also `display flip`), LED ring alerts on / off and brightness, the
  color for each alert (jamming, cluster alert, sub-GHz burst, lost master; or Off), how long
  a cluster alert flashes (10 s, 30 s, 1 min, 5 min, or until a button; any button stops it), what counts as a
  burst (louder than 10 / 15 / 20 / 25 / 30 dB over the band's quiet level, default 30, and
  longer than 2.5 / 7.5 / 15 / 20 / 30 ms, default 15; louder and longer = fewer false bursts), which bands to
  watch, whether jamming is sent to the master, and tire sensors (315 MHz / 433.92 / Off).
- **On the web console:** every burst goes to the master as a row in LIVE DEVICES, one
  row per band ("Sub-GHz bursts: Ford fobs, tire sensors", ADDRESS 315 MHz): HITS counts
  the bursts, SIGNAL is the latest one's, NODE says which T-Embed heard it. They're in
  their own **Sub-GHz** category (CATEGORIES; on by default) and the radio filter next
  to the search box shows only them (or only Wi-Fi, Bluetooth LE, Classic BT or LoRa).
  A row is a band, not a device: the bursts carry no ID until they're decoded. Sub-GHz
  rows never raise "following" alerts.
- **LED ring = alert light:** jamming lights it solid (red by default) while it lasts, a
  new alert from the cluster pulses it (purple; for as long as Alert flash says), a sub-GHz burst flashes it
  (blue), and while it looks for its master it blinks slowly (yellow). Picking a color in
  SETTINGS shows it on the ring for a moment.
- **Battery** from its BQ27220 fuel gauge.
- Next: fob and remote IDs, replay and brute-force patterns. Pins: SPI 11 / 9 / 10 shared by screen (CS 41, DC 16, RST 40,
  BL 21), CC1101 (CS 12, GDO0 3, GDO2 38, antenna switch 47 / 48) and SD (CS 13); power
  enable 15; jog wheel 4 / 5, press 0, back 6; I2C 8 / 18.

