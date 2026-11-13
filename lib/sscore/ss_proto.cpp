#include "ss_proto.h"

#include <string.h>

namespace ss {

// ---------------------------------------------------------------------------
// Writer / Reader

void Writer::u8(uint8_t v) {
  if (!ok_ || pos_ + 1 > cap_) { ok_ = false; return; }
  buf_[pos_++] = v;
}
void Writer::u16(uint16_t v) { u8((uint8_t)(v & 0xFF)); u8((uint8_t)(v >> 8)); }
void Writer::u32(uint32_t v) { u16((uint16_t)(v & 0xFFFF)); u16((uint16_t)(v >> 16)); }
void Writer::u64(uint64_t v) { u32((uint32_t)(v & 0xFFFFFFFFu)); u32((uint32_t)(v >> 32)); }
void Writer::bytes(const uint8_t* p, size_t n) {
  if (!ok_ || pos_ + n > cap_) { ok_ = false; return; }
  memcpy(buf_ + pos_, p, n);
  pos_ += n;
}

uint8_t Reader::u8() {
  if (!ok_ || pos_ + 1 > len_) { ok_ = false; return 0; }
  return buf_[pos_++];
}
uint16_t Reader::u16() { uint16_t lo = u8(); uint16_t hi = u8(); return (uint16_t)(lo | (hi << 8)); }
uint32_t Reader::u32() { uint32_t lo = u16(); uint32_t hi = u16(); return lo | (hi << 16); }
uint64_t Reader::u64() { uint64_t lo = u32(); uint64_t hi = u32(); return lo | (hi << 32); }
void Reader::bytes(uint8_t* out, size_t n) {
  if (!ok_ || pos_ + n > len_) { ok_ = false; memset(out, 0, n); return; }
  memcpy(out, buf_ + pos_, n);
  pos_ += n;
}

// ---------------------------------------------------------------------------
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection)

uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// ---------------------------------------------------------------------------
// Frames

size_t encodeFrame(uint8_t type, uint16_t node_id, uint16_t seq,
                   const uint8_t* payload, size_t payload_len,
                   uint8_t* out, size_t out_cap) {
  if (payload_len > kMaxPayload) return 0;
  const size_t total = kHeaderSize + payload_len + kCrcSize;
  if (total > out_cap) return 0;
  Writer w(out, out_cap);
  w.u16(kMagic);
  w.u8(kProtoVersion);
  w.u8(type);
  w.u16(node_id);
  w.u16(seq);
  if (payload_len) w.bytes(payload, payload_len);
  w.u16(crc16(out, kHeaderSize + payload_len));
  return w.ok() ? w.size() : 0;
}

bool decodeFrame(const uint8_t* frame, size_t len, Header& hdr,
                 const uint8_t** payload, size_t* payload_len) {
  if (!frame || len < kHeaderSize + kCrcSize || len > kMaxFrame) return false;
  Reader r(frame, len);
  hdr.magic = r.u16();
  hdr.version = r.u8();
  hdr.type = r.u8();
  hdr.node_id = r.u16();
  hdr.seq = r.u16();
  if (hdr.magic != kMagic || hdr.version != kProtoVersion) return false;
  const size_t body = len - kCrcSize;
  const uint16_t want = (uint16_t)(frame[body] | (frame[body + 1] << 8));
  if (crc16(frame, body) != want) return false;
  if (payload) *payload = frame + kHeaderSize;
  if (payload_len) *payload_len = body - kHeaderSize;
  return true;
}

// ---------------------------------------------------------------------------
// Payloads

static void helloFields(Writer& w, const Hello& m) {
  w.bytes(m.mac, 6); w.u16(m.fw_version); w.u8(m.hw); w.u8(m.caps); w.u8(m.role_pref);
}
static void welcomeFields(Writer& w, const Welcome& m) {
  w.bytes(m.core_mac, 6); w.u8(m.node_index); w.u32(m.core_ms);
}

