#include "ss_lora.h"

#include <string.h>

#include "ss_crypto.h"

namespace ss {

// Meshtastic US: slot = djb2(channel name) % 104 (250 kHz slots from 902 MHz); LongFast
// lands on slot 20 = 906.875 MHz as the Meshtastic docs state, MediumFast on 913.125.
const LoraPreset kMeshLongFast = {"LongFast", 906.875f, 250.0f, 11, 5, 0x2B, 16};
const LoraPreset kMeshMediumFast = {"MediumFast", 913.125f, 250.0f, 9, 5, 0x2B, 16};

float loraWanUplinkMhz(uint8_t n) { return 903.9f + 0.2f * (float)(n & 7); }

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
