// SkySentinel wire protocol (v1)
//
// One frame format for every link: ESP-NOW between C5 nodes and the Core today,
// COBS-framed USB/UART serial to a single-board-computer master later.
// Everything is little-endian and serialized field by field (never by casting
// packed structs), so the same bytes decode identically on an ESP32, a Pi, or
// in Python (tools/ssproto.py).
//
// Frame layout (max 250 bytes, the ESP-NOW v1 payload limit):
//   u16 magic 0x5353 ("SS") | u8 version | u8 type | u16 node_id | u16 seq
//   payload (0..240 bytes)
//   u16 crc16-ccitt over header + payload
//
// Pure C++ (no Arduino / ESP-IDF headers) so it builds on the host for tests
// and on a Linux SBC unchanged.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ss {

constexpr uint16_t kMagic        = 0x5353;
constexpr uint8_t  kProtoVersion = 1;
constexpr size_t   kMaxFrame     = 250;
constexpr size_t   kHeaderSize   = 8;
constexpr size_t   kCrcSize      = 2;
constexpr size_t   kMaxPayload   = kMaxFrame - kHeaderSize - kCrcSize;  // 240

enum MsgType : uint8_t {
  MSG_HELLO     = 1,  // node -> core (broadcast while unpaired)
  MSG_WELCOME   = 2,  // core -> node
  MSG_CONFIG    = 3,  // core -> node (role, categories, channels)
  MSG_HEARTBEAT = 4,  // node -> core
  MSG_SIGHTINGS = 5,  // node -> core (batch)
  MSG_NAME      = 6,  // node -> core (device name, sent once per address)
  MSG_EVENT     = 7,  // node -> core (behavioral detection: deauth flood etc.)
  MSG_COMMAND   = 8,  // core -> node (reboot, identify, name)
  MSG_DASH      = 9,  // core -> dashboard node (CYD): what's around, latest alert
  MSG_DASH_CMD  = 10, // dashboard node -> core: a tap (toggle a category, deployment)
  MSG_DASH_VIEW = 11, // core -> dashboard node: data for the screen it has open
  MSG_PAIR_REQ  = 12, // unpaired node -> broadcast: "I'd like to join", with its public key
  MSG_PAIR_OFFER = 13,// master in pairing mode -> node: the master's public key (and "blink")
  MSG_PAIR_KEY  = 14, // master -> node, after you press ACCEPT: the cluster key, sealed
  MSG_LOG       = 15, // master -> dashboard node: one detection-log record (ss_log.h), for its SD card
  MSG_GPS       = 16, // GPS node -> master, every 5 s: fix details and satellites (the web page's GPS window)
  MSG_FILE_REQ  = 17, // master -> node with an SD card: list its files, or read part of one (ss_fileserve.h)
  MSG_FILE      = 18, // node -> master: the answer (a page of the file list, a piece of a file, or an error)
};

enum NodeCmd : uint8_t {
  NC_REBOOT   = 1,
  NC_IDENTIFY = 2,  // flash the node's screen / LED for arg seconds so you can find it
  NC_LABEL    = 3,  // the name you gave this node on the master (text)
  NC_UNPAIR   = 4,  // FORGET on the master: drop the cluster key, wait to be paired again
  NC_SD_FORMAT = 5, // erase and format the node's SD card. arg = request token (the master repeats
                    // commands for a while; the node formats once per token)
};
constexpr size_t kMaxLabel = 20;
struct NodeCommand {  // 3 + len bytes
  uint8_t cmd = 0;
  uint8_t arg = 0;
  uint8_t len = 0;
  char    text[kMaxLabel + 1] = {0};
};

// Detection categories. Bit (1 << cat) in a category mask.
enum Category : uint8_t {
  CAT_NONE = 0,
  CAT_FLOCK,       // Flock ALPR cameras and Flock Raven
  CAT_BODYCAM,
  CAT_TRACKER,
  CAT_GLASSES,
  CAT_DRONE,
  CAT_SKIMMER,
  CAT_PENTEST,
  CAT_PHONE,
  CAT_AUDIO,
  CAT_WEARABLE,
  CAT_OTHER_BLE,
  CAT_OTHER_WIFI,
  CAT_LORA,        // Meshtastic nodes, LoRaWAN trackers (sub-GHz node)
  CAT_VEHICLE,     // cars: Tesla Bluetooth, car Wi-Fi hotspots, head units, OBD dongles
  CAT_SUBGHZ,      // sub-GHz bursts per band (T-Embed CC1101): fobs, remotes, sensors. Last free bit.
  CAT_COUNT
};