size_t helloAuthInput(const Hello& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  helloFields(w, m);
  return w.ok() ? w.size() : 0;
}
size_t welcomeAuthInput(const Welcome& m, const uint8_t node_mac[6], uint8_t* out, size_t cap) {
  Writer w(out, cap);
  welcomeFields(w, m);
  w.bytes(node_mac, 6);
  return w.ok() ? w.size() : 0;
}

size_t encodeHello(const Hello& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  helloFields(w, m);
  w.u32(m.auth);
  return w.ok() ? w.size() : 0;
}
bool decodeHello(const uint8_t* p, size_t n, Hello& m) {
  Reader r(p, n);
  r.bytes(m.mac, 6); m.fw_version = r.u16(); m.hw = r.u8(); m.caps = r.u8(); m.role_pref = r.u8();
  m.auth = r.u32();
  return r.ok();
}

size_t encodeWelcome(const Welcome& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  welcomeFields(w, m);
  w.u32(m.auth);
  return w.ok() ? w.size() : 0;
}
bool decodeWelcome(const uint8_t* p, size_t n, Welcome& m) {
  Reader r(p, n);
  r.bytes(m.core_mac, 6); m.node_index = r.u8(); m.core_ms = r.u32(); m.auth = r.u32();
  return r.ok();
}

size_t encodeConfig(const Config& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(m.cfg_version); w.u8(m.role); w.u16(m.category_mask); w.u64(m.channel_mask);
  w.u16(m.dwell_ms); w.u16(m.report_ms); w.i8(m.rssi_floor); w.u8(m.ble_duty_pct);
  w.u8(m.node_flags);
  const uint8_t ll = (uint8_t)strnlen(m.label, kMaxLabel);
  w.u8(ll); w.bytes((const uint8_t*)m.label, ll);
  w.u8(m.lora_mode); w.u8(m.lora_slot);
  return w.ok() ? w.size() : 0;
}
bool decodeConfig(const uint8_t* p, size_t n, Config& m) {
  Reader r(p, n);
  m.cfg_version = r.u8(); m.role = r.u8(); m.category_mask = r.u16(); m.channel_mask = r.u64();
  m.dwell_ms = r.u16(); m.report_ms = r.u16(); m.rssi_floor = r.i8(); m.ble_duty_pct = r.u8();
  if (!r.ok()) return false;
  // Optional trailing fields, added later (older masters stop before them).
  m.node_flags = r.remaining() ? r.u8() : 0;
  memset(m.label, 0, sizeof(m.label));
  if (r.remaining()) {
    const uint8_t ll = r.u8();
    if (ll > kMaxLabel) return false;
    r.bytes((uint8_t*)m.label, ll);
  }
  m.lora_mode = r.remaining() ? r.u8() : 0;
  m.lora_slot = r.remaining() ? r.u8() : 0;
  return r.ok();
}

size_t encodeHeartbeat(const Heartbeat& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(m.uptime_s); w.u8(m.cfg_version); w.u8(m.channel); w.u8(m.batt_pct); w.i8(m.temp_c);
  w.u16(m.dropped); w.u32(m.wifi_frames); w.u32(m.ble_adv);
  w.u8(m.gps); w.u8(m.gps_sats);
  w.u8(m.reset); w.u16(m.heap_kb); w.u16(m.heap_min_kb); w.u16(m.losses);
  w.u8(m.has_pos ? 1 : 0); w.u32((uint32_t)m.lat_e7); w.u32((uint32_t)m.lon_e7); w.u32(m.utc);
  w.u8(m.sd);
  w.u8(m.link);
  w.u16(m.tx_wait_ms); w.u16(m.stuck_ms); w.u8(m.wants);
  w.u16(m.release); w.u32(m.build);
  return w.ok() ? w.size() : 0;
}
bool decodeHeartbeat(const uint8_t* p, size_t n, Heartbeat& m) {
  Reader r(p, n);
  m.uptime_s = r.u32(); m.cfg_version = r.u8(); m.channel = r.u8(); m.batt_pct = r.u8();
  m.temp_c = r.i8(); m.dropped = r.u16(); m.wifi_frames = r.u32(); m.ble_adv = r.u32();
  if (!r.ok()) return false;
  // Optional trailing fields (older nodes stop before them)
  m.gps = r.remaining() ? r.u8() : 0;
  m.gps_sats = r.remaining() ? r.u8() : 0;
  if (r.remaining() >= 7) {
    m.reset = r.u8(); m.heap_kb = r.u16(); m.heap_min_kb = r.u16(); m.losses = r.u16();
  }
  if (r.remaining() >= 13) {
    m.has_pos = r.u8() != 0; m.lat_e7 = (int32_t)r.u32(); m.lon_e7 = (int32_t)r.u32(); m.utc = r.u32();
  }
  if (r.remaining() >= 1) m.sd = r.u8();
  if (r.remaining() >= 1) m.link = r.u8();
  if (r.remaining() >= 5) { m.tx_wait_ms = r.u16(); m.stuck_ms = r.u16(); m.wants = r.u8(); }
  if (r.remaining() >= 6) { m.release = r.u16(); m.build = r.u32(); }
  return r.ok();
}

