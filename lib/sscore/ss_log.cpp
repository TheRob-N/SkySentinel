#include "ss_log.h"

#include <stdio.h>
#include <string.h>

#include "ss_classify.h"
#include "ss_gps.h"
#include "ss_proto.h"

namespace ss {

const char kLogCsvHeader[] = "time_utc,uptime_s,lat,lon,kind,category,signature,mac,name,rssi,conf,strong,detail\n";

const char* logKindName(uint8_t k) {
  static const char* const n[] = {"", "start", "device", "following", "lingering", "deauth_flood",
                                  "disassoc_flood", "ble_spam", "track", "wifi_spam", "evil_twin", "rf_jam"};
  return k < sizeof(n) / sizeof(n[0]) ? n[k] : "";
}

size_t encodeLogRec(const LogRec& m, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u32(m.seq); w.u32(m.utc); w.u32(m.up_s);
  w.u8((m.has_pos ? 1 : 0) | (m.strong ? 2 : 0) | (m.has_mac ? 4 : 0));
  w.u32((uint32_t)m.lat_e7); w.u32((uint32_t)m.lon_e7);
  w.u8(m.kind); w.u8(m.cat); w.u16(m.sig); w.u8(m.conf); w.i8(m.rssi);
  w.bytes(m.mac, 6);
  w.u16(m.a); w.u16(m.b);
  uint8_t l = 0;
  while (l < kLogName && m.name[l]) ++l;
  w.u8(l);
  w.bytes((const uint8_t*)m.name, l);
  return w.ok() ? w.size() : 0;
}

bool decodeLogRec(const uint8_t* p, size_t n, LogRec& m) {
  Reader r(p, n);
  m.seq = r.u32(); m.utc = r.u32(); m.up_s = r.u32();
  const uint8_t f = r.u8();
  m.has_pos = f & 1; m.strong = f & 2; m.has_mac = f & 4;
  m.lat_e7 = (int32_t)r.u32(); m.lon_e7 = (int32_t)r.u32();
  m.kind = r.u8(); m.cat = r.u8(); m.sig = r.u16(); m.conf = r.u8(); m.rssi = r.i8();
  r.bytes(m.mac, 6);
  m.a = r.u16(); m.b = r.u16();
  const uint8_t l = r.u8();
  if (!r.ok() || l > kLogName) return false;
  r.bytes((uint8_t*)m.name, l);
  m.name[l] = '\0';
  return r.ok();
}

// Degrees * 1e7 -> "41.7481234" (no floating point, exact)
static int coordText(int32_t v, char* out, size_t cap) {
  const uint32_t a = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
  return snprintf(out, cap, "%s%lu.%07lu", v < 0 ? "-" : "", (unsigned long)(a / 10000000u),
                  (unsigned long)(a % 10000000u));
}

size_t formatLogCsv(const LogRec& r, char* out, size_t cap) {
  char t[20] = "", lat[16] = "", lon[16] = "", mac[18] = "", name[2 * kLogName + 3] = "", detail[40] = "";
  if (r.utc) formatUtc(r.utc, t);
  if (r.has_pos) { coordText(r.lat_e7, lat, sizeof(lat)); coordText(r.lon_e7, lon, sizeof(lon)); }
  if (r.has_mac) formatMac(r.mac, mac);
  if (r.name[0]) {  // quoted, quotes doubled, control characters dropped
    size_t o = 0;
    name[o++] = '"';
    for (const char* s = r.name; *s && o < sizeof(name) - 3; ++s) {
      if ((uint8_t)*s < 0x20) continue;
      if (*s == '"') name[o++] = '"';
      name[o++] = *s;
    }
    name[o++] = '"';
    name[o] = '\0';
  }
  switch (r.kind) {
    case LK_DEVICE: snprintf(detail, sizeof(detail), "%s", r.a ? "upgraded" : "new"); break;
    case LK_FOLLOWING:
    case LK_LINGERING: snprintf(detail, sizeof(detail), "places=%u minutes=%u", r.a, r.b); break;
    case LK_DEAUTH:
    case LK_DISASSOC: snprintf(detail, sizeof(detail), "channel=%u frames=%u", r.a, r.b); break;
    case LK_BLE_SPAM: snprintf(detail, sizeof(detail), "fake_devices=%u", r.a); break;
    case LK_WIFI_SPAM: snprintf(detail, sizeof(detail), "fake_networks=%u", r.a); break;
    case LK_EVIL_TWIN: snprintf(detail, sizeof(detail), "network_names=%u", r.a); break;
    case LK_RF_JAM: snprintf(detail, sizeof(detail), "mhz=%u.%02u db_over_floor=%u", r.a / 100, r.a % 100, r.b); break;
    case LK_START: snprintf(detail, sizeof(detail), "boot=%u", r.a); break;
    default: break;
  }
  const bool dev = r.kind == LK_DEVICE || r.kind == LK_FOLLOWING || r.kind == LK_LINGERING;
  char rssi[6] = "", conf[5] = "";
  if (r.rssi != -127 && r.kind != LK_TRACK && r.kind != LK_START) snprintf(rssi, sizeof(rssi), "%d", r.rssi);
  if (dev) snprintf(conf, sizeof(conf), "%u", r.conf);
  char sig[64] = "";
  if (dev && sigName(r.sig)[0]) snprintf(sig, sizeof(sig), "\"%s\"", sigName(r.sig));
  const int n = snprintf(out, cap, "%s,%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", t, (unsigned long)r.up_s, lat, lon,
                         logKindName(r.kind), dev && r.cat ? categoryName(r.cat) : "", sig, mac,
                         name, rssi, conf, dev ? (r.strong ? "yes" : "no") : "", detail);
  if (n <= 0 || (size_t)n >= cap) return 0;
  return (size_t)n;
}

}  // namespace ss