static_assert(CAT_COUNT <= 16, "category masks are 16 bits");
constexpr uint16_t catBit(uint8_t cat) { return (uint16_t)(1u << cat); }
constexpr uint16_t kSurveillanceMask =
    catBit(CAT_FLOCK) | catBit(CAT_BODYCAM) | catBit(CAT_TRACKER) | catBit(CAT_GLASSES) |
    catBit(CAT_DRONE) | catBit(CAT_SKIMMER) | catBit(CAT_PENTEST);
constexpr uint16_t kConsumerMask =
    catBit(CAT_PHONE) | catBit(CAT_AUDIO) | catBit(CAT_WEARABLE) | catBit(CAT_OTHER_BLE);
constexpr uint16_t kAllCategoriesMask =
    kSurveillanceMask | kConsumerMask | catBit(CAT_OTHER_WIFI) | catBit(CAT_LORA) | catBit(CAT_VEHICLE) |
    catBit(CAT_SUBGHZ);

// What kind of transmission a sighting came from.
enum Kind : uint8_t {
  KIND_NONE            = 0x00,
  KIND_WIFI_BEACON     = 0x01,
  KIND_WIFI_PROBE_REQ  = 0x02,
  KIND_WIFI_PROBE_RESP = 0x03,
  KIND_WIFI_DATA       = 0x04,
  KIND_WIFI_OTHER      = 0x05,
  KIND_BLE_ADV         = 0x10,
  KIND_BT_CLASSIC      = 0x20,  // Classic Bluetooth, from an original-ESP32 node (inquiry)
  KIND_LORA_MESHTASTIC = 0x30,  // sub-GHz node (Heltec Wireless Tracker): addr = node id, zero padded
  KIND_LORAWAN         = 0x31,  //   addr = DevAddr zero padded, or last 6 bytes of DevEUI (join)
  KIND_LORA_MESHCORE   = 0x32,  //   addr = first 6 bytes of the node's public key
  KIND_SUBGHZ          = 0x40,  // sub-GHz bursts on one band (T-Embed): addr = 02:53:47 + kHz (subGhzSighting)
};
inline bool kindIsWifi(uint8_t k) { return k >= KIND_WIFI_BEACON && k <= KIND_WIFI_OTHER; }
inline bool kindIsBle(uint8_t k) { return k == KIND_BLE_ADV; }
inline bool kindIsLora(uint8_t k) { return (k & 0xF0) == 0x30; }
inline bool kindIsSubGhz(uint8_t k) { return k == KIND_SUBGHZ; }
// Radio class used to key devices: 1 = Wi-Fi, 2 = BLE, 3 = BT classic, 4 = LoRa.
inline uint8_t radioClass(uint8_t k) {
  if (kindIsWifi(k)) return 1;
  if (kindIsBle(k)) return 2;
  if (k == KIND_BT_CLASSIC) return 3;
  if (kindIsLora(k)) return 4;
  if (kindIsSubGhz(k)) return 5;
  return 0;
}

// Sighting flags
enum SightingFlags : uint8_t {
  SF_RANDOM_ADDR  = 0x01,  // Wi-Fi locally-administered bit, or BLE random address
  SF_STATIC_RAND  = 0x02,  // BLE random static (does not rotate)
  SF_CONNECTABLE  = 0x04,
  SF_HAS_NAME     = 0x08,  // a MSG_NAME was (or will be) sent for this address
  SF_STRONG_MATCH = 0x10,  // signature match is specific, not vendor-level
  SF_5GHZ         = 0x20,
  SF_SPAM_PROBE   = 0x40,  // category disabled, but sent so the Core's BLE spam detector sees it
};

enum HwType : uint8_t {
  HW_UNKNOWN = 0, HW_C5_DEVKIT = 1, HW_MARAUDER_V8 = 2, HW_CYD = 3, HW_SBC = 4, HW_HELTEC_TRACKER = 5, HW_ESP32_DEVKIT = 6, HW_TEMBED_CC1101 = 7,
};

enum Caps : uint8_t {
  CAP_WIFI24     = 0x01,
  CAP_WIFI5      = 0x02,
  CAP_BLE        = 0x04,
  CAP_BT_CLASSIC = 0x08,
  CAP_GPS        = 0x10,
  CAP_DISPLAY    = 0x20,
  CAP_LORA_915   = 0x40,
  CAP_DASH       = 0x80,  // has a touch screen and wants MSG_DASH (CYD)
};