size_t encodeGpsInfo(const GpsInfo& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(m.state); w.u8(m.sats_used); w.u8(m.hdop10);
  w.u8((m.has_pos ? 1 : 0) | (m.has_alt ? 2 : 0) | (m.has_motion ? 4 : 0));
  w.u32((uint32_t)m.lat_e7); w.u32((uint32_t)m.lon_e7);
  w.u16((uint16_t)m.alt_m); w.u16(m.kmh10); w.u16(m.course10); w.u32(m.utc);
  const uint8_t n = m.n_sat > kGpsSats ? (uint8_t)kGpsSats : m.n_sat;
  w.u8(n);
  for (uint8_t i = 0; i < n; ++i) {
    const auto& s = m.sat[i];
    w.u8((uint8_t)((s.sys & 0x7F) | (s.used ? 0x80 : 0))); w.u8(s.prn); w.u8(s.elev); w.u8(s.az2); w.u8(s.snr);
  }
  return w.ok() ? w.size() : 0;
}

bool decodeGpsInfo(const uint8_t* p, size_t n, GpsInfo& m) {
  Reader r(p, n);
  m.state = r.u8(); m.sats_used = r.u8(); m.hdop10 = r.u8();
  const uint8_t f = r.u8();
  m.has_pos = f & 1; m.has_alt = f & 2; m.has_motion = f & 4;
  m.lat_e7 = (int32_t)r.u32(); m.lon_e7 = (int32_t)r.u32();
  m.alt_m = (int16_t)r.u16(); m.kmh10 = r.u16(); m.course10 = r.u16(); m.utc = r.u32();
  m.n_sat = r.u8();
  if (!r.ok() || m.n_sat > kGpsSats) return false;
  for (uint8_t i = 0; i < m.n_sat; ++i) {
    auto& s = m.sat[i];
    const uint8_t b = r.u8();
    s.sys = b & 0x7F; s.used = b & 0x80; s.prn = r.u8(); s.elev = r.u8(); s.az2 = r.u8(); s.snr = r.u8();
  }
  return r.ok();
}

const char* sdStateName(uint8_t s) {
  static const char* const k[] = {"none", "no_card", "logging", "error", "full", "formatting", "unformatted"};
  return s < sizeof(k) / sizeof(k[0]) ? k[s] : "none";
}

const char* resetWhyName(uint8_t r) {
  static const char* const k[] = {"unknown", "power", "software", "crash", "watchdog", "brownout", "no_master", "deepsleep", "usb", "radio", "stuck_tx"};
  return r < sizeof(k) / sizeof(k[0]) ? k[r] : "unknown";
}

static void putText(Writer& w, const char* t, uint8_t max) {
  uint8_t l = 0;
  while (l < max && t[l]) ++l;
  w.u8(l);
  w.bytes((const uint8_t*)t, l);
}
static bool getText(Reader& r, char* t, uint8_t max) {
  const uint8_t l = r.u8();
  if (!r.ok() || l > max) return false;
  r.bytes((uint8_t*)t, l);
  t[l] = '\0';
  return r.ok();
}

