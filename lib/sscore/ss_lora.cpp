#include "ss_lora.h"

#include <string.h>

#include "ss_crypto.h"

namespace ss {

// Meshtastic US: slot = djb2(channel name) % 104 (250 kHz slots from 902 MHz); LongFast
// lands on slot 20 = 906.875 MHz as the Meshtastic docs state, MediumFast on 45 = 913.125.
const LoraPreset kMeshLongFast = {"LongFast", 906.875f, 250.0f, 11, 5, 0x2B, 16};
const LoraPreset kMeshMediumFast = {"MediumFast", 913.125f, 250.0f, 9, 5, 0x2B, 16};

float loraWanUplinkMhz(uint8_t n) { return 903.9f + 0.2f * (float)(n & 7); }

// MeshCore app "USA/Canada (Recommended)". Receive-side preamble: the SX1262 only needs
// to see at least this many symbols, MeshCore sends more.
const LoraPreset kMeshCoreUs = {"MeshCore", 910.525f, 62.5f, 7, 5, 0x12, 8};

uint16_t meshUsSlots(float bw_khz) { return bw_khz > 0 ? (uint16_t)(26000.0f / bw_khz) : 0; }

uint16_t meshUsDefaultSlot(const char* name, float bw_khz) {
  uint32_t h = 5381;  // djb2, as the Meshtastic firmware hashes the channel name
  for (const char* c = name; c && *c; ++c) h = ((h << 5) + h) + (uint8_t)*c;
  const uint16_t n = meshUsSlots(bw_khz);
  return n ? (uint16_t)(h % n + 1) : 0;
}

float meshUsMhz(float bw_khz, uint16_t slot) {
  // 902 MHz + half a channel + (slot - 1) channels; double keeps 62.5 kHz steps exact
  return (float)(902.0 + bw_khz / 2000.0 + (double)(slot ? slot - 1 : 0) * bw_khz / 1000.0);
}

// Meshtastic modem presets: default channel name, bandwidth, SF, coding rate.
struct MeshPresetDef { uint8_t mode; const char* name; float bw; uint8_t sf, cr; };
static const MeshPresetDef kMeshPresets[] = {
    {LM_LONGFAST, "LongFast", 250.0f, 11, 5},     {LM_MEDIUMFAST, "MediumFast", 250.0f, 9, 5},
    {LM_MEDIUMSLOW, "MediumSlow", 250.0f, 10, 5}, {LM_LONGSLOW, "LongSlow", 125.0f, 12, 8},
    {LM_LONGMOD, "LongMod", 125.0f, 11, 8},       {LM_SHORTFAST, "ShortFast", 250.0f, 7, 5},
    {LM_SHORTSLOW, "ShortSlow", 250.0f, 8, 5},    {LM_SHORTTURBO, "ShortTurbo", 500.0f, 7, 5},
};
static const char* const kModeNames[LM_COUNT] = {"auto",      "longfast",  "mediumfast", "mediumslow",
                                                 "longslow",  "longmod",   "shortfast",  "shortslow",
                                                 "shortturbo", "meshcore", "lorawan"};

const char* loraModeName(uint8_t mode) { return mode < LM_COUNT ? kModeNames[mode] : "auto"; }

bool parseLoraMode(const char* s, uint8_t& mode) {
  if (!s) return false;
  for (uint8_t m = 0; m < LM_COUNT; ++m) {
    const char* a = kModeNames[m];
    const char* b = s;
    while (*a && *b && (*b | 0x20) == *a) { ++a; ++b; }
    if (!*a && !*b) { mode = m; return true; }
  }
  return false;
}

bool loraModeIsMeshtastic(uint8_t mode) { return mode >= LM_LONGFAST && mode <= LM_SHORTTURBO; }

bool loraModePreset(uint8_t mode, uint16_t slot, LoraPreset& out) {
  if (mode == LM_MESHCORE) { out = kMeshCoreUs; return true; }
  for (const MeshPresetDef& d : kMeshPresets)
    if (d.mode == mode) {
      if (slot > meshUsSlots(d.bw)) slot = 0;
      if (!slot) slot = meshUsDefaultSlot(d.name, d.bw);
      out = {d.name, meshUsMhz(d.bw, slot), d.bw, d.sf, d.cr, 0x2B, 16};
      return true;
    }
  return false;
}

static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool parseMeshtastic(const uint8_t* p, size_t n, MeshHeader& h) {
  if (!p || n < 16) return false;
  h.to = le32(p);
  h.from = le32(p + 4);
  h.id = le32(p + 8);
  const uint8_t flags = p[12];
  h.hop_limit = flags & 0x07;
  h.want_ack = (flags & 0x08) != 0;
  h.via_mqtt = (flags & 0x10) != 0;
  h.hop_start = (uint8_t)(flags >> 5);
  h.channel_hash = p[13];
  h.next_hop = p[14];
  h.relay_node = p[15];
  // Sender 0 / broadcast isn't a real node; hop_limit can't exceed where it started.
  if (h.from == 0 || h.from == 0xFFFFFFFFu) return false;
  if (h.hop_start && h.hop_limit > h.hop_start) return false;
  return true;
}

bool parseLoraWan(const uint8_t* p, size_t n, LoraWanFrame& f) {
  if (!p || n < 1) return false;
  const uint8_t mhdr = p[0];
  if ((mhdr & 0x03) != 0) return false;  // LoRaWAN major version R1
  f.mtype = (uint8_t)(mhdr >> 5);
  if (f.mtype == 0) {  // join request: MHDR JoinEUI(8) DevEUI(8) DevNonce(2) MIC(4)
    if (n != 23) return false;
    for (int i = 0; i < 8; ++i) {  // little-endian on air
      f.join_eui[i] = p[8 - i];
      f.dev_eui[i] = p[16 - i];
    }
    return true;
  }
  if (f.mtype == 2 || f.mtype == 4) {  // data up: MHDR DevAddr(4) FCtrl FCnt(2) [FOpts] [FPort Payload] MIC(4)
    if (n < 12) return false;
    const uint8_t foptsLen = p[5] & 0x0F;
    if (n < (size_t)(12 + foptsLen)) return false;
    f.dev_addr = le32(p + 1);
    f.fcnt = (uint16_t)(p[6] | (p[7] << 8));
    return f.dev_addr != 0;
  }
  return false;  // downlinks / rejoin / proprietary: not what we listen for
}

static Sighting loraBase(uint8_t kind, uint16_t sig, uint8_t conf, int8_t rssi) {
  Sighting s;
  s.kind = kind;
  s.rssi_max = s.rssi_last = rssi;
  s.hits = 1;
  s.category = CAT_LORA;
  s.sig_id = sig;
  s.conf = conf;
  s.flags = SF_STRONG_MATCH;
  s.company = 0xFFFF;
  s.fp = 0;
  return s;
}

Sighting makeMeshSighting(const MeshHeader& h, int8_t rssi) {
  Sighting s = loraBase(KIND_LORA_MESHTASTIC, SIG_MESHTASTIC, 90, rssi);
  s.addr[2] = (uint8_t)(h.from >> 24);
  s.addr[3] = (uint8_t)(h.from >> 16);
  s.addr[4] = (uint8_t)(h.from >> 8);
  s.addr[5] = (uint8_t)h.from;
  // How far away the sender really is: 0 hops = we heard it directly (RSSI is its
  // own); more = a relay retransmitted it (RSSI is the relay's).
  if (h.via_mqtt) s.channel = kLoraViaMqtt;
  else s.channel = h.hop_start >= h.hop_limit ? (uint8_t)(h.hop_start - h.hop_limit) : 0;
  return s;
}

Sighting makeMeshCoreSighting(const MeshCorePacket& m, const MeshCoreAdvert& a, int8_t rssi) {
  const uint16_t sig = a.node_type == MCN_CHAT       ? SIG_MESHCORE_CHAT
                       : a.node_type == MCN_REPEATER ? SIG_MESHCORE_REPEATER
                       : a.node_type == MCN_ROOM     ? SIG_MESHCORE_ROOM
                       : a.node_type == MCN_SENSOR   ? SIG_MESHCORE_SENSOR
                                                     : SIG_MESHCORE_NODE;
  Sighting s = loraBase(KIND_LORA_MESHCORE, sig, 90, rssi);
  memcpy(s.addr, a.pubkey, 6);
  s.channel = m.hops;
  return s;
}

bool parseMeshCore(const uint8_t* p, size_t n, MeshCorePacket& m) {
  if (!p || n < 2) return false;
  const uint8_t hdr = p[0];
  if ((hdr >> 6) != 0) return false;  // payload version 1 is 0 on the air
  m.route = hdr & 0x03;
  m.type = (uint8_t)((hdr >> 2) & 0x0F);
  if (m.type > MCT_CONTROL && m.type != MCT_RAW_CUSTOM) return false;
  size_t i = 1;
  if (m.route == MCR_TRANSPORT_FLOOD || m.route == MCR_TRANSPORT_DIRECT) i += 4;  // two transport codes
  if (i >= n) return false;
  const uint8_t pl = p[i++];
  m.hash_size = (uint8_t)((pl >> 6) + 1);
  if (m.hash_size > 3) return false;  // reserved
  m.hops = pl & 0x3F;
  const size_t pathBytes = (size_t)m.hops * m.hash_size;
  if (i + pathBytes > n) return false;
  i += pathBytes;
  m.payload_off = i;
  m.payload_len = n - i;
  // Every real payload has something in it; group messages at least a hash + MAC.
  if (!m.payload_len) return false;
  if ((m.type == MCT_GRP_TXT || m.type == MCT_GRP_DATA) && m.payload_len < 3) return false;
  return true;
}

const char* meshCoreTypeName(uint8_t t) {
  switch (t) {
    case MCT_REQ: return "request";
    case MCT_RESPONSE: return "response";
    case MCT_TXT_MSG: return "direct message";
    case MCT_ACK: return "ack";
    case MCT_ADVERT: return "advert";
    case MCT_GRP_TXT: return "group text";
    case MCT_GRP_DATA: return "group data";
    case MCT_ANON_REQ: return "login / anon request";
    case MCT_PATH: return "path";
    case MCT_TRACE: return "trace";
    case MCT_MULTIPART: return "multipart";
    case MCT_CONTROL: return "control";
    case MCT_RAW_CUSTOM: return "custom";
    default: return "?";
  }
}

bool parseMeshCoreAdvert(const uint8_t* p, size_t n, MeshCoreAdvert& a) {
  // pubkey(32) timestamp(4) signature(64) flags(1) [lat(4) lon(4)] [feat1(2)] [feat2(2)] [name]
  if (!p || n < 101) return false;
  memcpy(a.pubkey, p, 32);
  a.timestamp = le32(p + 32);
  size_t i = 100;
  const uint8_t flags = p[i++];
  a.node_type = flags & 0x0F;
  if (a.node_type < MCN_CHAT || a.node_type > MCN_SENSOR) return false;
  a.has_pos = (flags & 0x10) != 0;
  if (a.has_pos) {
    if (i + 8 > n) return false;
    a.lat_e6 = (int32_t)le32(p + i);
    a.lon_e6 = (int32_t)le32(p + i + 4);
    i += 8;
    if (a.lat_e6 < -90000000 || a.lat_e6 > 90000000 || a.lon_e6 < -180000000 || a.lon_e6 > 180000000) return false;
  }
  if (flags & 0x20) i += 2;
  if (flags & 0x40) i += 2;
  if (i > n) return false;
  memset(a.name, 0, sizeof(a.name));
  if (flags & 0x80) {
    size_t k = 0;
    for (; i < n && p[i] && k < kMaxName; ++i) {
      const uint8_t c = p[i];
      if (c < 0x20 || c == 0x7F) return false;  // names are text (UTF-8 allowed)
      a.name[k++] = (char)c;
    }
  }
  // The public key is never all zeros.
  uint8_t any = 0;
  for (uint8_t b : a.pubkey) any |= b;
  return any != 0;
}

Sighting makeLoraWanSighting(const LoraWanFrame& f, int8_t rssi) {
  if (f.mtype == 0) {
    Sighting s = loraBase(KIND_LORAWAN, SIG_LORAWAN_JOIN, 85, rssi);
    memcpy(s.addr, f.dev_eui + 2, 6);
    return s;
  }
  Sighting s = loraBase(KIND_LORAWAN, SIG_LORAWAN_DEVICE, 80, rssi);
  s.addr[2] = (uint8_t)(f.dev_addr >> 24);
  s.addr[3] = (uint8_t)(f.dev_addr >> 16);
  s.addr[4] = (uint8_t)(f.dev_addr >> 8);
  s.addr[5] = (uint8_t)f.dev_addr;
  return s;
}


// ---------------------------------------------------------------------------
// Meshtastic default channel

// "AQ==" expands to this key (Meshtastic's defaultpsk).
static const uint8_t kMeshDefaultKey[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                            0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

uint8_t meshChannelHash(const char* name) {
  uint8_t h = 0;
  for (const char* c = name; c && *c; ++c) h ^= (uint8_t)*c;
  for (uint8_t b : kMeshDefaultKey) h ^= b;
  return h;
}

bool meshDecryptDefault(const MeshHeader& h, const char* channel_name, const uint8_t* enc, size_t n, uint8_t* out) {
  if (!enc || !n || h.channel_hash != meshChannelHash(channel_name)) return false;
  uint8_t iv[16] = {0};   // packet id (64-bit LE), sender (32-bit LE), zeros
  for (int i = 0; i < 4; ++i) {
    iv[i] = (uint8_t)(h.id >> (8 * i));
    iv[8 + i] = (uint8_t)(h.from >> (8 * i));
  }
  aesCtr(kMeshDefaultKey, sizeof(kMeshDefaultKey), iv, enc, out, n);
  return true;
}

// Minimal protobuf reader: varints, fixed32, length-delimited.
struct Pb {
  const uint8_t* p;
  size_t n, i = 0;
  bool ok = true;
  uint64_t varint() {
    uint64_t v = 0;
    for (int s = 0; s < 64; s += 7) {
      if (i >= n) { ok = false; return 0; }
      const uint8_t b = p[i++];
      v |= (uint64_t)(b & 0x7F) << s;
      if (!(b & 0x80)) return v;
    }
    ok = false;
    return 0;
  }
  uint32_t fixed32() {
    if (i + 4 > n) { ok = false; return 0; }
    const uint32_t v = le32(p + i);
    i += 4;
    return v;
  }
  // Next field: returns false at the end or on bad data. For wire type 2, (sub, sub_n).
  bool next(uint32_t& field, uint8_t& wt, uint64_t& v, const uint8_t*& sub, size_t& sub_n) {
    if (!ok || i >= n) return false;
    const uint64_t key = varint();
    field = (uint32_t)(key >> 3);
    wt = (uint8_t)(key & 7);
    if (!ok || field == 0) return false;
    switch (wt) {
      case 0: v = varint(); break;
      case 1: if (i + 8 > n) return false; v = le32(p + i); i += 8; break;
      case 2: {
        const uint64_t l = varint();
        if (!ok || l > n - i) return false;
        sub = p + i; sub_n = (size_t)l; i += (size_t)l;
        break;
      }
      case 5: v = fixed32(); break;
      default: return false;
    }
    return ok;
  }
};

static void pbString(const uint8_t* s, size_t l, char* out, size_t cap) {
  size_t k = 0;
  for (size_t j = 0; j < l && k + 1 < cap; ++j)
    if (s[j] >= 0x20) out[k++] = (char)s[j];   // UTF-8 kept as is, control bytes dropped
  out[k] = '\0';
}

bool decodeMeshData(const uint8_t* p, size_t n, MeshInfo& out) {
  Pb d{p, n};
  uint32_t f; uint8_t wt; uint64_t v = 0; const uint8_t* sub = nullptr; size_t sub_n = 0;
  const uint8_t* payload = nullptr; size_t payload_n = 0;
  bool any = false;
  while (d.next(f, wt, v, sub, sub_n)) {
    if (f == 1 && wt == 0) { out.port = (uint32_t)v; any = true; }
    else if (f == 2 && wt == 2) { payload = sub; payload_n = sub_n; }
  }
  // A wrong key (private channel with a colliding hash) almost never parses this far.
  if (!any || !out.port || out.port > 1024 || !payload) return false;
  Pb m{payload, payload_n};
  if (out.port == MP_NODEINFO) {
    while (m.next(f, wt, v, sub, sub_n)) {
      if (f == 2 && wt == 2) pbString(sub, sub_n, out.long_name, sizeof(out.long_name));
      else if (f == 3 && wt == 2) pbString(sub, sub_n, out.short_name, sizeof(out.short_name));
      else if (f == 5 && wt == 0) out.hw_model = (uint32_t)v;
    }
    return m.ok && out.long_name[0];
  }
  if (out.port == MP_POSITION) {
    bool la = false, lo = false;
    while (m.next(f, wt, v, sub, sub_n)) {
      if (f == 1 && wt == 5) { out.lat_e7 = (int32_t)(uint32_t)v; la = true; }
      else if (f == 2 && wt == 5) { out.lon_e7 = (int32_t)(uint32_t)v; lo = true; }
      else if (f == 3 && wt == 0) out.alt_m = (int32_t)v;
    }
    out.has_pos = m.ok && la && lo && (out.lat_e7 || out.lon_e7) && out.lat_e7 >= -900000000 && out.lat_e7 <= 900000000 &&
                  out.lon_e7 >= -1800000000 && out.lon_e7 <= 1800000000;
    return out.has_pos;
  }
  if (out.port == MP_TELEMETRY) {
    while (m.next(f, wt, v, sub, sub_n)) {
      if (f == 2 && wt == 2) {   // device_metrics
        Pb dm{sub, sub_n};
        uint32_t f2; uint8_t wt2; uint64_t v2 = 0; const uint8_t* s2; size_t n2;
        while (dm.next(f2, wt2, v2, s2, n2))
          if (f2 == 1 && wt2 == 0) { out.has_batt = true; out.batt_pct = (uint8_t)(v2 > 101 ? 101 : v2); }
      }
    }
    return true;
  }
  return true;   // text and the rest: the port is enough (contents aren't shown)
}
}  // namespace ss