enum Role : uint8_t { ROLE_AUTO = 0, ROLE_WIFI = 1, ROLE_BLE = 2, ROLE_BOTH = 3,
                     // Master-side pin only (never sent): this node scans the whole plan on
                     // its own, outside the split; the other nodes share the work between them.
                     ROLE_ALL = 4 };

enum EventType : uint8_t {
  EV_DEAUTH_FLOOD   = 1,
  EV_DISASSOC_FLOOD = 2,
  EV_BLE_SPAM       = 3,  // raised by the Core; channel holds the burst's mean RSSI (int8)
  EV_WIFI_SPAM      = 4,  // beacon flood (fake networks from one transmitter); channel = mean RSSI
  EV_EVIL_TWIN      = 5,  // one radio (src) answering as several network names (count): Pineapple "PineAP" / karma
  EV_RF_JAM         = 6,  // sub-GHz band held loud (T-Embed CC1101): channel = band, count = MHz x 100, dst[0] = dB over floor
};

struct Header {
  uint16_t magic   = kMagic;
  uint8_t  version = kProtoVersion;
  uint8_t  type    = 0;
  uint16_t node_id = 0;
  uint16_t seq     = 0;
};

// `auth` = first 4 bytes of HMAC-SHA256(cluster key, fields before it), little-endian.
// 0 when the cluster runs without a key. Lets nodes and the Core ignore strangers.
struct Hello {  // 15 bytes
  uint8_t  mac[6]     = {0};
  uint16_t fw_version = 0;
  uint8_t  hw         = HW_UNKNOWN;
  uint8_t  caps       = 0;
  uint8_t  role_pref  = ROLE_AUTO;
  uint32_t auth       = 0;
};

struct Welcome {  // 15 bytes
  uint8_t  core_mac[6] = {0};
  uint8_t  node_index  = 0;
  uint32_t core_ms     = 0;  // Core uptime, lets nodes express ages in Core time
  uint32_t auth        = 0;  // HMAC over core_mac..core_ms plus the node's MAC (see ss_auth)
};

// Config.node_flags
enum NodeFlags : uint8_t {
  NF_DISPLAY_OFF = 0x01,  // node's own screen (Heltec Wireless Tracker) off, backlight dark
  NF_LONG_RANGE  = 0x02,  // talk to the master in Espressif Long Range (LR) mode
  NF_PROBES      = 0x04,  // send the networks phones ask for by name (opt-in on the master's page)
  NF_LR_FAST     = 0x08,  // with NF_LONG_RANGE: the 500 kbit/s LR rate (half the airtime, a bit less range)
};

struct Config {  // 18 bytes + optional node_flags (older nodes ignore it)
  uint8_t  cfg_version   = 0;
  uint8_t  role          = ROLE_BOTH;
  uint16_t category_mask = 0;
  uint64_t channel_mask  = 0;  // bit i = kChannels[i] (see ss_channels.h)
  uint16_t dwell_ms      = 120;
  uint16_t report_ms     = 1000;
  int8_t   rssi_floor    = -95;
  uint8_t  ble_duty_pct  = 100;
  uint8_t  node_flags    = 0;     // NodeFlags; absent on the wire from older masters = 0
  char     label[kMaxLabel + 1] = {0};  // the name given to this node on the master (optional)
  uint8_t  lora_mode     = 0;     // LoRa boards: what to listen to (ss_lora.h LoraMode, 0 = auto)
  uint8_t  lora_slot     = 0;     //   Meshtastic frequency slot, 1-based (0 = the preset's default)
};
constexpr size_t kMaxConfigSize = 18 + 1 + 1 + kMaxLabel + 2;