size_t encodeFileReq(const FileReq& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(m.req); w.u8(m.op); w.u32(m.off); w.u8(m.count);
  putText(w, m.name, kMaxFileName);
  return w.ok() ? w.size() : 0;
}
bool decodeFileReq(const uint8_t* p, size_t n, FileReq& m) {
  Reader r(p, n);
  m.req = r.u8(); m.op = r.u8(); m.off = r.u32(); m.count = r.u8();
  return r.ok() && getText(r, m.name, kMaxFileName);
}
size_t encodeFileChunk(const FileChunk& m, uint8_t* out, size_t cap) {
  if (m.len > kFileChunk) return 0;
  Writer w(out, cap);
  w.u8(m.req); w.u8(m.kind); w.u32(m.off); w.u32(m.total); w.u8(m.len);
  w.bytes(m.data, m.len);
  return w.ok() ? w.size() : 0;
}
bool decodeFileChunk(const uint8_t* p, size_t n, FileChunk& m) {
  Reader r(p, n);
  m.req = r.u8(); m.kind = r.u8(); m.off = r.u32(); m.total = r.u32(); m.len = r.u8();
  if (!r.ok() || m.len > kFileChunk) return false;
  r.bytes(m.data, m.len);
  return r.ok();
}
bool fileNameOk(const char* name) {
  size_t l = 0;
  for (; name[l]; ++l) {
    const uint8_t c = (uint8_t)name[l];
    if (c < 0x20 || c >= 0x7F || c == '/' || c == '\\' || c == ':' || c == '"') return false;
    if (c == '.' && name[l + 1] == '.') return false;
  }
  return l >= 1 && l <= kMaxFileName;
}

size_t encodeDash(const Dash& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(m.deploy); w.u16(m.cats); w.u8(m.nodes_online); w.u8(m.nodes_total); w.u16(m.devices_now);
  w.u8(CAT_COUNT - 1);
  for (uint8_t c = 1; c < CAT_COUNT; ++c) w.u8(m.now[c]);
  w.u16(m.alert.seq); w.u8(m.alert.level); w.u8(m.alert.category); w.u16(m.alert.age_s);
  putText(w, m.alert.text, kDashAlertText);
  const uint8_t n = m.n_threats > kDashThreats ? kDashThreats : m.n_threats;
  w.u8(n);
  for (uint8_t i = 0; i < n; ++i) {
    const DashThreat& t = m.threats[i];
    w.u8(t.category); w.i8(t.rssi); w.u8(t.conf); w.u8(t.flags);
    putText(w, t.text, kDashText);
  }
  return w.ok() ? w.size() : 0;
}
bool decodeDash(const uint8_t* p, size_t n, Dash& m) {
  Reader r(p, n);
  m = Dash();
  m.deploy = r.u8(); m.cats = r.u16(); m.nodes_online = r.u8(); m.nodes_total = r.u8(); m.devices_now = r.u16();
  const uint8_t nc = r.u8();
  for (uint8_t c = 1; c <= nc; ++c) {
    const uint8_t v = r.u8();
    if (c < CAT_COUNT) m.now[c] = v;  // newer masters may know more categories
  }
  m.alert.seq = r.u16(); m.alert.level = r.u8(); m.alert.category = r.u8(); m.alert.age_s = r.u16();
  if (!r.ok() || !getText(r, m.alert.text, kDashAlertText)) return false;
  m.n_threats = r.u8();
  if (!r.ok() || m.n_threats > kDashThreats) return false;
  for (uint8_t i = 0; i < m.n_threats; ++i) {
    DashThreat& t = m.threats[i];
    t.category = r.u8(); t.rssi = r.i8(); t.conf = r.u8(); t.flags = r.u8();
    if (!r.ok() || !getText(r, t.text, kDashText)) return false;
  }
  return r.ok();
}

size_t encodeDashCommand(const DashCommand& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(m.cmd); w.u8(m.arg);
  if (m.cmd == DC_VIEW) { w.u16(m.offset); w.u32(m.id); }
  return w.ok() ? w.size() : 0;
}
bool decodeDashCommand(const uint8_t* p, size_t n, DashCommand& m) {
  Reader r(p, n);
  m.cmd = r.u8(); m.arg = r.u8();
  m.offset = 0; m.id = 0;
  if (r.ok() && r.remaining() >= 6) { m.offset = r.u16(); m.id = r.u32(); }
  return r.ok();
}

