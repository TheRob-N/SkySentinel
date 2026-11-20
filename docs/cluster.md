# Running a cluster

Roles, pairing, how the master shares out the work, the link between boards, and the
cluster view. Back to the [README](../README.md).

## Roles

| Role | What it does |
|---|---|
| **standalone** | Everything on one board: hops all 38 channels, keeps the device list, feeds the web UI. Use this with a single C5. |
| **master** | The Core. Pairs nodes over ESP-NOW, assigns work, merges their sightings. Never leaves the link channel (it has to hear the nodes), so its own radio listens to Bluetooth (a C5 master in short blocks, 15% of the time) plus Wi-Fi on the link channel, as slot 0. `local off` turns that off. |
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
  type, and a fresh X25519 public key) every 1.5 s on the link channel.
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
with Wi-Fi parked on the link channel, then Wi-Fi hops with the radio to itself. A C5 master gets
1.5 s of every 10, since node frames that arrive during its Bluetooth go unanswered.

Nodes hop channels, then every 5 seconds (a dashboard CYD: every second) return to
the link channel, send batched sightings (9 per frame, repeats collapsed; sooner if the batch
fills), names, flood events, and a heartbeat every 5 s. Each heartbeat gets the CONFIG
back, which doubles as the keep-alive.

## Link channel (1, 6 or 11, chosen by the master)

The whole cluster talks on one 2.4 GHz channel: the master's own Wi-Fi page and every
node's reports. Only 1, 6 and 11 are used, since they don't overlap. A strong network on
an overlapping channel (a hotspot on 7 next to a link on 6) hurts the most: the radios
can't decode it, so they just find the air busy and lose frames.

- **Automatic (default).** Once a minute the master adds up what the nodes hear on and
  around each candidate (strength, overlap, how busy routers say they are) and averages
  that over about an hour. It moves only when another channel is clearly better three
  times in a row, 10 minutes apart; the first decision comes 10 minutes after start, and
  after a move it stays put for 6 hours. It also remembers how the link actually did on
  each channel and won't go back to one where it lost more.
- **Manual.** LINK CH in the console (or `linkch 1|6|11`) pins a channel; AUTO hands it
  back; SURVEY NOW decides right away (`linkch auto`, `linkch survey`).
- **Nodes follow.** The master tells every node with its orders. A node that loses the
  master looks for it on its saved channel first, then the other two.

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