// Pairing (see ss_pair.h). Sent in the clear: the public keys are public, and the
// cluster key travels sealed with the X25519 shared secret.
constexpr size_t kPairKeyMax = 32;   // cluster keys are up to 32 characters
enum PairOfferFlag : uint8_t { PO_BLINK = 0x01 };
struct PairReq {
  uint8_t  mac[6] = {0};
  uint8_t  hw = 0, caps = 0;
  uint16_t fw = 0;
  uint8_t  pub[32] = {0};
};
struct PairOffer {
  uint8_t target[6] = {0};
  uint8_t pub[32] = {0};
  uint8_t flags = 0;
};
struct PairKey {
  uint8_t target[6] = {0};
  uint8_t len = 0;
  uint8_t sealed[kPairKeyMax] = {0};
  uint8_t tag[16] = {0};
};
size_t encodePairReq(const PairReq& m, uint8_t* out, size_t cap);
bool   decodePairReq(const uint8_t* p, size_t n, PairReq& m);
size_t encodePairOffer(const PairOffer& m, uint8_t* out, size_t cap);
bool   decodePairOffer(const uint8_t* p, size_t n, PairOffer& m);
size_t encodePairKey(const PairKey& m, uint8_t* out, size_t cap);
bool   decodePairKey(const uint8_t* p, size_t n, PairKey& m);

struct Heartbeat {  // 18 bytes, + 2 optional (GPS), + 7 (health), + 13 (position, clock), + 1 (SD)
  uint32_t uptime_s    = 0;
  uint8_t  cfg_version = 0;
  uint8_t  channel     = 0;
  uint8_t  batt_pct    = 255;  // 255 = no battery
  int8_t   temp_c      = -128; // -128 = unknown
  uint16_t dropped     = 0;    // sightings dropped since last heartbeat (queue full)
  uint32_t wifi_frames = 0;
  uint32_t ble_adv     = 0;
  uint8_t  gps         = 0;    // GpsState (ss_gps.h), 0 = no GPS on this board
  uint8_t  gps_sats    = 0;    // satellites used in the fix
  // Health (optional, newer nodes): why it last started, memory, link drops
  uint8_t  reset       = 0;    // ResetWhy
  uint16_t heap_kb     = 0;    // free heap now
  uint16_t heap_min_kb = 0;    // lowest free heap since boot
  uint16_t losses      = 0;    // times it lost the master since boot
  // Position and clock (optional, GPS boards with a fix / a date)
  bool     has_pos     = false;
  int32_t  lat_e7      = 0;    // degrees * 1e7
  int32_t  lon_e7      = 0;
  uint32_t utc         = 0;    // Unix seconds, 0 = unknown
  // SD card logging (optional, dashboard nodes): SdState
  uint8_t  sd          = 0;
  // Link (optional): bit 0 = can do Long Range, bit 1 = sending to the master in LR
  uint8_t  link        = 0;
};
enum HbLink : uint8_t { HL_LR_CAPABLE = 0x01, HL_LR_ON = 0x02 };
enum SdState : uint8_t { SD_NONE = 0, SD_NO_CARD = 1, SD_LOGGING = 2, SD_ERROR = 3, SD_FULL = 4, SD_FORMATTING = 5,
                       SD_UNFORMATTED = 6 /* card answers, but no FAT file system (exFAT, NTFS, blank) */ };
const char* sdStateName(uint8_t s);

// Everything a GPS node knows, for the web page's GPS window (MSG_GPS).
constexpr size_t kGpsSats = 32;
struct GpsInfo {
  uint8_t state = 0, sats_used = 0;
  uint8_t hdop10 = 0;          // HDOP x 10 (255 = 25.5 or worse)
  bool has_pos = false;
  int32_t lat_e7 = 0, lon_e7 = 0;
  bool has_alt = false;
  int16_t alt_m = 0;
  bool has_motion = false;
  uint16_t kmh10 = 0;          // ground speed, km/h x 10
  uint16_t course10 = 0;       // degrees x 10
  uint32_t utc = 0;
  uint8_t n_sat = 0;
  struct Sat { uint8_t sys, prn, elev, az2, snr; bool used; } sat[kGpsSats];  // az2 = azimuth / 2
};
size_t encodeGpsInfo(const GpsInfo& m, uint8_t* out, size_t cap);
bool decodeGpsInfo(const uint8_t* p, size_t n, GpsInfo& m);  // "none", "no_card", "logging", "error", "full", "formatting", "unformatted"
enum ResetWhy : uint8_t {
  RW_UNKNOWN = 0, RW_POWER = 1 /* power on or reset button */, RW_SOFTWARE = 2 /* reboot command, update */,
  RW_CRASH = 3, RW_WATCHDOG = 4, RW_BROWNOUT = 5, RW_NO_MASTER = 6 /* restarted itself after losing the master */,
  RW_DEEPSLEEP = 7, RW_USB = 8 /* reset over USB: flashing, or a serial monitor opening the port */,
  RW_RADIO = 9 /* restarted itself: its Wi-Fi radio heard nothing (didn't start properly) */,
  RW_STUCK_TX = 10 /* restarted itself: frames stuck in the Wi-Fi driver (esp-idf #19269) or sends refused */,
};
const char* resetWhyName(uint8_t r);  // "power", "crash", ...