size_t encodePairReq(const PairReq& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.bytes(m.mac, 6); w.u8(m.hw); w.u8(m.caps); w.u16(m.fw); w.bytes(m.pub, 32);
  return w.ok() ? w.size() : 0;
}
bool decodePairReq(const uint8_t* p, size_t n, PairReq& m) {
  Reader r(p, n);
  r.bytes(m.mac, 6); m.hw = r.u8(); m.caps = r.u8(); m.fw = r.u16(); r.bytes(m.pub, 32);
  return r.ok();
}
size_t encodePairOffer(const PairOffer& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.bytes(m.target, 6); w.bytes(m.pub, 32); w.u8(m.flags);
  return w.ok() ? w.size() : 0;
}
bool decodePairOffer(const uint8_t* p, size_t n, PairOffer& m) {
  Reader r(p, n);
  r.bytes(m.target, 6); r.bytes(m.pub, 32); m.flags = r.u8();
  return r.ok();
}
size_t encodePairKey(const PairKey& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  if (m.len > kPairKeyMax) return 0;
  w.bytes(m.target, 6); w.u8(m.len); w.bytes(m.sealed, kPairKeyMax); w.bytes(m.tag, 16);
  return w.ok() ? w.size() : 0;
}
bool decodePairKey(const uint8_t* p, size_t n, PairKey& m) {
  Reader r(p, n);
  r.bytes(m.target, 6); m.len = r.u8(); r.bytes(m.sealed, kPairKeyMax); r.bytes(m.tag, 16);
  return r.ok() && m.len <= kPairKeyMax;
}

uint8_t dashViewOf(const uint8_t* p, size_t n) { return n ? p[0] : (uint8_t)DV_NONE; }

size_t encodeDashDevices(const DashDevices& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  const uint8_t n = m.n > kDashRows ? kDashRows : m.n;
  w.u8(DV_DEVICES); w.u16(m.total); w.u16(m.offset); w.u8(n);
  for (uint8_t i = 0; i < n; ++i) {
    const DashDevRow& d = m.rows[i];
    w.u32(d.id); w.u8(d.category); w.i8(d.rssi); w.u8(d.conf); w.u8(d.flags); w.u8(d.age_s); w.u8(d.radio);
    putText(w, d.text, kDashText);
  }
  return w.ok() ? w.size() : 0;
}
bool decodeDashDevices(const uint8_t* p, size_t n, DashDevices& m) {
  Reader r(p, n);
  m = DashDevices();
  if (r.u8() != DV_DEVICES) return false;
  m.total = r.u16(); m.offset = r.u16(); m.n = r.u8();
  if (!r.ok() || m.n > kDashRows) return false;
  for (uint8_t i = 0; i < m.n; ++i) {
    DashDevRow& d = m.rows[i];
    d.id = r.u32(); d.category = r.u8(); d.rssi = r.i8(); d.conf = r.u8(); d.flags = r.u8(); d.age_s = r.u8(); d.radio = r.u8();
    if (!r.ok() || !getText(r, d.text, kDashText)) return false;
  }
  return r.ok();
}

