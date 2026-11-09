// Parsing, hardware fingerprinting and signature matching for BLE advertisements
// and 802.11 management frames. Pure C++ so it runs on a node, the Core, an SBC,
// or the host test suite.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ss_proto.h"

namespace ss {

// ---------------------------------------------------------------------------
// BLE

// (Not named BLE_ADDR_*: NimBLE defines those as macros.)
enum BleAddrType : uint8_t { SS_ADDR_PUBLIC = 0, SS_ADDR_RANDOM = 1 };

struct BleSvcData {
  uint16_t uuid = 0;
  const uint8_t* data = nullptr;  // points into the payload passed to parseBleAdv
  uint8_t len = 0;
};

struct BleAdv {
  // Filled by the caller (radio driver):
  uint8_t addr[6] = {0};  // MSB first
  uint8_t addr_type = SS_ADDR_PUBLIC;
  int8_t rssi = -127;
  bool connectable = false;
  // Filled by parseBleAdv:
  bool has_flags = false;
  uint8_t flags = 0;
  bool has_company = false;
  uint16_t company = 0xFFFF;
  const uint8_t* mfg = nullptr;  // manufacturer data after the 2-byte company id
  uint8_t mfg_len = 0;
  uint16_t uuid16[8] = {0};
  uint8_t n_uuid16 = 0;
  uint8_t n_uuid128 = 0;
  BleSvcData svc[4];
  uint8_t n_svc = 0;
  char name[kMaxName + 1] = {0};
  uint8_t name_len = 0;
  bool has_tx_power = false;
  int8_t tx_power = 0;
  bool has_appearance = false;
  uint16_t appearance = 0;  // GAP appearance: category = value >> 6, subcategory = value & 0x3F
  uint32_t fp = 0;  // fingerprint: stable across address rotation for the same model/state
};

// Parses advertising data (AD structures). Tolerates truncated/garbage input.
bool parseBleAdv(const uint8_t* payload, size_t len, BleAdv& out);

bool bleIsRandom(const BleAdv& a);
bool bleIsStaticRandom(const BleAdv& a);  // random static: top two bits 11, does not rotate

// ---------------------------------------------------------------------------
// Wi-Fi

struct WifiFrame {
  uint8_t type = 0, subtype = 0;
  uint8_t kind = KIND_NONE;
  uint8_t addr1[6] = {0}, addr2[6] = {0}, addr3[6] = {0};  // addr2 = transmitter
  uint16_t seq = 0;
  char ssid[33] = {0};
  uint8_t ssid_len = 0;
  bool has_ssid = false;
  uint8_t ds_channel = 0;
  bool remote_id = false;  // ASTM F3411 Wi-Fi beacon vendor IE
  uint32_t vendor[6] = {0};  // (OUI << 8) | vendor type
  uint8_t n_vendor = 0;
  uint32_t fp = 0;  // probe-request capability fingerprint (0 for other frames)
  bool to_ds = false, from_ds = false;   // data frames: to_ds && !from_ds = a client talking to its AP (addr1)
  uint64_t tsf = 0;        // beacon / probe response timestamp: microseconds since the AP started
  bool apple_ie = false;   // Apple vendor element (00:17:F2): iPhone / iPad hotspots carry it
  const uint8_t* rid = nullptr;   // Remote ID payload (after OUI + type), points into the frame
  uint8_t rid_len = 0;
  // Access point details (beacons / probe responses)
  uint8_t sec = 0;         // ApSec
  uint8_t gen = 0;         // Wi-Fi generation 4..7 (0 = older a/b/g)
  uint8_t width = 20;      // channel width, MHz
  uint16_t sta = 0xFFFF;   // devices connected (BSS Load element), 0xFFFF = not said
  uint8_t util = 255;      // channel busy, 0..255 (BSS Load), 255 = not said
  char wps[33] = {0};      // WPS manufacturer / model, "" = none
};
enum ApSec : uint8_t { SEC_UNKNOWN = 0, SEC_OPEN, SEC_WEP, SEC_WPA, SEC_WPA2, SEC_WPA3, SEC_WPA2_3, SEC_ENTERPRISE, SEC_OWE };
const char* apSecName(uint8_t s);   // "open", "WPA2", ...

// `frame` starts at the 802.11 header, `len` excludes the FCS.
bool parseWifiFrame(const uint8_t* frame, size_t len, WifiFrame& out);

// Drone Remote ID (ASTM F3411 / FAA): what the broadcast says. Values as sent; 0 / ""
// when that message wasn't in this packet (each packet usually carries one or a pack).
struct RemoteId {
  bool has_id = false;
  char id[21] = {0};        // serial number (or session / registration id)
  uint8_t id_type = 0;      // 1 serial, 2 CAA registration, 3 UTM, 4 session
  uint8_t ua_type = 0;      // 2 helicopter / multirotor, 1 aeroplane, ...
  bool has_loc = false;
  int32_t lat_e7 = 0, lon_e7 = 0;
  int16_t alt_m = -1000;    // geodetic (GPS) altitude, -1000 = unknown
  int16_t height_m = -1000; // above takeoff or ground, -1000 = unknown
  uint16_t speed_cms = 0;   // horizontal, cm/s
  uint16_t dir = 361;       // degrees true, 361 = unknown
  uint8_t status = 0;       // 2 airborne, 1 on ground
  bool has_op = false;
  int32_t op_lat_e7 = 0, op_lon_e7 = 0;   // where the pilot / controller is
  char text[24] = {0};      // self-ID description ("Survey flight")
};
// payload = BLE service data for 0xFFFA (app code 0x0D, counter, message), or a Wi-Fi
// beacon's Remote ID element after its OUI + type (counter, message pack).
bool decodeRemoteIdBle(const uint8_t* p, size_t n, RemoteId& out);
bool decodeRemoteIdWifi(const uint8_t* p, size_t n, RemoteId& out);
bool remoteIdFromBle(const BleAdv& a, RemoteId& out);

// Maker of a fixed (global) address, from the IEEE registry: 0 = not one we list.
// Phone and tablet makers plus Amazon (tools/update_oui_makers.py).
uint8_t ouiMaker(const uint8_t mac[6]);
const char* ouiMakerName(uint8_t maker);
uint16_t ouiMakerCompany(uint8_t maker);   // Bluetooth SIG id (for the MAKER column), 0xFFFF none
bool ouiMakerIsPhone(uint8_t maker);

inline bool macIsLocallyAdministered(const uint8_t mac[6]) { return (mac[0] & 0x02) != 0; }
inline bool macIsMulticast(const uint8_t mac[6]) { return (mac[0] & 0x01) != 0; }

// ---------------------------------------------------------------------------
// Signatures

enum SigId : uint16_t {
  SIG_NONE = 0,
  // Flock / Raven
  SIG_FLOCK_SSID = 1, SIG_FLOCK_WIFI_OUI, SIG_FLOCK_WIFI_OUI_GENERIC, SIG_FLOCK_BLE_NAME,
  SIG_FLOCK_BLE_MFGR, SIG_FLOCK_BLE_OUI, SIG_RAVEN, SIG_RAVEN_PARTIAL,
  SIG_FLOCK_OWN_OUI, SIG_RING_OUI,
  // Body cams
  SIG_AXON_OUI = 20, SIG_AXON_NAME, SIG_AXON_UUID, SIG_TASER_MFGR, SIG_MOTO_SOLUTIONS, SIG_MOTO_SOLUTIONS_OUI,
  SIG_DIGITAL_ALLY_OUI, SIG_WATCHGUARD_OUI, SIG_UTILITY_OUI, SIG_ZEPCAM_OUI,
  // Glasses
  SIG_RAYBAN_META = 30, SIG_LUXOTTICA_OUI, SIG_META_UUID, SIG_META_MFGR, SIG_META_INC, SIG_META_OUI,
  SIG_SNAP_SPECTACLES, SIG_VUZIX, SIG_VUZIX_OUI,
  // Trackers
  SIG_FINDMY_SEPARATED = 40, SIG_FINDMY_NEAR_OWNER, SIG_SMARTTAG, SIG_TILE, SIG_GOOGLE_FMDN, SIG_DULT, SIG_CHIPOLO,
  // Drones
  SIG_REMOTE_ID_BLE = 60, SIG_REMOTE_ID_WIFI, SIG_DJI_DRONEID, SIG_DJI_MFGR, SIG_DJI_OUI, SIG_PARROT_OUI,
  SIG_SKYDIO_OUI, SIG_DRONE_SSID,
  // Skimmers
  SIG_SERIAL_BLE_MODULE = 70,
  // Pentest gear
  SIG_FLIPPER = 80, SIG_PINEAPPLE, SIG_PWNAGOTCHI,
  // Phones
  SIG_APPLE_DEVICE = 100, SIG_SAMSUNG_DEVICE, SIG_GOOGLE_DEVICE, SIG_PROBE_RANDOM, SIG_PROBE_GLOBAL,
  SIG_SAYS_PHONE,
  // Audio (SAYS_* = the device announces its own type in its advert)
  SIG_APPLE_AUDIO = 120, SIG_FAST_PAIR, SIG_BOSE, SIG_SONY, SIG_HARMAN,
  SIG_SAYS_EARBUDS, SIG_SAYS_HEADSET, SIG_SAYS_HEADPHONES, SIG_SAYS_SPEAKER, SIG_SAYS_HEARING_AID,
  SIG_LE_AUDIO, SIG_SWIFT_PAIR_AUDIO, SIG_AUDIO_MAKER,
  // Wearables
  SIG_GARMIN = 140, SIG_POLAR, SIG_HEART_RATE, SIG_SAYS_WATCH, SIG_SAYS_FITNESS, SIG_WEARABLE_MAKER,
  // Other
  SIG_MICROSOFT = 160, SIG_GENERIC_BLE, SIG_ACCESS_POINT, SIG_SAYS_COMPUTER, SIG_SAYS_INPUT, SIG_SWIFT_PAIR,
  // Classic Bluetooth (from the Class of Device a discoverable device answers with)
  SIG_CLASSIC_HEADSET = 180, SIG_CLASSIC_HEADPHONES, SIG_CLASSIC_SPEAKER, SIG_CLASSIC_CAR_AUDIO,
  SIG_CLASSIC_AUDIO, SIG_CLASSIC_PHONE, SIG_CLASSIC_COMPUTER, SIG_CLASSIC_INPUT, SIG_CLASSIC_WEARABLE,
  SIG_CLASSIC_DEVICE, SIG_CLASSIC_SERIAL,
  // LoRa (sub-GHz node)
  SIG_MESHTASTIC = 200, SIG_LORAWAN_DEVICE, SIG_LORAWAN_JOIN,
  SIG_MESHCORE_CHAT, SIG_MESHCORE_REPEATER, SIG_MESHCORE_ROOM, SIG_MESHCORE_SENSOR, SIG_MESHCORE_NODE,
  // Vehicles
  SIG_TESLA_BLE = 220, SIG_CAR_WIFI, SIG_CAR_BT_NAME, SIG_OBD_DONGLE, SIG_CAR_STEREO,
  // Wi-Fi clients seen in their traffic, and makers known by their address block
  SIG_WIFI_CLIENT_PRIVATE = 240, SIG_WIFI_CLIENT, SIG_PHONE_MAKER_WIFI, SIG_AMAZON_WIFI, SIG_PHONE_HOTSPOT,
};

struct Match {
  uint8_t category = CAT_NONE;
  uint16_t sig_id = SIG_NONE;
  uint8_t conf = 0;
  bool strong = false;
};

Match classifyBle(const BleAdv& a);
Match classifyWifi(const WifiFrame& f);
const char* sigName(uint16_t sig_id);

// Classic Bluetooth device found by inquiry: classify from its Class of Device and
// name. HC-05/HC-06-style serial modules (the usual gas-pump skimmer radio) -> skimmer.
Match classifyClassic(uint32_t class_of_device, const char* name);
Sighting makeClassicSighting(const uint8_t addr[6], const Match& m, int8_t rssi);

// A readable model or display name for devices that don't advertise a name of their
// own: AirPods / Beats model numbers, Microsoft Swift Pair display names. Writes a
// NUL-terminated string to out (up to kMaxName chars), returns its length or 0.
uint8_t bleModelName(const BleAdv& a, char* out);

// Builds the wire record for a classified sighting (hits = 1, age = 0).
Sighting makeBleSighting(const BleAdv& a, const Match& m, uint8_t channel);
Sighting makeWifiSighting(const WifiFrame& f, const Match& m, uint8_t channel, int8_t rssi);

// ---------------------------------------------------------------------------
// Behavioral: deauth / disassoc floods (one instance per node).

class FloodMonitor {
 public:
  explicit FloodMonitor(uint16_t threshold_per_sec = 15, uint32_t holdoff_ms = 10000)
      : threshold_(threshold_per_sec), holdoff_ms_(holdoff_ms) {}
  // Feed every deauth (subtype 12) / disassoc (subtype 10) frame.
  void onFrame(const WifiFrame& f, uint8_t channel, uint32_t now_ms);
  // Feed beacons and probe responses: one BSSID answering as 3+ different network names
  // within a minute is a karma / evil-twin radio (Wi-Fi Pineapple "PineAP" and the like),
  // whatever address it hides behind. Real routers give each network its own BSSID.
  void onApFrame(const WifiFrame& f, uint8_t channel, uint32_t now_ms);
  // Returns true (once per hold-off) when a flood was detected, or an evil twin.
  bool poll(Event& out, uint32_t now_ms);

 private:
  struct Twin { uint8_t bssid[6]; uint32_t first_ms, fired_ms; uint32_t names[5]; uint8_t n; bool used; };
  static constexpr uint8_t kTwins = 8;
  Twin twins_[kTwins] = {};
  Event twin_ev_{};
  bool twin_pending_ = false;
  uint16_t threshold_;
  uint32_t holdoff_ms_;
  uint32_t window_start_ = 0;
  uint16_t deauth_ = 0, disassoc_ = 0;
  Event last_{};
  bool pending_ = false;
  uint32_t last_fire_ = 0;
  bool fired_once_ = false;
};

uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t h = 2166136261u);

}  // namespace ss