struct Sighting {  // 24 bytes on the wire
  uint8_t  addr[6]   = {0};  // MSB first, as printed (AA:BB:CC:DD:EE:FF)
  uint8_t  kind      = KIND_NONE;
  uint8_t  channel   = 0;    // Wi-Fi channel, or 37/38/39 (0 if unknown) for BLE
  int8_t   rssi_max  = -127;
  int8_t   rssi_last = -127;
  uint8_t  hits      = 0;    // saturates at 255
  uint8_t  category  = CAT_NONE;
  uint16_t sig_id    = 0;    // signature id (ss_classify.h), 0 = none
  uint8_t  conf      = 0;    // 0..100
  uint8_t  flags     = 0;
  uint16_t company   = 0;    // BLE company id (0xFFFF = none)
  uint32_t fp        = 0;    // hardware fingerprint hash, 0 = none
  uint16_t age_ms    = 0;    // ms between the last hit and when the batch was sent
};
constexpr size_t kSightingSize          = 24;
constexpr size_t kMaxSightingsPerFrame  = (kMaxPayload - 1) / kSightingSize;  // 9

constexpr size_t kMaxName = 32;
constexpr size_t kMaxWps = 24;
// Access point details from its beacon (ss_classify.h WifiFrame): security, Wi-Fi
// generation, channel width, load, and the make / model its WPS element gives.
struct ApMeta {
  uint8_t  sec   = 0;        // ApSec (ss_classify.h), 0 = not known
  uint8_t  gen   = 0;        // 4..7 = Wi-Fi 4..7, 0 = older
  uint8_t  width = 20;       // MHz
  uint8_t  util  = 255;      // channel busy 0..255, 255 = not said
  uint16_t sta   = 0xFFFF;   // devices connected, 0xFFFF = not said
  uint32_t up_s  = 0;        // how long since it started (beacon clock), 0 = unknown
  char     wps[kMaxWps + 1] = {0};
};
enum NameWhat : uint8_t {
  NAME_DEVICE = 0,   // the device's own name (an AP's SSID, a BLE name)
  NAME_PROBE  = 1,   // a network this device (a phone) is looking for, from its probe request
  NAME_POS    = 2,   // where the device says it is ("lat,lon,alt_m"): Meshtastic position
  NAME_LINK   = 3,   // the access point (BSSID, "AA:BB:CC:DD:EE:FF") this client is connected to
  NAME_BATT   = 4,   // battery percent the device reports ("87"; 101 = on external power)
  NAME_MESH   = 5,   // last Meshtastic packet: "LongFast:67" (channel:port) or "private:3A" (channel hash);
                     // MeshCore: "meshcore:advert" (what it last sent)
  NAME_RID_ID = 6,   // drone Remote ID: "serial|id_type|ua_type"
  NAME_RID_MOVE = 7, // drone motion: "height_m,speed_cms,dir,status" (position goes as NAME_POS)
  NAME_RID_OP = 8,   // where the drone's pilot is: "lat,lon"
  NAME_RID_TEXT = 9, // the drone's self-ID text ("Roof survey")
};
// 8 + len bytes, then optional tails: 0xB0 + what (NAME_PROBE..: the text isn't the device's name),
// 0xA1 sec gen width util sta(2) wps_len wps (access point details).
struct Name {
  uint8_t addr[6]          = {0};
  uint8_t kind             = KIND_NONE;
  uint8_t len              = 0;
  char    name[kMaxName + 1] = {0};
  uint8_t what             = NAME_DEVICE;
  bool    has_ap           = false;
  ApMeta  ap;
};

struct Event {  // 22 bytes
  uint8_t  type     = 0;
  uint8_t  channel  = 0;
  uint16_t count    = 0;      // frames seen in the window
  uint8_t  src[6]   = {0};
  uint8_t  dst[6]   = {0};
  uint8_t  bssid[6] = {0};
};

// ---------------------------------------------------------------------------
// Low-level serialization helpers