size_t encodeDashDetail(const DashDetail& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(DV_DETAIL); w.u32(m.id); w.u8(m.found ? 1 : 0);
  if (m.found) {
    w.bytes(m.addr, 6);
    w.u8(m.category); w.u8(m.radio); w.u8(m.conf); w.u8(m.flags); w.u8(m.channel); w.u8(m.rotations); w.u8(m.places);
    w.u16(m.company); w.i8(m.rssi); w.i8(m.rssi_max); w.u32(m.age_s); w.u32(m.first_s); w.u32(m.hits);
    putText(w, m.sig, kDashText);
    putText(w, m.name, kMaxName);
  }
  return w.ok() ? w.size() : 0;
}
bool decodeDashDetail(const uint8_t* p, size_t n, DashDetail& m) {
  Reader r(p, n);
  m = DashDetail();
  if (r.u8() != DV_DETAIL) return false;
  m.id = r.u32(); m.found = r.u8() != 0;
  if (!r.ok()) return false;
  if (m.found) {
    r.bytes(m.addr, 6);
    m.category = r.u8(); m.radio = r.u8(); m.conf = r.u8(); m.flags = r.u8(); m.channel = r.u8(); m.rotations = r.u8(); m.places = r.u8();
    m.company = r.u16(); m.rssi = r.i8(); m.rssi_max = r.i8(); m.age_s = r.u32(); m.first_s = r.u32(); m.hits = r.u32();
    if (!r.ok() || !getText(r, m.sig, kDashText) || !getText(r, m.name, kMaxName)) return false;
  }
  return r.ok();
}

size_t encodeDashEvents(const DashEvents& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  const uint8_t n = m.n > kDashEvents ? kDashEvents : m.n;
  w.u8(DV_EVENTS); w.u16(m.total); w.u16(m.offset); w.u8(n);
  for (uint8_t i = 0; i < n; ++i) {
    const DashEvent& e = m.ev[i];
    w.u8(e.level); w.u8(e.category); w.u32(e.age_s);
    putText(w, e.text, kDashAlertText);
  }
  return w.ok() ? w.size() : 0;
}
bool decodeDashEvents(const uint8_t* p, size_t n, DashEvents& m) {
  Reader r(p, n);
  m = DashEvents();
  if (r.u8() != DV_EVENTS) return false;
  m.total = r.u16(); m.offset = r.u16(); m.n = r.u8();
  if (!r.ok() || m.n > kDashEvents) return false;
  for (uint8_t i = 0; i < m.n; ++i) {
    DashEvent& e = m.ev[i];
    e.level = r.u8(); e.category = r.u8(); e.age_s = r.u32();
    if (!r.ok() || !getText(r, e.text, kDashAlertText)) return false;
  }
  return r.ok();
}

size_t encodeDashNodes(const DashNodes& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  const uint8_t n = m.n > kDashNodes ? kDashNodes : m.n;
  w.u8(DV_NODES); w.u8(m.total); w.u8(m.offset); w.u8(n);
  for (uint8_t i = 0; i < n; ++i) {
    const DashNode& d = m.nodes[i];
    w.u16(d.id); w.u8(d.hw); w.u8(d.flags); w.u8(d.caps); w.u8(d.chans); w.u8(d.duty); w.i8(d.link);
    w.u8(d.loss_pct); w.u8(d.batt); w.u8(d.gps); w.u8(d.sats); w.u8(d.reset);
    w.u16(d.heap_kb); w.u16(d.losses); w.u32(d.up_s);
    putText(w, d.label, kDashLabel);
  }
  return w.ok() ? w.size() : 0;
}
bool decodeDashNodes(const uint8_t* p, size_t n, DashNodes& m) {
  Reader r(p, n);
  m = DashNodes();
  if (r.u8() != DV_NODES) return false;
  m.total = r.u8(); m.offset = r.u8(); m.n = r.u8();
  if (!r.ok() || m.n > kDashNodes) return false;
  for (uint8_t i = 0; i < m.n; ++i) {
    DashNode& d = m.nodes[i];
    d.id = r.u16(); d.hw = r.u8(); d.flags = r.u8(); d.caps = r.u8(); d.chans = r.u8(); d.duty = r.u8(); d.link = r.i8();
    d.loss_pct = r.u8(); d.batt = r.u8(); d.gps = r.u8(); d.sats = r.u8(); d.reset = r.u8();
    d.heap_kb = r.u16(); d.losses = r.u16(); d.up_s = r.u32();
    if (!r.ok() || !getText(r, d.label, kDashLabel)) return false;
  }
  return r.ok();
}

