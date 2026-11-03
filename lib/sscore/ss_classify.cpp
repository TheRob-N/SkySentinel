#include "ss_classify.h"

#include <cstdio>
#include <ctype.h>
#include <cstring>

#include <string.h>

#include "ss_channels.h"
#include "ss_oui_makers.h"
#include "ss_sigdata.h"

namespace ss {

using namespace sigdata;

uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t h) {
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

static uint32_t fnvByte(uint32_t h, uint8_t b) { return fnv1a(&b, 1, h); }
static uint32_t fnvU16(uint32_t h, uint16_t v) { h = fnvByte(h, (uint8_t)v); return fnvByte(h, (uint8_t)(v >> 8)); }

template <size_t N, typename T>
static constexpr size_t countof(const T (&)[N]) { return N; }

static bool startsWith(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool containsNoCase(const char* s, const char* word) {
  const size_t n = strlen(word);
  for (; *s; ++s) {
    size_t i = 0;
    while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)word[i])) ++i;
    if (i == n) return true;
  }
  return false;
}
static bool startsWithNoCase(const char* s, const char* prefix) {
  for (; *prefix; ++s, ++prefix) {
    if (!*s) return false;
    char a = *s, b = *prefix;
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    if (a != b) return false;
  }
  return true;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "Flock-7F68FF" on BSSID 70:C9:4E:7F:68:FF: the name ends in the radio's own address.
static bool ssidEndsInMac(const char* ssid, uint8_t len, const uint8_t mac[6]) {
  if (len < 6) return false;
  const char* t = ssid + len - 6;
  for (int i = 0; i < 3; ++i) {
    const int hi = hexVal(t[2 * i]), lo = hexVal(t[2 * i + 1]);
    if (hi < 0 || lo < 0 || ((hi << 4) | lo) != mac[3 + i]) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// BLE parsing

bool parseBleAdv(const uint8_t* p, size_t len, BleAdv& a) {
  a.has_flags = false; a.has_company = false; a.company = 0xFFFF; a.mfg = nullptr; a.mfg_len = 0;
  a.n_uuid16 = 0; a.n_uuid128 = 0; a.n_svc = 0; a.name[0] = '\0'; a.name_len = 0; a.has_tx_power = false;
  a.has_appearance = false; a.appearance = 0;

  // The fingerprint covers what a device model/state always sends the same way:
  // AD structure types and lengths, company id, Apple message type, service UUIDs.
  // It skips names and rolling payload bytes.
  uint32_t fp = fnvByte(2166136261u, (uint8_t)(bleIsRandom(a) ? 1 : 0));
  size_t i = 0;
  bool ok = true;
  while (i < len) {
    const uint8_t l = p[i];
    if (l == 0) break;  // padding
    if (i + 1 + l > len) { ok = false; break; }
    const uint8_t type = p[i + 1];
    const uint8_t* d = p + i + 2;
    const uint8_t dl = (uint8_t)(l - 1);
    fp = fnvByte(fp, type);
    switch (type) {
      case 0x01:  // flags
        if (dl >= 1) { a.has_flags = true; a.flags = d[0]; fp = fnvByte(fp, d[0]); }
        break;
      case 0x02: case 0x03:  // 16-bit service UUIDs
        for (uint8_t k = 0; k + 1 < dl; k += 2) {
          const uint16_t u = (uint16_t)(d[k] | (d[k + 1] << 8));
          if (a.n_uuid16 < countof(a.uuid16)) a.uuid16[a.n_uuid16++] = u;
          fp = fnvU16(fp, u);
        }
        break;
      case 0x06: case 0x07:  // 128-bit service UUIDs
        a.n_uuid128 = (uint8_t)(a.n_uuid128 + dl / 16);
        fp = fnv1a(d, dl, fp);
        break;
      case 0x08: case 0x09: {  // shortened / complete local name
        const uint8_t n = dl > kMaxName ? (uint8_t)kMaxName : dl;
        if (type == 0x09 || a.name_len == 0) {
          memcpy(a.name, d, n);
          a.name[n] = '\0';
          a.name_len = n;
        }
        break;
      }
      case 0x19:  // appearance
        if (dl >= 2) {
          a.has_appearance = true;
          a.appearance = (uint16_t)(d[0] | (d[1] << 8));
          fp = fnvU16(fp, a.appearance);
        }
        break;
      case 0x0A:  // TX power
        if (dl >= 1) { a.has_tx_power = true; a.tx_power = (int8_t)d[0]; fp = fnvByte(fp, d[0]); }
        break;
      case 0x16:  // service data, 16-bit UUID
        if (dl >= 2) {
          const uint16_t u = (uint16_t)(d[0] | (d[1] << 8));
          if (a.n_svc < countof(a.svc)) {
            a.svc[a.n_svc].uuid = u;
            a.svc[a.n_svc].data = d + 2;
            a.svc[a.n_svc].len = (uint8_t)(dl - 2);
            ++a.n_svc;
          }
          fp = fnvU16(fp, u);
          fp = fnvByte(fp, dl);
        }
        break;
      case 0xFF:  // manufacturer specific
        if (dl >= 2) {
          a.has_company = true;
          a.company = (uint16_t)(d[0] | (d[1] << 8));
          a.mfg = d + 2;
          a.mfg_len = (uint8_t)(dl - 2);
          fp = fnvU16(fp, a.company);
          fp = fnvByte(fp, dl);
          if (a.company == kCoApple && a.mfg_len >= 1) fp = fnvByte(fp, a.mfg[0]);
        }
        break;
      default:
        fp = fnvByte(fp, dl);
        break;
    }
    i += 1 + l;
  }
  a.fp = fp ? fp : 1;
  return ok;
}

bool bleIsRandom(const BleAdv& a) { return a.addr_type != SS_ADDR_PUBLIC; }
bool bleIsStaticRandom(const BleAdv& a) { return bleIsRandom(a) && (a.addr[0] & 0xC0) == 0xC0; }

static bool hasUuid16(const BleAdv& a, uint16_t u) {
  for (uint8_t i = 0; i < a.n_uuid16; ++i) if (a.uuid16[i] == u) return true;
  for (uint8_t i = 0; i < a.n_svc; ++i) if (a.svc[i].uuid == u) return true;
  return false;
}

static const BleSvcData* svcData(const BleAdv& a, uint16_t u) {
  for (uint8_t i = 0; i < a.n_svc; ++i) if (a.svc[i].uuid == u) return &a.svc[i];
  return nullptr;
}

template <size_t N>
static const Oui* matchOui(const Oui (&list)[N], const uint8_t mac[6]) {
  for (size_t i = 0; i < N; ++i)
    if (memcmp(list[i].b, mac, 3) == 0) return &list[i];
  return nullptr;
}

static Match mk(uint8_t cat, uint16_t sig, uint8_t conf, bool strong) {
  Match m;
  m.category = cat; m.sig_id = sig; m.conf = conf; m.strong = strong;
  return m;
}

// ---------------------------------------------------------------------------
// Vendor tables (sources in ss_sigdata.h)

#define OS(a, b, c, cat, sig, conf, strong) {{a, b, c}, cat, sig, conf, strong}
static const OuiSig kOuiSigs[] = {
  // ALPR: Flock Safety's own registered block, and Ring (Amazon), whose cameras feed
  // police requests (ouispy-detector listed them; low weight, never an alert alone).
  OS(0xB4, 0x1E, 0x52, CAT_FLOCK, SIG_FLOCK_OWN_OUI, 80, true),
  OS(0x18, 0x7F, 0x88, CAT_FLOCK, SIG_RING_OUI, 35, false), OS(0x34, 0x3E, 0xA4, CAT_FLOCK, SIG_RING_OUI, 35, false),
  OS(0x54, 0xE0, 0x19, CAT_FLOCK, SIG_RING_OUI, 35, false), OS(0x5C, 0x47, 0x5E, CAT_FLOCK, SIG_RING_OUI, 35, false),
  OS(0x64, 0x9A, 0x63, CAT_FLOCK, SIG_RING_OUI, 35, false), OS(0x90, 0x48, 0x6C, CAT_FLOCK, SIG_RING_OUI, 35, false),
  OS(0x9C, 0x76, 0x13, CAT_FLOCK, SIG_RING_OUI, 35, false),
  // Body cams
  OS(0x00, 0x25, 0xDF, CAT_BODYCAM, SIG_AXON_OUI, 75, false),          // Taser International (Axon)
  OS(0x00, 0x23, 0xBD, CAT_BODYCAM, SIG_DIGITAL_ALLY_OUI, 75, false),  // Digital Ally
  OS(0x00, 0x1D, 0x96, CAT_BODYCAM, SIG_WATCHGUARD_OUI, 75, false),    // WatchGuard Video (Motorola)
  OS(0x00, 0x09, 0xBC, CAT_BODYCAM, SIG_UTILITY_OUI, 70, false),       // Utility Inc (BodyWorn)
  OS(0x00, 0x16, 0xED, CAT_BODYCAM, SIG_UTILITY_OUI, 70, false),
  OS(0x48, 0x46, 0x8D, CAT_BODYCAM, SIG_ZEPCAM_OUI, 70, false),        // Zepcam
  // Motorola Solutions: police / fire radios as much as body cams
  OS(0x00, 0x04, 0x7D, CAT_BODYCAM, SIG_MOTO_SOLUTIONS_OUI, 50, false),
  OS(0x00, 0x18, 0x85, CAT_BODYCAM, SIG_MOTO_SOLUTIONS_OUI, 50, false),
  OS(0x00, 0x1F, 0x92, CAT_BODYCAM, SIG_MOTO_SOLUTIONS_OUI, 50, false),
  OS(0x4C, 0xCC, 0x34, CAT_BODYCAM, SIG_MOTO_SOLUTIONS_OUI, 50, false),
  OS(0x10, 0x74, 0x6F, CAT_BODYCAM, SIG_MOTO_SOLUTIONS_OUI, 50, false),
  OS(0xB8, 0xE2, 0x8C, CAT_BODYCAM, SIG_MOTO_SOLUTIONS_OUI, 50, false),
  // Glasses
  OS(0x98, 0x59, 0x49, CAT_GLASSES, SIG_LUXOTTICA_OUI, 80, true),      // Luxottica (Ray-Ban / Oakley Meta)
  OS(0x98, 0xDA, 0x92, CAT_GLASSES, SIG_VUZIX_OUI, 70, false),         // Vuzix
  // Meta Platforms Technologies: Ray-Ban Meta, but Quest headsets too
  OS(0x80, 0xF3, 0xEF, CAT_GLASSES, SIG_META_OUI, 50, false), OS(0x84, 0x57, 0xF7, CAT_GLASSES, SIG_META_OUI, 50, false),
  OS(0x88, 0x25, 0x08, CAT_GLASSES, SIG_META_OUI, 50, false), OS(0x94, 0xF9, 0x29, CAT_GLASSES, SIG_META_OUI, 50, false),
  OS(0xB4, 0x17, 0xA8, CAT_GLASSES, SIG_META_OUI, 50, false), OS(0xC0, 0xDD, 0x8A, CAT_GLASSES, SIG_META_OUI, 50, false),
  OS(0xCC, 0xA1, 0x74, CAT_GLASSES, SIG_META_OUI, 50, false), OS(0xD0, 0xB3, 0xC2, CAT_GLASSES, SIG_META_OUI, 50, false),
  OS(0xD4, 0xD6, 0x59, CAT_GLASSES, SIG_META_OUI, 50, false),
  // Drones. DJI also makes Osmo cameras and mics; Parrot's two oldest blocks were car kits.
  OS(0x60, 0x60, 0x1F, CAT_DRONE, SIG_DJI_OUI, 60, false), OS(0x34, 0xD2, 0x62, CAT_DRONE, SIG_DJI_OUI, 60, false),
  OS(0x48, 0x1C, 0xB9, CAT_DRONE, SIG_DJI_OUI, 60, false), OS(0x58, 0xB8, 0x58, CAT_DRONE, SIG_DJI_OUI, 60, false),
  OS(0xE4, 0x7A, 0x2C, CAT_DRONE, SIG_DJI_OUI, 60, false),
  OS(0x90, 0x03, 0xB7, CAT_DRONE, SIG_PARROT_OUI, 60, false), OS(0x90, 0x3A, 0xE6, CAT_DRONE, SIG_PARROT_OUI, 60, false),
  OS(0xA0, 0x14, 0x3D, CAT_DRONE, SIG_PARROT_OUI, 60, false), OS(0x00, 0x12, 0x1C, CAT_DRONE, SIG_PARROT_OUI, 40, false),
  OS(0x00, 0x26, 0x7E, CAT_DRONE, SIG_PARROT_OUI, 40, false),
  OS(0x38, 0x1D, 0x14, CAT_DRONE, SIG_SKYDIO_OUI, 75, false),          // Skydio (police DFR drones)
};
#undef OS

static const IdSig kCompanySigs[] = {
  {0x034D, CAT_BODYCAM, SIG_TASER_MFGR, 85, true},          // TASER International (Axon)
  {0x04EC, CAT_BODYCAM, SIG_MOTO_SOLUTIONS, 60, false},     // Motorola Solutions (not 0x0008, phones)
  {0x0D53, CAT_GLASSES, SIG_RAYBAN_META, 85, true},         // Luxottica
  {0x03C2, CAT_GLASSES, SIG_SNAP_SPECTACLES, 80, true},     // Snapchat
  {0x058E, CAT_GLASSES, SIG_META_MFGR, 65, true},           // Meta Platforms Technologies (glasses, Quest)
  {0x01AB, CAT_GLASSES, SIG_META_INC, 45, false},           // Meta Platforms, Inc.
  {0x060C, CAT_GLASSES, SIG_VUZIX, 75, true},               // Vuzix
  {0x08AA, CAT_DRONE, SIG_DJI_MFGR, 65, false},             // SZ DJI Technology
};

static const IdSig kUuidSigs[] = {
  {0xFC81, CAT_BODYCAM, SIG_AXON_UUID, 85, true},   // Axon Enterprise
  {0xFE6B, CAT_BODYCAM, SIG_AXON_UUID, 85, true},   // TASER International
  {0xFE6C, CAT_BODYCAM, SIG_AXON_UUID, 85, true},
  {0xFE04, CAT_BODYCAM, SIG_MOTO_SOLUTIONS, 60, false},
  {0xFD8E, CAT_BODYCAM, SIG_MOTO_SOLUTIONS, 60, false},
  {0xFD5F, CAT_GLASSES, SIG_META_UUID, 75, true},   // Meta Platforms Technologies (Ray-Ban Meta, Quest)
  {0xFE45, CAT_GLASSES, SIG_SNAP_SPECTACLES, 80, true},
};

template <size_t N>
static const OuiSig* matchOuiSig(const OuiSig (&list)[N], const uint8_t mac[6]) {
  for (size_t i = 0; i < N; ++i)
    if (memcmp(list[i].b, mac, 3) == 0) return &list[i];
  return nullptr;
}

// ---------------------------------------------------------------------------
// BLE classification: surveillance first, then consumer.

Match classifyBle(const BleAdv& a) {
  const bool pub = !bleIsRandom(a);

  // Flock / Raven
  for (const char* n : kFlockBleNames)
    if (a.name_len && strstr(a.name, n))
      return mk(CAT_FLOCK, SIG_FLOCK_BLE_NAME, strcmp(n, "Penguin") == 0 ? 60 : 85, strcmp(n, "Penguin") != 0);
  {
    uint8_t raven = 0;
    for (uint16_t u : kRavenUuids) if (hasUuid16(a, u)) ++raven;
    if (raven >= 2) return mk(CAT_FLOCK, SIG_RAVEN, 90, true);
    if (raven == 1) return mk(CAT_FLOCK, SIG_RAVEN_PARTIAL, 45, false);
  }
  if (a.has_company && a.company == kFlockBleCompany) return mk(CAT_FLOCK, SIG_FLOCK_BLE_MFGR, 70, false);

  // Axon body cams
  if (a.name_len && strstr(a.name, "Axon")) return mk(CAT_BODYCAM, SIG_AXON_NAME, 80, true);

  // Vehicles: a Tesla's phone-key radio is named "S" + 16 hex digits (from its VIN) + a
  // letter; car head units and OBD dongles go by brand or stock names.
  if (a.name_len == 18 && a.name[0] == 'S') {
    bool hex = true;
    for (int i = 1; i <= 16 && hex; ++i) hex = isxdigit((unsigned char)a.name[i]) != 0;
    const char e = a.name[17];
    if (hex && (e == 'C' || e == 'D' || e == 'R' || e == 'P')) return mk(CAT_VEHICLE, SIG_TESLA_BLE, 90, true);
  }
  if (a.name_len) {
    for (const char* n : kObdNames) if (startsWithNoCase(a.name, n)) return mk(CAT_VEHICLE, SIG_OBD_DONGLE, 70, false);
    for (const char* n : kCarBtPrefixes) if (startsWith(a.name, n)) return mk(CAT_VEHICLE, SIG_CAR_BT_NAME, 60, false);
  }

  // Drones: Remote ID is a broadcast standard, the strongest thing here
  if (hasUuid16(a, kRemoteIdBleUuid) || svcData(a, kRemoteIdBleUuid)) return mk(CAT_DRONE, SIG_REMOTE_ID_BLE, 95, true);

  // Body cams, glasses, drones by assigned service UUID, then by maker
  for (const IdSig& u : kUuidSigs)
    if (hasUuid16(a, u.id) || svcData(a, u.id)) return mk(u.cat, u.sig, u.conf, u.strong);
  if (a.has_company)
    for (const IdSig& c : kCompanySigs)
      if (a.company == c.id) return mk(c.cat, c.sig, c.conf, c.strong);

  if (pub) {
    if (const OuiSig* o = matchOuiSig(kOuiSigs, a.addr)) return mk(o->cat, o->sig, o->conf, o->strong);
    if (const Oui* o = matchOui(kFlockBleOuis, a.addr))
      return mk(CAT_FLOCK, SIG_FLOCK_BLE_OUI, o->generic ? 20 : 50, false);
  }

  // Trackers
  if (a.has_company && a.company == kCoApple && a.mfg_len >= 2 && a.mfg[0] == kAppleFindMy) {
    // Length 25 = full public key, sent while separated from the owner.
    if (a.mfg[1] >= 0x19) return mk(CAT_TRACKER, SIG_FINDMY_SEPARATED, 80, true);
    return mk(CAT_TRACKER, SIG_FINDMY_NEAR_OWNER, 40, false);
  }
  if (hasUuid16(a, kSmartTagUuid)) return mk(CAT_TRACKER, SIG_SMARTTAG, 85, true);
  for (uint16_t u : kTileUuids) if (hasUuid16(a, u)) return mk(CAT_TRACKER, SIG_TILE, 85, true);
  if (const BleSvcData* s = svcData(a, kEddystoneUuid)) {
    if (s->len >= 1 && (s->data[0] == 0x40 || s->data[0] == 0x41)) return mk(CAT_TRACKER, SIG_GOOGLE_FMDN, 85, true);
  }
  for (uint16_t u : kChipoloUuids) if (hasUuid16(a, u) || svcData(a, u)) return mk(CAT_TRACKER, SIG_CHIPOLO, 85, true);
  if (a.has_company && a.company == kCoChipolo) return mk(CAT_TRACKER, SIG_CHIPOLO, 75, true);
  if (hasUuid16(a, kDultUuid)) return mk(CAT_TRACKER, SIG_DULT, 70, false);

  // Pentest gear
  if (a.name_len && startsWith(a.name, "Flipper ")) return mk(CAT_PENTEST, SIG_FLIPPER, 85, true);

  // Skimmer-style serial modules
  for (const char* n : kSerialModuleNames)
    if (a.name_len && startsWith(a.name, n)) return mk(CAT_SKIMMER, SIG_SERIAL_BLE_MODULE, 40, false);

  // What the device says it is (GAP appearance), when it says so.
  const uint8_t appCat = a.has_appearance ? (uint8_t)(a.appearance >> 6) : 0;
  const uint8_t appSub = a.has_appearance ? (uint8_t)(a.appearance & 0x3F) : 0;

  // Audio
  if (a.has_company && a.company == kCoApple && a.mfg_len >= 1 && a.mfg[0] == kAppleProximityPairing)
    return mk(CAT_AUDIO, SIG_APPLE_AUDIO, 80, true);
  if (appCat == kAppearWearableAudio) {
    if (appSub == 1) return mk(CAT_AUDIO, SIG_SAYS_EARBUDS, 75, false);
    if (appSub == 2) return mk(CAT_AUDIO, SIG_SAYS_HEADSET, 75, false);
    return mk(CAT_AUDIO, SIG_SAYS_HEADPHONES, 75, false);
  }
  if (appCat == kAppearHearingAid) return mk(CAT_AUDIO, SIG_SAYS_HEARING_AID, 75, false);
  if (appCat == kAppearAudioSink) return mk(CAT_AUDIO, SIG_SAYS_SPEAKER, 70, false);
  if (a.has_company && a.company == kCoMicrosoft && a.mfg_len >= 2 && a.mfg[0] == kSwiftPairBeacon) {
    // Swift Pair: a device in pairing mode asking Windows to offer it. With a
    // Classic Bluetooth part it carries the Class of Device; major class 4 = audio/video.
    const uint8_t sub = a.mfg[1];
    if ((sub == 1 || sub == 2) && a.mfg_len >= 12) {
      const uint32_t cod = (uint32_t)a.mfg[9] | ((uint32_t)a.mfg[10] << 8) | ((uint32_t)a.mfg[11] << 16);
      if (((cod >> 8) & 0x1F) == 0x04) return mk(CAT_AUDIO, SIG_SWIFT_PAIR_AUDIO, 70, false);
    }
    return mk(CAT_OTHER_BLE, SIG_SWIFT_PAIR, 40, false);
  }
  for (uint16_t u : kLeAudioUuids) if (hasUuid16(a, u)) return mk(CAT_AUDIO, SIG_LE_AUDIO, 65, false);
  if (hasUuid16(a, kFastPairUuid)) return mk(CAT_AUDIO, SIG_FAST_PAIR, 60, false);
  if (a.has_company) {
    switch (a.company) {
      case kCoBose:   return mk(CAT_AUDIO, SIG_BOSE, 70, false);
      case kCoHarman: return mk(CAT_AUDIO, SIG_HARMAN, 60, false);
      case kCoSony:   return mk(CAT_AUDIO, SIG_SONY, 50, false);
      case kCoGarmin: return mk(CAT_WEARABLE, SIG_GARMIN, 80, false);
      case kCoPolar:  return mk(CAT_WEARABLE, SIG_POLAR, 80, false);
      default: break;
    }
  }
  if (hasUuid16(a, kHeartRateUuid)) return mk(CAT_WEARABLE, SIG_HEART_RATE, 60, false);
  if (appCat == kAppearWatch) return mk(CAT_WEARABLE, SIG_SAYS_WATCH, 70, false);
  if (appCat == kAppearHeartRate || appCat == kAppearRunWalk || appCat == kAppearCycling || appCat == kAppearPulseOx)
    return mk(CAT_WEARABLE, SIG_SAYS_FITNESS, 60, false);
  if (appCat == kAppearPhone) return mk(CAT_PHONE, SIG_SAYS_PHONE, 60, false);
  if (appCat == kAppearComputer) return mk(CAT_OTHER_BLE, SIG_SAYS_COMPUTER, 50, false);
  if (appCat == kAppearHid) return mk(CAT_OTHER_BLE, SIG_SAYS_INPUT, 50, false);
  // No self-description: go by maker when the maker only makes one kind of thing.
  if (a.has_company) {
    for (uint16_t co : kAudioMakers) if (a.company == co) return mk(CAT_AUDIO, SIG_AUDIO_MAKER, 55, false);
    for (uint16_t co : kWearableMakers) if (a.company == co) return mk(CAT_WEARABLE, SIG_WEARABLE_MAKER, 60, false);
  }

  // Phones and computers
  if (a.has_company) {
    if (a.company == kCoApple && a.mfg_len >= 1) {
      const uint8_t t = a.mfg[0];
      if (t == kAppleNearbyInfo || t == kAppleHandoff || t == kAppleNearbyAction)
        return mk(CAT_PHONE, SIG_APPLE_DEVICE, 50, false);
    }
    // Samsung and Google adverts come from phones but also TVs, fridges, speakers and
    // Chromecasts. Phones hide behind rotating random addresses; a fixed (public)
    // address is a TV or appliance.
    if (a.company == kCoSamsung) return mk(pub ? CAT_OTHER_BLE : CAT_PHONE, SIG_SAMSUNG_DEVICE, 40, false);
    if (a.company == kCoGoogle) return mk(pub ? CAT_OTHER_BLE : CAT_PHONE, SIG_GOOGLE_DEVICE, 40, false);
    if (a.company == kCoMicrosoft) return mk(CAT_OTHER_BLE, SIG_MICROSOFT, 30, false);
  }
  return mk(CAT_OTHER_BLE, SIG_GENERIC_BLE, 10, false);
}

// ---------------------------------------------------------------------------
// Classic Bluetooth

Match classifyClassic(uint32_t cod, const char* name) {
  if (name && name[0]) {
    for (const char* n : kObdNames) if (startsWithNoCase(name, n)) return mk(CAT_VEHICLE, SIG_OBD_DONGLE, 70, false);
    for (const char* n : kClassicSerialNames)
      if (startsWith(name, n)) return mk(CAT_SKIMMER, SIG_CLASSIC_SERIAL, 55, false);
    for (const char* n : kCarBtPrefixes) if (startsWith(name, n)) return mk(CAT_VEHICLE, SIG_CAR_BT_NAME, 65, false);
  }
  const uint8_t major = (uint8_t)((cod >> 8) & 0x1F);
  const uint8_t minor = (uint8_t)((cod >> 2) & 0x3F);
  switch (major) {
    case 0x04:  // audio / video
      switch (minor) {
        case 0x01: case 0x02: return mk(CAT_AUDIO, SIG_CLASSIC_HEADSET, 70, false);  // headset, hands-free
        case 0x06: return mk(CAT_AUDIO, SIG_CLASSIC_HEADPHONES, 70, false);
        case 0x05: return mk(CAT_AUDIO, SIG_CLASSIC_SPEAKER, 65, false);
        case 0x08: return mk(CAT_VEHICLE, SIG_CAR_STEREO, 70, false);   // car audio: a car
        default: return mk(CAT_AUDIO, SIG_CLASSIC_AUDIO, 60, false);
      }
    case 0x02: return mk(CAT_PHONE, SIG_CLASSIC_PHONE, 65, false);
    case 0x07: return mk(CAT_WEARABLE, SIG_CLASSIC_WEARABLE, 60, false);
    case 0x01: return mk(CAT_OTHER_BLE, SIG_CLASSIC_COMPUTER, 50, false);
    case 0x05: return mk(CAT_OTHER_BLE, SIG_CLASSIC_INPUT, 50, false);
    default: return mk(CAT_OTHER_BLE, SIG_CLASSIC_DEVICE, 20, false);
  }
}

Sighting makeClassicSighting(const uint8_t addr[6], const Match& m, int8_t rssi) {
  Sighting s;
  memcpy(s.addr, addr, 6);
  s.kind = KIND_BT_CLASSIC;
  s.channel = 0;
  s.rssi_max = s.rssi_last = rssi;
  s.hits = 1;
  s.category = m.category;
  s.sig_id = m.sig_id;
  s.conf = m.conf;
  s.flags = m.strong ? SF_STRONG_MATCH : 0;  // Classic addresses are fixed: no rotation
  s.company = 0xFFFF;
  s.fp = 0;
  return s;
}

// ---------------------------------------------------------------------------
// Model / display names

static uint8_t copyName(char* out, const char* s) {
  const size_t n = strnlen(s, kMaxName);
  memcpy(out, s, n);
  out[n] = '\0';
  return (uint8_t)n;
}

uint8_t bleModelName(const BleAdv& a, char* out) {
  out[0] = '\0';
  if (!a.has_company) return 0;
  // AirPods / Beats proximity pairing: [0x07][len][prefix][model hi][model lo]...
  if (a.company == kCoApple && a.mfg_len >= 5 && a.mfg[0] == kAppleProximityPairing) {
    const uint16_t model = (uint16_t)((a.mfg[3] << 8) | a.mfg[4]);
    for (const auto& m : kAppleAudioModels)
      if (m.id == model) return copyName(out, m.name);
    return 0;
  }
  // Microsoft Connected Devices beacon: what kind of Windows / Xbox device it is.
  if (a.company == kCoMicrosoft && a.mfg_len >= 2 && a.mfg[0] == 0x01) {
    static const char* const kTypes[] = {nullptr, "Xbox", nullptr, nullptr, nullptr, nullptr, "iPhone (Microsoft apps)",
                                         "iPad (Microsoft apps)", "Android (Microsoft apps)", "Windows desktop", nullptr,
                                         "Windows phone", "Linux (Microsoft apps)", "Windows IoT", "Surface Hub",
                                         "Windows laptop", "Windows tablet"};
    const uint8_t t = a.mfg[1] & 0x1F;
    if (t < sizeof(kTypes) / sizeof(kTypes[0]) && kTypes[t]) return copyName(out, kTypes[t]);
    return 0;
  }
  // Swift Pair: display name follows the fixed fields (UTF-8, to the end of the data).
  if (a.company == kCoMicrosoft && a.mfg_len >= 3 && a.mfg[0] == kSwiftPairBeacon) {
    const uint8_t sub = a.mfg[1];
    const uint8_t at = sub == 0 ? 3 : 12;  // id, sub, rssi [, BR/EDR addr(6), CoD(3)]
    if (a.mfg_len <= at) return 0;
    uint8_t n = 0;
    for (uint8_t i = at; i < a.mfg_len && n < kMaxName; ++i) {
      const uint8_t c = a.mfg[i];
      if (c == 0) break;
      if (c < 0x20 || c == 0x7F) return 0;  // not text: wrong layout, don't show garbage
      out[n++] = (char)c;
    }
    out[n] = '\0';
    return n;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Wi-Fi parsing

static void mac6(uint8_t* out, const uint8_t* in) { memcpy(out, in, 6); }

bool parseWifiFrame(const uint8_t* f, size_t len, WifiFrame& w) {
  w = WifiFrame();
  if (len < 24) return false;
  w.type = (uint8_t)((f[0] >> 2) & 0x03);
  w.subtype = (uint8_t)((f[0] >> 4) & 0x0F);
  mac6(w.addr1, f + 4);
  mac6(w.addr2, f + 10);
  mac6(w.addr3, f + 16);
  w.seq = (uint16_t)((f[22] | (f[23] << 8)) >> 4);
  w.to_ds = (f[1] & 0x01) != 0;
  w.from_ds = (f[1] & 0x02) != 0;

  if (w.type == 2) { w.kind = KIND_WIFI_DATA; return true; }
  if (w.type != 0) { w.kind = KIND_NONE; return false; }

  size_t ie = 24;
  if (f[1] & 0x80) ie += 4;  // +HTC field
  switch (w.subtype) {
    case 8: w.kind = KIND_WIFI_BEACON; ie += 12; break;
    case 5: w.kind = KIND_WIFI_PROBE_RESP; ie += 12; break;
    case 4: w.kind = KIND_WIFI_PROBE_REQ; break;
    default: w.kind = KIND_WIFI_OTHER; return true;  // deauth, disassoc, auth, action...
  }
  if (ie > len) return false;
  if (w.kind != KIND_WIFI_PROBE_REQ && ie >= 12) {
    const uint8_t* t = f + ie - 12;   // fixed fields: timestamp (8), interval, capability
    for (int i = 7; i >= 0; --i) w.tsf = (w.tsf << 8) | t[i];
  }

  const bool probe = w.kind == KIND_WIFI_PROBE_REQ;
  const bool ap = !probe;
  const bool privacy = ap && ie >= 2 && (f[ie - 2] & 0x10);   // capability info, last fixed field
  bool rsn = false, wpa1 = false, akmPsk = false, akmSae = false, akm1x = false, akmOwe = false;
  uint32_t fp = 2166136261u;
  while (ie + 2 <= len) {
    const uint8_t id = f[ie];
    const uint8_t l = f[ie + 1];
    if (ie + 2 + l > len) break;  // truncated trailing element: keep what we have
    const uint8_t* d = f + ie + 2;
    if (probe) fp = fnvByte(fp, id);
    switch (id) {
      case 0:  // SSID (not in fingerprint: varies with what the phone is looking for)
        w.has_ssid = true;
        w.ssid_len = l > 32 ? 32 : l;
        memcpy(w.ssid, d, w.ssid_len);
        w.ssid[w.ssid_len] = '\0';
        break;
      case 3:  // DS parameter set (current channel; not in fingerprint)
        if (l >= 1) w.ds_channel = d[0];
        break;
      case 1: case 50: case 45: case 127: case 191:  // rates, ext rates, HT, ext caps, VHT
        if (probe) fp = fnv1a(d, l, fp);
        if (ap && id == 45 && w.gen < 4) w.gen = 4;
        if (ap && id == 191 && w.gen < 5) w.gen = 5;
        break;
      case 61:  // HT operation: secondary channel offset -> 40 MHz
        if (ap && l >= 2 && (d[1] & 0x03) && (d[1] & 0x04) && w.width < 40) w.width = 40;
        if (probe) fp = fnvByte(fp, l);
        break;
      case 192:  // VHT operation: channel width
        if (ap && l >= 3) {
          if (d[0] == 1) w.width = d[2] ? 160 : (w.width < 80 ? 80 : w.width);
          else if (d[0] == 2 || d[0] == 3) w.width = 160;
        }
        if (probe) fp = fnvByte(fp, l);
        break;
      case 11:  // BSS Load: stations, channel utilization
        if (ap && l >= 3) { w.sta = (uint16_t)(d[0] | (d[1] << 8)); w.util = d[2]; }
        if (probe) fp = fnvByte(fp, l);
        break;
      case 48:  // RSN: AKM suites tell WPA2 / WPA3 / Enterprise / OWE
        if (probe) { fp = fnvByte(fp, l); break; }
        if (l >= 8) {
          rsn = true;
          size_t o = 6;   // version 2 + group cipher 4
          const uint16_t npw = (uint16_t)(d[o] | (d[o + 1] << 8));
          o += 2 + 4u * npw;
          if (o + 2 <= l) {
            const uint16_t nakm = (uint16_t)(d[o] | (d[o + 1] << 8));
            o += 2;
            for (uint16_t k = 0; k < nakm && o + 4 <= l; ++k, o += 4) {
              if (d[o] != 0x00 || d[o + 1] != 0x0F || d[o + 2] != 0xAC) continue;
              const uint8_t t = d[o + 3];
              if (t == 2 || t == 4 || t == 6) akmPsk = true;
              else if (t == 8 || t == 9 || t == 24 || t == 25) akmSae = true;
              else if (t == 1 || t == 3 || t == 5 || t == 11 || t == 12 || t == 13) akm1x = true;
              else if (t == 18) akmOwe = true;
            }
          }
        }
        break;
      case 221:  // vendor specific: OUI + type only
        if (l >= 4) {
          const uint32_t v = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
          if (w.n_vendor < countof(w.vendor)) w.vendor[w.n_vendor++] = v;
          if (v == kRemoteIdWifiVendor) { w.remote_id = true; w.rid = d + 4; w.rid_len = (uint8_t)(l - 4); }
          if ((v >> 8) == 0x0017F2) w.apple_ie = true;
          if (probe) fp = fnv1a(d, 4, fp);
          if (ap && v == 0x0050F201) wpa1 = true;
          if (ap && v == 0x0050F204 && !w.wps[0]) {
            // WPS: big-endian TLVs. Manufacturer, model name, model number -> one short string.
            char man[24] = "", model[24] = "", num[16] = "";
            for (size_t o = 4; o + 4 <= l;) {
              const uint16_t t = (uint16_t)((d[o] << 8) | d[o + 1]), n = (uint16_t)((d[o + 2] << 8) | d[o + 3]);
              o += 4;
              if (o + n > l) break;
              char* dst = t == 0x1021 ? man : t == 0x1023 ? model : t == 0x1024 ? num : nullptr;
              const size_t cap = t == 0x1024 ? sizeof(num) : sizeof(man);
              if (dst) {
                size_t k = 0;
                for (size_t i = 0; i < n && k + 1 < cap; ++i) {
                  const char c = (char)d[o + i];
                  if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) dst[k++] = c;
                }
                dst[k] = '\0';
              }
              o += n;
            }
            size_t k = 0;
            auto put = [&](const char* src) {
              if (!src[0]) return;
              if (k && k + 1 < sizeof(w.wps)) w.wps[k++] = ' ';
              for (; *src && k + 1 < sizeof(w.wps); ++src) w.wps[k++] = *src;
              w.wps[k] = '\0';
            };
            put(man);
            put(model);
            if (strcmp(num, model) != 0) put(num);
          }
        }
        break;
      case 255:  // element ID extension (HE capabilities = 35, EHT = 108)
        if (probe && l >= 1) {
          fp = fnvByte(fp, d[0]);
          if (d[0] == 35) fp = fnv1a(d, l, fp);
        }
        if (ap && l >= 1) {
          if (d[0] == 35 && w.gen < 6) w.gen = 6;
          if (d[0] == 108 && w.gen < 7) w.gen = 7;
        }
        break;
      default:
        if (probe) fp = fnvByte(fp, l);
        break;
    }
    ie += 2 + l;
  }
  w.fp = probe ? (fp ? fp : 1) : 0;
  if (ap) {
    if (akmSae && akmPsk) w.sec = SEC_WPA2_3;
    else if (akmSae) w.sec = SEC_WPA3;
    else if (akmOwe) w.sec = SEC_OWE;
    else if (akm1x) w.sec = SEC_ENTERPRISE;
    else if (akmPsk || rsn) w.sec = SEC_WPA2;
    else if (wpa1) w.sec = SEC_WPA;
    else if (privacy) w.sec = SEC_WEP;
    else w.sec = SEC_OPEN;
  }
  return true;
}

const char* apSecName(uint8_t s) {
  static const char* const n[] = {"", "open", "WEP", "WPA", "WPA2", "WPA3", "WPA2/WPA3", "Enterprise", "Enhanced Open (OWE)"};
  return s < sizeof(n) / sizeof(n[0]) ? n[s] : "";
}

// ---------------------------------------------------------------------------
// Wi-Fi classification

Match classifyWifi(const WifiFrame& f) {
  const uint8_t* tx = f.addr2;
  const bool global = !macIsLocallyAdministered(tx);

  if (memcmp(tx, kPwnagotchiMac, 6) == 0) return mk(CAT_PENTEST, SIG_PWNAGOTCHI, 95, true);
  if (f.remote_id) return mk(CAT_DRONE, SIG_REMOTE_ID_WIFI, 95, true);
  if (f.has_ssid && f.ssid_len) {
    // Seen in the wild as both "Flock-7F68FF" and "FLOCK-7F68FF".
    for (const char* p : kFlockSsidPrefixes)
      if (startsWithNoCase(f.ssid, p))
        return mk(CAT_FLOCK, SIG_FLOCK_SSID, ssidEndsInMac(f.ssid, f.ssid_len, tx) ? 95 : 85, true);
  }
  for (uint8_t i = 0; i < f.n_vendor; ++i)
    if ((f.vendor[i] >> 8) == kDjiDroneIdOui) return mk(CAT_DRONE, SIG_DJI_DRONEID, 90, true);
  if (f.has_ssid && f.ssid_len && f.kind != KIND_WIFI_PROBE_REQ) {
    for (const char* p : kDroneSsidPrefixes)
      if (startsWithNoCase(f.ssid, p)) return mk(CAT_DRONE, SIG_DRONE_SSID, 75, false);
    if (f.kind != KIND_WIFI_PROBE_REQ) {
      for (const char* p : kCarWifiPrefixes)
        if (startsWithNoCase(f.ssid, p)) return mk(CAT_VEHICLE, SIG_CAR_WIFI, 70, false);
      // A phone sharing its connection: default hotspot names ("Rob's iPhone", "Galaxy S24
      // 1234", "AndroidAP_5678", "Pixel_9012"), or an iPhone / iPad's own Apple element on
      // a private (random) address.
      for (const char* p : kHotspotSsidWords)
        if (containsNoCase(f.ssid, p)) return mk(CAT_PHONE, SIG_PHONE_HOTSPOT, 60, false);
    }
  }
  if (!global && f.apple_ie && (f.kind == KIND_WIFI_BEACON || f.kind == KIND_WIFI_PROBE_RESP))
    return mk(CAT_PHONE, SIG_PHONE_HOTSPOT, 55, false);
  if (global && memcmp(tx, kHak5Oui, 3) == 0) return mk(CAT_PENTEST, SIG_PINEAPPLE, 85, true);
  if (global) {
    if (const OuiSig* o = matchOuiSig(kOuiSigs, tx)) return mk(o->cat, o->sig, o->conf, o->strong);
    if (const Oui* o = matchOui(kFlockWifiOuis, tx)) {
      // The list is almost all Liteon, Silicon Labs and other module makers whose Wi-Fi
      // parts are in countless laptops and TVs. A Flock camera runs an access point, so
      // only a beacon from one of these counts; a client from one is just a laptop, and a
      // maker that's everywhere (Raspberry Pi, Espressif) proves nothing. Those are filed
      // as ordinary Wi-Fi with the hint kept in their details, so the ALPR count stays clean.
      const bool ap = f.kind == KIND_WIFI_BEACON || f.kind == KIND_WIFI_PROBE_RESP;
      if (o->generic || !ap) return mk(CAT_OTHER_WIFI, SIG_FLOCK_WIFI_OUI_GENERIC, 20, false);
      return mk(CAT_FLOCK, SIG_FLOCK_WIFI_OUI, 45, false);
    }
  }
  if (f.kind == KIND_WIFI_DATA) {
    // A client heard in its own traffic (to its access point). Phones, tablets and
    // newer laptops use a private address per network; a fixed one is usually a TV,
    // printer, plug or older device, unless the maker says phone.
    if (!global) return mk(CAT_PHONE, SIG_WIFI_CLIENT_PRIVATE, 30, false);
    const uint8_t mk_ = ouiMaker(tx);
    if (mk_ && ouiMakerIsPhone(mk_)) return mk(CAT_PHONE, SIG_PHONE_MAKER_WIFI, 45, false);
    if (mk_) return mk(CAT_OTHER_WIFI, SIG_AMAZON_WIFI, 25, false);
    return mk(CAT_OTHER_WIFI, SIG_WIFI_CLIENT, 15, false);
  }
  if (f.kind == KIND_WIFI_PROBE_REQ && global) {
    const uint8_t mk_ = ouiMaker(tx);
    if (mk_ && ouiMakerIsPhone(mk_)) return mk(CAT_PHONE, SIG_PHONE_MAKER_WIFI, 45, false);
    if (mk_) return mk(CAT_OTHER_WIFI, SIG_AMAZON_WIFI, 25, false);
  }
  if (f.kind == KIND_WIFI_PROBE_REQ)
    // Phones have randomized their probe addresses since iOS 8 / Android 8. A probe from a
    // fixed address is almost always something else: a printer, TV, camera, smart plug, laptop.
    return global ? mk(CAT_OTHER_WIFI, SIG_PROBE_GLOBAL, 25, false) : mk(CAT_PHONE, SIG_PROBE_RANDOM, 30, false);
  if (f.kind == KIND_WIFI_BEACON || f.kind == KIND_WIFI_PROBE_RESP)
    return mk(CAT_OTHER_WIFI, SIG_ACCESS_POINT, 5, false);
  return mk(CAT_OTHER_WIFI, SIG_NONE, 0, false);
}

const char* sigName(uint16_t id) {
  switch (id) {
    case SIG_FLOCK_SSID: return "Flock camera SSID";
    case SIG_FLOCK_WIFI_OUI: return "Possible Flock camera (module OUI, access point)";
    case SIG_FLOCK_WIFI_OUI_GENERIC: return "Chip maker Flock also uses (common in laptops)";
    case SIG_FLOCK_BLE_NAME: return "Flock device name";
    case SIG_FLOCK_BLE_MFGR: return "Flock BLE (mfgr 0x09C8)";
    case SIG_FLOCK_BLE_OUI: return "Flock-listed BLE OUI (unverified)";
    case SIG_RAVEN: return "Flock Raven gunshot sensor";
    case SIG_RAVEN_PARTIAL: return "Raven UUID (single, weak)";
    case SIG_AXON_OUI: return "Axon device (OUI)";
    case SIG_AXON_NAME: return "Axon body camera";
    case SIG_FLOCK_OWN_OUI: return "Flock Safety device (own OUI)";
    case SIG_RING_OUI: return "Ring camera (Amazon)";
    case SIG_AXON_UUID: return "Axon / TASER device";
    case SIG_TASER_MFGR: return "Axon / TASER device (maker ID)";
    case SIG_MOTO_SOLUTIONS: return "Motorola Solutions (police radio or body cam)";
    case SIG_MOTO_SOLUTIONS_OUI: return "Motorola Solutions radio / camera (OUI)";
    case SIG_DIGITAL_ALLY_OUI: return "Digital Ally body / car camera";
    case SIG_WATCHGUARD_OUI: return "WatchGuard body / car camera";
    case SIG_UTILITY_OUI: return "Utility BodyWorn camera";
    case SIG_ZEPCAM_OUI: return "Zepcam body camera";
    case SIG_RAYBAN_META: return "Ray-Ban / Oakley Meta glasses";
    case SIG_LUXOTTICA_OUI: return "Ray-Ban / Oakley Meta glasses (OUI)";
    case SIG_META_UUID: return "Meta glasses or Quest headset";
    case SIG_META_MFGR: return "Meta glasses or Quest headset (maker ID)";
    case SIG_META_INC: return "Meta device";
    case SIG_META_OUI: return "Meta glasses or Quest (OUI)";
    case SIG_SNAP_SPECTACLES: return "Snap Spectacles";
    case SIG_VUZIX: return "Vuzix smart glasses";
    case SIG_VUZIX_OUI: return "Vuzix smart glasses (OUI)";
    case SIG_DJI_DRONEID: return "DJI drone (DroneID)";
    case SIG_DJI_MFGR: return "DJI drone or camera";
    case SIG_DJI_OUI: return "DJI drone or camera (OUI)";
    case SIG_PARROT_OUI: return "Parrot drone (OUI)";
    case SIG_SKYDIO_OUI: return "Skydio drone";
    case SIG_DRONE_SSID: return "Drone Wi-Fi hotspot";
    case SIG_FINDMY_SEPARATED: return "Find My tracker, away from owner";
    case SIG_FINDMY_NEAR_OWNER: return "Find My device, near owner";
    case SIG_SMARTTAG: return "Samsung SmartTag";
    case SIG_TILE: return "Tile tracker";
    case SIG_GOOGLE_FMDN: return "Google Find Hub tracker";
    case SIG_DULT: return "Tracker (unwanted-tracking alert)";
    case SIG_CHIPOLO: return "Chipolo tracker";
    case SIG_REMOTE_ID_BLE: return "Drone Remote ID (BLE)";
    case SIG_REMOTE_ID_WIFI: return "Drone Remote ID (Wi-Fi)";
    case SIG_SERIAL_BLE_MODULE: return "Serial BLE module";
    case SIG_FLIPPER: return "Flipper Zero";
    case SIG_PINEAPPLE: return "Wi-Fi Pineapple";
    case SIG_PWNAGOTCHI: return "Pwnagotchi";
    case SIG_APPLE_DEVICE: return "Apple device";
    case SIG_SAMSUNG_DEVICE: return "Samsung device";
    case SIG_GOOGLE_DEVICE: return "Google device";
    case SIG_PROBE_RANDOM: return "Phone (Wi-Fi probe, randomized)";
    case SIG_PROBE_GLOBAL: return "Wi-Fi client, fixed address (not a phone)";
    case SIG_APPLE_AUDIO: return "AirPods / Beats";
    case SIG_FAST_PAIR: return "Fast Pair accessory";
    case SIG_BOSE: return "Bose audio";
    case SIG_SONY: return "Sony device";
    case SIG_HARMAN: return "JBL / Harman audio";
    case SIG_GARMIN: return "Garmin wearable";
    case SIG_POLAR: return "Polar wearable";
    case SIG_HEART_RATE: return "Heart-rate sensor";
    case SIG_MICROSOFT: return "Windows PC / Microsoft";
    case SIG_SAYS_PHONE: return "Phone";
    case SIG_SAYS_EARBUDS: return "Earbuds";
    case SIG_SAYS_HEADSET: return "Headset";
    case SIG_SAYS_HEADPHONES: return "Headphones";
    case SIG_SAYS_SPEAKER: return "Speaker";
    case SIG_SAYS_HEARING_AID: return "Hearing aid";
    case SIG_LE_AUDIO: return "LE Audio device";
    case SIG_SWIFT_PAIR_AUDIO: return "Headphones (pairing mode)";
    case SIG_SAYS_WATCH: return "Watch";
    case SIG_SAYS_FITNESS: return "Fitness sensor";
    case SIG_AUDIO_MAKER: return "Audio device";
    case SIG_CLASSIC_HEADSET: return "Headset (Classic BT)";
    case SIG_CLASSIC_HEADPHONES: return "Headphones (Classic BT)";
    case SIG_CLASSIC_SPEAKER: return "Speaker (Classic BT)";
    case SIG_CLASSIC_CAR_AUDIO: return "Car audio (Classic BT)";
    case SIG_CLASSIC_AUDIO: return "Audio device (Classic BT)";
    case SIG_CLASSIC_PHONE: return "Phone (Classic BT)";
    case SIG_CLASSIC_COMPUTER: return "Computer (Classic BT)";
    case SIG_CLASSIC_INPUT: return "Keyboard / mouse (Classic BT)";
    case SIG_CLASSIC_WEARABLE: return "Wearable (Classic BT)";
    case SIG_CLASSIC_DEVICE: return "Classic Bluetooth device";
    case SIG_CLASSIC_SERIAL: return "Serial BT module (HC-05 type, skimmer radio)";
    case SIG_MESHTASTIC: return "Meshtastic node";
    case SIG_LORAWAN_DEVICE: return "LoRaWAN sensor / tracker";
    case SIG_LORAWAN_JOIN: return "LoRaWAN device joining a network";
    case SIG_TESLA_BLE: return "Tesla (phone-key Bluetooth)";
    case SIG_WIFI_CLIENT_PRIVATE: return "Phone, tablet or laptop on Wi-Fi (private address)";
    case SIG_WIFI_CLIENT: return "Wi-Fi device, connected (fixed address)";
    case SIG_PHONE_MAKER_WIFI: return "Phone or tablet (maker's own address)";
    case SIG_AMAZON_WIFI: return "Amazon device (Fire tablet, Echo, Fire TV, Ring)";
    case SIG_PHONE_HOTSPOT: return "Phone hotspot (sharing its connection)";
    case SIG_CAR_WIFI: return "Car Wi-Fi hotspot";
    case SIG_CAR_BT_NAME: return "Car (Bluetooth name)";
    case SIG_OBD_DONGLE: return "OBD-II dongle (plugged into a car)";
    case SIG_CAR_STEREO: return "Car stereo / hands-free (Classic BT)";
    case SIG_WEARABLE_MAKER: return "Wearable";
    case SIG_SAYS_COMPUTER: return "Computer";
    case SIG_SAYS_INPUT: return "Keyboard / mouse / controller";
    case SIG_SWIFT_PAIR: return "Device in pairing mode (Swift Pair)";
    case SIG_GENERIC_BLE: return "BLE device";
    case SIG_ACCESS_POINT: return "Access point";
    default: return "";
  }
}

// ---------------------------------------------------------------------------
// Wire records

Sighting makeBleSighting(const BleAdv& a, const Match& m, uint8_t channel) {
  Sighting s;
  memcpy(s.addr, a.addr, 6);
  s.kind = KIND_BLE_ADV;
  s.channel = channel;
  s.rssi_max = s.rssi_last = a.rssi;
  s.hits = 1;
  s.category = m.category;
  s.sig_id = m.sig_id;
  s.conf = m.conf;
  s.flags = (uint8_t)((bleIsRandom(a) ? SF_RANDOM_ADDR : 0) | (bleIsStaticRandom(a) ? SF_STATIC_RAND : 0) |
                      (a.connectable ? SF_CONNECTABLE : 0) | (a.name_len ? SF_HAS_NAME : 0) |
                      (m.strong ? SF_STRONG_MATCH : 0));
  s.company = a.has_company ? a.company : 0xFFFF;
  s.fp = a.fp;
  return s;
}

// ---------------------------------------------------------------------------
// Remote ID (ASTM F3411-22a). Each message is 25 bytes: [type << 4 | version] + 24.

static int32_t le32s(const uint8_t* p) { return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }
static int16_t ridAlt(const uint8_t* p) {   // 0.5 m steps from -1000 m; 0 = unknown
  const uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
  return v == 0 ? (int16_t)-1000 : (int16_t)((int32_t)v / 2 - 1000);
}
static void ridText(const uint8_t* p, size_t n, char* out) {
  size_t k = 0;
  for (size_t i = 0; i < n && p[i]; ++i) if (p[i] >= 0x20 && p[i] < 0x7F) out[k++] = (char)p[i];
  out[k] = '\0';
}
static bool ridMessage(const uint8_t* m, RemoteId& r, int depth = 0) {
  const uint8_t type = m[0] >> 4;
  switch (type) {
    case 0:   // Basic ID
      r.id_type = m[1] >> 4; r.ua_type = m[1] & 0x0F;
      ridText(m + 2, 20, r.id);
      r.has_id = r.id[0] != 0;
      return true;
    case 1: {  // Location / vector
      const uint8_t fl = m[1];
      r.status = fl >> 4;
      r.dir = (uint16_t)(m[2] + ((fl & 0x02) ? 180 : 0));
      if (r.dir > 360) r.dir = 361;
      const uint8_t sp = m[3];
      if (sp != 255) r.speed_cms = (uint16_t)((fl & 0x01) ? sp * 75 + 6375 : sp * 25);
      r.lat_e7 = le32s(m + 5); r.lon_e7 = le32s(m + 9);
      r.alt_m = ridAlt(m + 15);
      r.height_m = ridAlt(m + 17);
      r.has_loc = (r.lat_e7 || r.lon_e7) && r.lat_e7 >= -900000000 && r.lat_e7 <= 900000000 &&
                  r.lon_e7 >= -1800000000 && r.lon_e7 <= 1800000000;
      return true;
    }
    case 3:   // Self-ID text
      ridText(m + 2, 23, r.text);
      return true;
    case 4:   // System: operator location
      r.op_lat_e7 = le32s(m + 2); r.op_lon_e7 = le32s(m + 6);
      r.has_op = (r.op_lat_e7 || r.op_lon_e7) && r.op_lat_e7 >= -900000000 && r.op_lat_e7 <= 900000000;
      return true;
    case 0xF: {  // message pack
      if (depth) return false;
      const uint8_t size = m[1], count = m[2];
      if (size != 25 || count > 9) return false;
      return true;   // handled by the caller, which knows the buffer length
    }
    default:
      return true;   // auth, operator id: nothing we show
  }
}
static bool ridPack(const uint8_t* p, size_t n, RemoteId& r) {
  if (n < 25) return false;
  if ((p[0] >> 4) != 0xF) return ridMessage(p, r);
  const uint8_t size = p[1], count = p[2];
  if (size != 25 || count == 0 || count > 9 || n < 3u + (size_t)count * 25) return false;
  for (uint8_t i = 0; i < count; ++i) ridMessage(p + 3 + i * 25, r, 1);
  return true;
}
bool decodeRemoteIdBle(const uint8_t* p, size_t n, RemoteId& r) {
  r = RemoteId();
  if (!p || n < 2 + 25 || p[0] != 0x0D) return false;   // app code, counter
  return ridPack(p + 2, n - 2, r);
}
bool decodeRemoteIdWifi(const uint8_t* p, size_t n, RemoteId& r) {
  r = RemoteId();
  if (!p || n < 1 + 25) return false;   // counter
  return ridPack(p + 1, n - 1, r);
}
bool remoteIdFromBle(const BleAdv& a, RemoteId& r) {
  const BleSvcData* s = svcData(a, kRemoteIdBleUuid);
  return s && decodeRemoteIdBle(s->data, s->len, r);
}

uint8_t ouiMaker(const uint8_t mac[6]) {
  if (macIsLocallyAdministered(mac) || macIsMulticast(mac)) return 0;
  const uint32_t key = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
  size_t lo = 0, hi = sizeof(ouimakers::kOuis) / sizeof(ouimakers::kOuis[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const uint32_t o = ouimakers::kOuis[mid] >> 8;
    if (o == key) return (uint8_t)(ouimakers::kOuis[mid] & 0xFF);
    if (o < key) lo = mid + 1; else hi = mid;
  }
  return 0;
}
static constexpr size_t kNMakers = sizeof(ouimakers::kMakers) / sizeof(ouimakers::kMakers[0]);
const char* ouiMakerName(uint8_t m) { return m < kNMakers ? ouimakers::kMakers[m].name : ""; }
uint16_t ouiMakerCompany(uint8_t m) { return m && m < kNMakers ? ouimakers::kMakers[m].company : 0xFFFF; }
bool ouiMakerIsPhone(uint8_t m) { return m && m < kNMakers && ouimakers::kMakers[m].phone; }

Sighting makeWifiSighting(const WifiFrame& f, const Match& m, uint8_t channel, int8_t rssi) {
  Sighting s;
  memcpy(s.addr, f.addr2, 6);
  s.kind = f.kind;
  s.channel = channel;
  s.rssi_max = s.rssi_last = rssi;
  s.hits = 1;
  s.category = m.category;
  s.sig_id = m.sig_id;
  s.conf = m.conf;
  s.flags = (uint8_t)((macIsLocallyAdministered(f.addr2) ? SF_RANDOM_ADDR : 0) |
                      (m.strong ? SF_STRONG_MATCH : 0) | (channelIs5GHz(channel) ? SF_5GHZ : 0));
  s.company = ouiMakerCompany(ouiMaker(f.addr2));   // shown as MAKER, like Bluetooth's
  s.fp = f.fp;
  return s;
}

// ---------------------------------------------------------------------------
// Flood monitor

void FloodMonitor::onFrame(const WifiFrame& f, uint8_t channel, uint32_t now_ms) {
  if (f.type != 0 || (f.subtype != 12 && f.subtype != 10)) return;
  if (now_ms - window_start_ >= 1000) {
    window_start_ = now_ms;
    deauth_ = disassoc_ = 0;
  }
  if (f.subtype == 12) ++deauth_; else ++disassoc_;
  const uint16_t n = f.subtype == 12 ? deauth_ : disassoc_;
  if (n >= threshold_ && !pending_) {
    pending_ = true;
    last_.type = f.subtype == 12 ? EV_DEAUTH_FLOOD : EV_DISASSOC_FLOOD;
    last_.channel = channel;
    last_.count = n;
    memcpy(last_.src, f.addr2, 6);
    memcpy(last_.dst, f.addr1, 6);
    memcpy(last_.bssid, f.addr3, 6);
  } else if (pending_ && n > last_.count) {
    last_.count = n;
  }
}

void FloodMonitor::onApFrame(const WifiFrame& f, uint8_t channel, uint32_t now_ms) {
  if ((f.kind != KIND_WIFI_BEACON && f.kind != KIND_WIFI_PROBE_RESP) || !f.has_ssid || !f.ssid_len || !f.ssid[0]) return;
  const uint32_t h = fnv1a((const uint8_t*)f.ssid, f.ssid_len);
  Twin* t = nullptr;
  Twin* oldest = &twins_[0];
  for (Twin& x : twins_) {
    if (x.used && memcmp(x.bssid, f.addr2, 6) == 0) { t = &x; break; }
    if (!x.used) { if (oldest->used) oldest = &x; }
    else if (oldest->used && x.first_ms < oldest->first_ms) oldest = &x;
  }
  if (t && t->n == 1 && t->names[0] == h && now_ms - t->first_ms < 60000) return;   // the common case: same name again
  if (!t || now_ms - t->first_ms >= 60000) {
    if (!t) { t = oldest; memcpy(t->bssid, f.addr2, 6); t->fired_ms = 0; }
    t->used = true;
    t->first_ms = now_ms;
    t->n = 0;
  }
  for (uint8_t i = 0; i < t->n; ++i) if (t->names[i] == h) return;
  if (t->n < 5) t->names[t->n++] = h;
  if (t->n >= 3 && (!t->fired_ms || now_ms - t->fired_ms > 300000) && !twin_pending_) {
    t->fired_ms = now_ms ? now_ms : 1;
    twin_ev_ = Event();
    twin_ev_.type = EV_EVIL_TWIN;
    twin_ev_.channel = channel;
    twin_ev_.count = t->n;
    memcpy(twin_ev_.src, f.addr2, 6);
    memcpy(twin_ev_.bssid, f.addr3, 6);
    twin_pending_ = true;
  }
}

bool FloodMonitor::poll(Event& out, uint32_t now_ms) {
  if (twin_pending_ && !pending_) { out = twin_ev_; twin_pending_ = false; return true; }
  if (!pending_) return false;
  if (fired_once_ && now_ms - last_fire_ < holdoff_ms_) { pending_ = false; return false; }
  out = last_;
  pending_ = false;
  fired_once_ = true;
  last_fire_ = now_ms;
  return true;
}

}  // namespace ss