class Writer {
 public:
  Writer(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}
  void u8(uint8_t v);
  void i8(int8_t v) { u8((uint8_t)v); }
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void bytes(const uint8_t* p, size_t n);
  size_t size() const { return pos_; }
  bool ok() const { return ok_; }

 private:
  uint8_t* buf_;
  size_t cap_;
  size_t pos_ = 0;
  bool ok_ = true;
};

class Reader {
 public:
  Reader(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}
  uint8_t u8();
  int8_t i8() { return (int8_t)u8(); }
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  void bytes(uint8_t* out, size_t n);
  size_t remaining() const { return ok_ ? len_ - pos_ : 0; }
  bool ok() const { return ok_; }

 private:
  const uint8_t* buf_;
  size_t len_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// ---------------------------------------------------------------------------
// Frames

uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF);  // CCITT-FALSE

// Builds a full frame. Returns its length, or 0 if it doesn't fit.
size_t encodeFrame(uint8_t type, uint16_t node_id, uint16_t seq,
                   const uint8_t* payload, size_t payload_len,
                   uint8_t* out, size_t out_cap);

// Validates magic, version, length and CRC. On success points `payload` into `frame`.
bool decodeFrame(const uint8_t* frame, size_t len, Header& hdr,
                 const uint8_t** payload, size_t* payload_len);

// Payload codecs. encode* return bytes written (0 on overflow); decode* return success.
// Bytes covered by Hello::auth / Welcome::auth (everything before the auth field;
// Welcome also appends the destination node MAC so a reply can't be replayed to another node).
size_t helloAuthInput(const Hello& m, uint8_t* out, size_t cap);
size_t welcomeAuthInput(const Welcome& m, const uint8_t node_mac[6], uint8_t* out, size_t cap);

size_t encodeHello(const Hello& m, uint8_t* out, size_t cap);
bool   decodeHello(const uint8_t* p, size_t n, Hello& m);
size_t encodeWelcome(const Welcome& m, uint8_t* out, size_t cap);
bool   decodeWelcome(const uint8_t* p, size_t n, Welcome& m);
size_t encodeConfig(const Config& m, uint8_t* out, size_t cap);
bool   decodeConfig(const uint8_t* p, size_t n, Config& m);
// ---------------------------------------------------------------------------
// Dashboard (CYD): the master's summary for a touch screen node, sent after each
// CONFIG to nodes with CAP_DASH. Fits one frame.
constexpr uint8_t kDashThreats = 5;
constexpr uint8_t kDashText = 26;
constexpr uint8_t kDashAlertText = 36;
enum DashThreatFlag : uint8_t { DT_FOLLOWING = 0x01, DT_STRONG = 0x02, DT_QUIET = 0x04 /* not heard for 10 s */ };
struct DashThreat {
  uint8_t category = CAT_NONE;
  int8_t  rssi = -127;
  uint8_t conf = 0;
  uint8_t flags = 0;
  char    text[kDashText + 1] = {0};   // name, or what it is ("Flock SSID")
};
struct DashAlert {
  uint16_t seq = 0;        // changes with every new alert (0 = none yet)
  uint8_t  level = 0;      // 1 notice, 2 alert (banner + beep)
  uint8_t  category = CAT_NONE;
  uint16_t age_s = 0;
  char     text[kDashAlertText + 1] = {0};
};
struct Dash {
  uint8_t  deploy = 0;            // Deployment (ss_follow.h)
  uint16_t cats = 0;              // categories switched on
  uint8_t  nodes_online = 0;
  uint8_t  nodes_total = 0;
  uint16_t devices_now = 0;
  uint8_t  now[CAT_COUNT] = {0};  // devices heard in the last 30 s per category (saturates at 255)
  DashAlert alert;
  uint8_t  n_threats = 0;
  DashThreat threats[kDashThreats];
};
enum DashCmd : uint8_t {
  DC_TOGGLE_CAT = 1,  // arg = category
  DC_DEPLOY = 2,      // arg = Deployment
  DC_VIEW = 3,        // arg = DashView it has open, offset = first row, id = device (detail)
};
struct DashCommand {
  uint8_t cmd = 0;
  uint8_t arg = 0;
  uint16_t offset = 0;  // optional (DC_VIEW)
  uint32_t id = 0;      // optional (DC_VIEW detail)
};