size_t encodeName(const Name& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  const uint8_t len = m.len > kMaxName ? (uint8_t)kMaxName : m.len;
  w.bytes(m.addr, 6); w.u8(m.kind); w.u8(len); w.bytes((const uint8_t*)m.name, len);
  if (m.what != NAME_DEVICE && m.what < 0x10) w.u8((uint8_t)(0xB0 + m.what));
  if (m.has_ap) {
    size_t wl = 0;
    while (wl < kMaxWps && m.ap.wps[wl]) ++wl;
    w.u8(0xA1); w.u8(m.ap.sec); w.u8(m.ap.gen); w.u8(m.ap.width); w.u8(m.ap.util); w.u16(m.ap.sta);
    w.u8((uint8_t)wl); w.bytes((const uint8_t*)m.ap.wps, wl);
    if (m.ap.up_s) { w.u8(0xA2); w.u32(m.ap.up_s); }
  }
  return w.ok() ? w.size() : 0;
}
bool decodeName(const uint8_t* p, size_t n, Name& m) {
  Reader r(p, n);
  r.bytes(m.addr, 6); m.kind = r.u8(); m.len = r.u8();
  if (!r.ok() || m.len > kMaxName) return false;
  r.bytes((uint8_t*)m.name, m.len);
  m.name[m.len] = '\0';
  if (!r.ok()) return false;
  m.what = NAME_DEVICE;
  m.has_ap = false;
  m.ap = ApMeta();
  while (r.remaining()) {   // optional tails; anything unknown ends the parse
    const uint8_t tag = r.u8();
    if (tag > 0xB0 && tag <= 0xBF) { m.what = (uint8_t)(tag - 0xB0); continue; }
    if (tag == 0xA2) { if (r.remaining() < 4) return false; m.ap.up_s = r.u32(); continue; }
    if (tag != 0xA1 || r.remaining() < 7) break;
    m.ap.sec = r.u8(); m.ap.gen = r.u8(); m.ap.width = r.u8(); m.ap.util = r.u8(); m.ap.sta = r.u16();
    uint8_t wl = r.u8();
    if (wl > kMaxWps || wl > r.remaining()) return false;
    r.bytes((uint8_t*)m.ap.wps, wl);
    m.ap.wps[wl] = '\0';
    m.has_ap = true;
  }
  return r.ok();
}

size_t encodeEvent(const Event& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(m.type); w.u8(m.channel); w.u16(m.count); w.bytes(m.src, 6); w.bytes(m.dst, 6); w.bytes(m.bssid, 6);
  return w.ok() ? w.size() : 0;
}
bool decodeEvent(const uint8_t* p, size_t n, Event& m) {
  Reader r(p, n);
  m.type = r.u8(); m.channel = r.u8(); m.count = r.u16(); r.bytes(m.src, 6); r.bytes(m.dst, 6); r.bytes(m.bssid, 6);
  return r.ok();
}

size_t encodeCommand(const NodeCommand& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  const uint8_t len = m.len > kMaxLabel ? (uint8_t)kMaxLabel : m.len;
  w.u8(m.cmd); w.u8(m.arg); w.u8(len); w.bytes((const uint8_t*)m.text, len);
  return w.ok() ? w.size() : 0;
}
bool decodeCommand(const uint8_t* p, size_t n, NodeCommand& m) {
  Reader r(p, n);
  m.cmd = r.u8(); m.arg = r.u8(); m.len = r.u8();
  if (!r.ok() || m.len > kMaxLabel) return false;
  r.bytes((uint8_t*)m.text, m.len);
  m.text[m.len] = '\0';
  return r.ok();
}

static void writeSighting(Writer& w, const Sighting& s) {
  w.bytes(s.addr, 6); w.u8(s.kind); w.u8(s.channel); w.i8(s.rssi_max); w.i8(s.rssi_last);
  w.u8(s.hits); w.u8(s.category); w.u16(s.sig_id); w.u8(s.conf); w.u8(s.flags);
  w.u16(s.company); w.u32(s.fp); w.u16(s.age_ms);
}
static void readSighting(Reader& r, Sighting& s) {
  r.bytes(s.addr, 6); s.kind = r.u8(); s.channel = r.u8(); s.rssi_max = r.i8(); s.rssi_last = r.i8();
  s.hits = r.u8(); s.category = r.u8(); s.sig_id = r.u16(); s.conf = r.u8(); s.flags = r.u8();
  s.company = r.u16(); s.fp = r.u32(); s.age_ms = r.u16();
}