// Screens beyond the dashboard. The CYD says which one it has open (DC_VIEW); the
// master sends that screen's data (MSG_DASH_VIEW) along with every dashboard update.
enum DashView : uint8_t { DV_NONE = 0, DV_DEVICES = 1, DV_DETAIL = 2, DV_EVENTS = 3, DV_NODES = 4 };
constexpr uint8_t kDashRows = 6;
constexpr uint8_t kDashEvents = 5;
constexpr uint8_t kDashNodes = 6;
constexpr uint8_t kDashLabel = 12;
enum DashRowFlag : uint8_t { DR_STRONG = 0x01, DR_FOLLOWING = 0x02, DR_RANDOM = 0x04, DR_SPOOFED = 0x08 };
struct DashDevRow {
  uint32_t id = 0;
  uint8_t  category = CAT_NONE;
  int8_t   rssi = -127;
  uint8_t  conf = 0;
  uint8_t  flags = 0;       // DashRowFlag
  uint8_t  age_s = 0;       // saturates at 255
  uint8_t  radio = 0;       // 1 Wi-Fi, 2 BLE, 3 Classic, 4 LoRa
  char     text[kDashText + 1] = {0};
};
struct DashDevices {
  uint16_t total = 0;       // devices that would be listed
  uint16_t offset = 0;
  uint8_t  n = 0;
  DashDevRow rows[kDashRows];
};
struct DashDetail {
  uint32_t id = 0;
  bool     found = false;
  uint8_t  addr[6] = {0};
  uint8_t  category = CAT_NONE, radio = 0, conf = 0, flags = 0, channel = 0, rotations = 0, places = 0;
  uint16_t company = 0xFFFF;
  int8_t   rssi = -127, rssi_max = -127;
  uint32_t age_s = 0, first_s = 0, hits = 0;
  char     sig[kDashText + 1] = {0};
  char     name[kMaxName + 1] = {0};
};
struct DashEvent {
  uint8_t  level = 0;       // 0 info, 1 notice, 2 alert
  uint8_t  category = CAT_NONE;
  uint32_t age_s = 0;
  char     text[kDashAlertText + 1] = {0};
};
struct DashEvents {
  uint16_t total = 0, offset = 0;
  uint8_t  n = 0;
  DashEvent ev[kDashEvents];
};
enum DashNodeFlag : uint8_t { DN_ONLINE = 0x01, DN_MASTER = 0x02, DN_ACK = 0x04 };
struct DashNode {
  uint16_t id = 0;
  uint8_t  hw = 0, flags = 0, caps = 0, chans = 0, duty = 0;
  int8_t   link = -127;
  uint8_t  loss_pct = 0;    // x10 (0.1 % steps), saturates at 255
  uint8_t  batt = 255, gps = 0, sats = 0, reset = 0;
  uint16_t heap_kb = 0, losses = 0;
  uint32_t up_s = 0;
  char     label[kDashLabel + 1] = {0};
};
struct DashNodes {
  uint8_t  total = 0, offset = 0, n = 0;
  DashNode nodes[kDashNodes];
};
uint8_t dashViewOf(const uint8_t* p, size_t n);   // first byte of a MSG_DASH_VIEW payload
size_t encodeDashDevices(const DashDevices& m, uint8_t* out, size_t cap);
bool   decodeDashDevices(const uint8_t* p, size_t n, DashDevices& m);
size_t encodeDashDetail(const DashDetail& m, uint8_t* out, size_t cap);
bool   decodeDashDetail(const uint8_t* p, size_t n, DashDetail& m);
size_t encodeDashEvents(const DashEvents& m, uint8_t* out, size_t cap);
bool   decodeDashEvents(const uint8_t* p, size_t n, DashEvents& m);
size_t encodeDashNodes(const DashNodes& m, uint8_t* out, size_t cap);
bool   decodeDashNodes(const uint8_t* p, size_t n, DashNodes& m);
size_t encodeDash(const Dash& m, uint8_t* out, size_t cap);
bool   decodeDash(const uint8_t* p, size_t n, Dash& m);
size_t encodeDashCommand(const DashCommand& m, uint8_t* out, size_t cap);
bool   decodeDashCommand(const uint8_t* p, size_t n, DashCommand& m);

size_t encodeHeartbeat(const Heartbeat& m, uint8_t* out, size_t cap);
bool   decodeHeartbeat(const uint8_t* p, size_t n, Heartbeat& m);
size_t encodeName(const Name& m, uint8_t* out, size_t cap);
bool   decodeName(const uint8_t* p, size_t n, Name& m);
size_t encodeEvent(const Event& m, uint8_t* out, size_t cap);
bool   decodeEvent(const uint8_t* p, size_t n, Event& m);
size_t encodeCommand(const NodeCommand& m, uint8_t* out, size_t cap);
bool   decodeCommand(const uint8_t* p, size_t n, NodeCommand& m);

// Remote SD card files (MSG_FILE_REQ / MSG_FILE). The master pulls a file from a node
// a window of chunks at a time and asks again from where it got to, so a lost frame just
// means asking again. Works for any node with a card; ss_fileserve.h answers.
constexpr size_t kMaxFileName = 31;
constexpr size_t kFileChunk = 200;   // data bytes per MSG_FILE
constexpr uint8_t kFileWindow = 8;   // chunks the master asks for at a time
enum FileOp : uint8_t { FO_LIST = 1, FO_READ = 2 };
struct FileReq {
  uint8_t  req = 0;     // the master's transfer number; answers carry it back
  uint8_t  op = 0;      // FileOp
  uint32_t off = 0;     // FO_LIST: first entry wanted; FO_READ: first byte wanted
  uint8_t  count = 0;   // FO_READ: chunks wanted (1..kFileWindow)
  char     name[kMaxFileName + 1] = {0};  // FO_READ: file name, no folders
};
enum FileKind : uint8_t { FK_LIST = 1, FK_DATA = 2, FK_ERROR = 3 };
enum FileErr : uint8_t { FE_NO_CARD = 1, FE_NOT_FOUND = 2, FE_BAD_NAME = 3, FE_READ = 4 };
struct FileChunk {
  uint8_t  req = 0;
  uint8_t  kind = 0;    // FileKind
  uint32_t off = 0;     // FK_LIST: index of the first entry here; FK_DATA: byte offset; FK_ERROR: FileErr
  uint32_t total = 0;   // FK_LIST: number of files; FK_DATA: file size now
  uint8_t  len = 0;
  uint8_t  data[kFileChunk] = {0};  // FK_LIST: entries {u8 name length, name, u32 size}; FK_DATA: file bytes
};
size_t encodeFileReq(const FileReq& m, uint8_t* out, size_t cap);
bool   decodeFileReq(const uint8_t* p, size_t n, FileReq& m);
size_t encodeFileChunk(const FileChunk& m, uint8_t* out, size_t cap);
bool   decodeFileChunk(const uint8_t* p, size_t n, FileChunk& m);
// A safe plain file name: 1..kMaxFileName printable characters, no '/', '\\' or "..".
bool   fileNameOk(const char* name);

// Sightings payload: u8 count, then count * 24-byte records.
size_t encodeSightings(const Sighting* s, size_t count, uint8_t* out, size_t cap);
// Decodes up to `max` records into `out`. Returns false on malformed input.
bool decodeSightings(const uint8_t* p, size_t n, Sighting* out, size_t max, size_t* count);

// ---------------------------------------------------------------------------
// COBS framing for byte streams (USB / UART). Encoded frames are delimited by 0x00.

// Worst-case encoded size of n bytes (excluding the trailing 0x00).
constexpr size_t cobsMaxEncoded(size_t n) { return n + n / 254 + 1; }
size_t cobsEncode(const uint8_t* in, size_t n, uint8_t* out, size_t cap);
size_t cobsDecode(const uint8_t* in, size_t n, uint8_t* out, size_t cap);  // 0 on error

// Feed bytes one at a time; returns true when a complete decoded frame is ready.
class CobsDeframer {
 public:
  bool feed(uint8_t byte);
  const uint8_t* frame() const { return out_; }
  size_t frameLen() const { return out_len_; }
  uint32_t errors() const { return errors_; }

 private:
  uint8_t buf_[cobsMaxEncoded(kMaxFrame) + 2];
  uint8_t out_[kMaxFrame];
  size_t len_ = 0;
  size_t out_len_ = 0;
  bool overflow_ = false;
  uint32_t errors_ = 0;
};

// ---------------------------------------------------------------------------
// Misc

const char* categoryName(uint8_t cat);   // "tracker", "flock", ...
const char* categoryLabel(uint8_t cat);  // "Trackers", "Flock", ...
inline uint16_t nodeIdFromMac(const uint8_t mac[6]) { return (uint16_t)((mac[4] << 8) | mac[5]); }
void formatMac(const uint8_t mac[6], char out[18]);  // "AA:BB:CC:DD:EE:FF"

}  // namespace ss