size_t encodeSightings(const Sighting* s, size_t count, uint8_t* out, size_t cap) {
  if (count > kMaxSightingsPerFrame) return 0;
  Writer w(out, cap);
  w.u8((uint8_t)count);
  for (size_t i = 0; i < count; ++i) writeSighting(w, s[i]);
  return w.ok() ? w.size() : 0;
}

bool decodeSightings(const uint8_t* p, size_t n, Sighting* out, size_t max, size_t* count) {
  Reader r(p, n);
  const uint8_t c = r.u8();
  if (!r.ok() || c > kMaxSightingsPerFrame || r.remaining() != (size_t)c * kSightingSize) return false;
  size_t kept = 0;
  for (uint8_t i = 0; i < c; ++i) {
    Sighting tmp;
    readSighting(r, tmp);
    if (kept < max) out[kept++] = tmp;
  }
  if (count) *count = kept;
  return r.ok();
}

// ---------------------------------------------------------------------------
// COBS

size_t cobsEncode(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
  if (cap < cobsMaxEncoded(n)) return 0;
  size_t code_idx = 0, o = 1;
  uint8_t code = 1;
  for (size_t i = 0; i < n; ++i) {
    if (in[i] == 0) {
      out[code_idx] = code;
      code_idx = o++;
      code = 1;
    } else {
      out[o++] = in[i];
      if (++code == 0xFF) {
        out[code_idx] = code;
        code_idx = o++;
        code = 1;
      }
    }
  }
  out[code_idx] = code;
  return o;
}

size_t cobsDecode(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
  size_t i = 0, o = 0;
  while (i < n) {
    const uint8_t code = in[i++];
    if (code == 0) return 0;
    for (uint8_t j = 1; j < code; ++j) {
      if (i >= n || o >= cap || in[i] == 0) return 0;
      out[o++] = in[i++];
    }
    if (code != 0xFF && i < n) {
      if (o >= cap) return 0;
      out[o++] = 0;
    }
  }
  return o;
}

bool CobsDeframer::feed(uint8_t byte) {
  if (byte != 0) {
    if (len_ < sizeof(buf_)) buf_[len_++] = byte;
    else overflow_ = true;
    return false;
  }
  // Delimiter: try to decode what we have.
  bool ready = false;
  if (len_ > 0 && !overflow_) {
    out_len_ = cobsDecode(buf_, len_, out_, sizeof(out_));
    if (out_len_ > 0) ready = true;
    else ++errors_;
  } else if (overflow_) {
    ++errors_;
  }
  len_ = 0;
  overflow_ = false;
  return ready;
}

// ---------------------------------------------------------------------------
// Misc

static const char* const kCatNames[CAT_COUNT] = {
  "none", "flock", "bodycam", "tracker", "glasses", "drone", "skimmer", "pentest",
  "phone", "audio", "wearable", "other_ble", "other_wifi", "lora", "vehicle", "subghz",
};
static const char* const kCatLabels[CAT_COUNT] = {
  "None", "ALPR", "Body cams", "Trackers", "Rec. glasses", "Drones", "Skimmers", "Pentest gear",
  "Phones", "Audio", "Wearables", "Other BLE", "Other Wi-Fi", "LoRa / Meshtastic", "Vehicles", "Sub-GHz",
};
const char* categoryName(uint8_t cat) { return cat < CAT_COUNT ? kCatNames[cat] : "unknown"; }
const char* categoryLabel(uint8_t cat) { return cat < CAT_COUNT ? kCatLabels[cat] : "Unknown"; }

void formatMac(const uint8_t mac[6], char out[18]) {
  static const char hex[] = "0123456789ABCDEF";
  size_t c = 0;
  for (int i = 0; i < 6; ++i) {
    out[c++] = hex[mac[i] >> 4];
    out[c++] = hex[mac[i] & 0x0F];
    if (i < 5) out[c++] = ':';
  }
  out[c] = '\0';
}

}  // namespace ss
