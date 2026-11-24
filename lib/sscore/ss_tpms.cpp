#include "ss_tpms.h"

#include <stdio.h>
#include <string.h>

namespace ss {

namespace {

// Half-bit period per radio setup, microseconds.
constexpr uint16_t kFskT = 52;
constexpr uint16_t kOokT = 122;
// A run longer than this many half-bits ends a frame (FSK preambles and Manchester data
// never hold a level that long; OOK gaps between frames are long and quiet).
constexpr uint16_t kFskMaxRun = 12;
constexpr uint16_t kOokMaxRun = 4;

struct Bits {
  static constexpr uint16_t kMax = 1024;
  uint8_t b[kMax / 8] = {0};
  uint16_t n = 0;
  void push(uint8_t v) {
    if (n >= kMax) return;
    const uint8_t m = (uint8_t)(0x80 >> (n & 7));
    if (v) b[n >> 3] |= m; else b[n >> 3] &= (uint8_t)~m;
    ++n;
  }
  uint8_t at(uint16_t i) const { return i < n ? (uint8_t)((b[i >> 3] >> (7 - (i & 7))) & 1) : 0; }
};

constexpr uint16_t kNotFound = 0xFFFF;

// First position at or after `from` where the top `nbits` of `pat` appear.
uint16_t find(const Bits& s, uint16_t from, const uint8_t* pat, uint16_t nbits) {
  for (uint16_t p = from; p + nbits <= s.n; ++p) {
    uint16_t k = 0;
    while (k < nbits && s.at((uint16_t)(p + k)) == ((pat[k >> 3] >> (7 - (k & 7))) & 1)) ++k;
    if (k == nbits) return p;
  }
  return kNotFound;
}

// Packs decoded bits MSB first.
struct Out {
  uint8_t b[24];
  uint16_t n = 0;
  Out() { memset(b, 0, sizeof(b)); }
  void push(uint8_t v) {
    if (n >= sizeof(b) * 8) return;
    if (v) b[n >> 3] |= (uint8_t)(0x80 >> (n & 7));
    ++n;
  }
};

// Manchester: each data bit is two half-bits that differ; the bit is the second half.
// Stops at the first pair that doesn't differ, or after `max` bits. Returns where it stopped.
uint16_t manchester(const Bits& s, uint16_t pos, uint16_t max, Out& o) {
  while (pos + 2 <= s.n && o.n < max) {
    const uint8_t a = s.at(pos), b = s.at((uint16_t)(pos + 1));
    if (a == b) break;
    o.push(b);
    pos = (uint16_t)(pos + 2);
  }
  return pos;
}

// Differential Manchester: a level change at the start of every bit (the clock); a second
// change mid-bit is a 0, none is a 1. Leading zeros come before the first long run.
uint16_t diffManchester(const Bits& s, uint16_t pos, uint16_t max, Out& o) {
  uint8_t prev = 0;
  bool synced = false;
  while (pos + 2 <= s.n && o.n < max) {
    const uint8_t a = s.at(pos), b = s.at((uint16_t)(pos + 1)), c = s.at((uint16_t)(pos + 2));
    if (a != b) {
      if (b != c) { o.push(0); pos = (uint16_t)(pos + 2); continue; }
      prev = a;           // b == c: realign on the half-bit after a
      pos = (uint16_t)(pos + 1);
    } else {
      prev = (uint8_t)(1 - a);
    }
    synced = true;
    break;
  }
  if (!synced) return pos;
  while (pos + 2 <= s.n && o.n < max) {
    const uint8_t a = s.at(pos);
    if (a == prev) break;   // no clock change: the frame is over
    const uint8_t b = s.at((uint16_t)(pos + 1));
    o.push(a == b ? 1 : 0);
    prev = b;
    pos = (uint16_t)(pos + 2);
  }
  return pos;
}

uint8_t crc8(const uint8_t* m, size_t n, uint8_t poly, uint8_t init) {
  uint8_t r = init;
  for (size_t i = 0; i < n; ++i) {
    r ^= m[i];
    for (int k = 0; k < 8; ++k) r = (uint8_t)((r & 0x80) ? (r << 1) ^ poly : r << 1);
  }
  return r;
}

uint8_t sum8(const uint8_t* m, size_t n) {
  uint8_t s = 0;
  for (size_t i = 0; i < n; ++i) s = (uint8_t)(s + m[i]);
  return s;
}

uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int16_t psiX10(float psi) {
  if (!(psi >= 0.0f && psi <= 150.0f)) return -1;
  return (int16_t)(psi * 10.0f + 0.5f);
}
int16_t kpaToPsiX10(float kpa) { return psiX10(kpa / 6.894757f); }

struct Found {
  TpmsReading* out;
  uint8_t max, n = 0;
  void add(uint8_t maker, uint32_t id, int16_t psi, int16_t temp) {
    for (uint8_t i = 0; i < n; ++i)
      if (out[i].maker == maker && out[i].id == id) return;   // the burst's repeats
    if (n >= max) return;
    TpmsReading& r = out[n++];
    r.maker = maker;
    r.id = id;
    r.psi_x10 = psi;
    r.temp_c = temp < -60 || temp > 200 ? (int16_t)-128 : temp;
  }
};

// --- FSK sensors ------------------------------------------------------------------------

void toyota(const Bits& s, Found& f) {
  static const uint8_t pat[2] = {0xA9, 0xE0};   // 1010 1001 1110 (sync 0x553C, 12 bits)
  for (uint16_t p = 0; (p = find(s, p, pat, 12)) != kNotFound && p + 156 <= s.n; p = (uint16_t)(p + 2)) {
    Out o;
    const uint16_t end = diffManchester(s, (uint16_t)(p + 11), 72, o);
    if (o.n < 72 || end - (p + 11) < 144) continue;
    const uint8_t* b = o.b;
    if (crc8(b, 8, 0x07, 0x80) != b[8]) continue;
    const uint8_t p1 = (uint8_t)(((b[4] & 0x7F) << 1) | (b[5] >> 7));
    if (p1 != (uint8_t)(b[7] ^ 0xFF)) continue;   // pressure is sent twice, once inverted
    const int16_t t = (int16_t)((((b[5] & 0x7F) << 1) | (b[6] >> 7)) - 40);
    f.add(TPMS_TOYOTA, be32(b), psiX10(p1 * 0.25f - 7.0f), t);
  }
}

void ford(const Bits& s, Found& f) {
  static const uint8_t pat[2] = {0xAA, 0xA9};
  for (uint16_t p = 0; (p = find(s, p, pat, 16)) != kNotFound; p = (uint16_t)(p + 2)) {
    Out o;
    manchester(s, (uint16_t)(p + 16), 64, o);
    if (o.n < 64) continue;
    const uint8_t* b = o.b;
    if (sum8(b, 7) != b[7]) continue;
    const uint8_t st = b[6] & 0x4C;
    if ((st != 0x08 && st != 0x04 && st != 0x44) || (b[6] & 0x90)) continue;   // learn / rest / moving only
    const int psi4 = ((b[6] & 0x20) << 3) | b[4];
    const int16_t t = (b[5] & 0x80) ? (int16_t)-128 : (int16_t)((b[5] & 0x7F) - 56);
    f.add(TPMS_FORD, be32(b), psiX10(psi4 * 0.25f), t);
  }
}

void vdo(const Bits& s, Found& f) {
  static const uint8_t pat[4] = {0xAA, 0xAA, 0xAA, 0xA9};
  for (uint16_t p = 0; (p = find(s, p, pat, 32)) != kNotFound && p + 32 + 160 <= s.n; p = (uint16_t)(p + 2)) {
    Out o;
    manchester(s, (uint16_t)(p + 32), 80, o);
    if (o.n < 80) continue;
    const uint8_t* b = o.b;
    if (crc8(b, 9, 0x07, 0xAA) != b[9]) continue;
    f.add(TPMS_VDO, be32(b + 1), psiX10(b[6] / 5.0f), (int16_t)(b[7] - 50));
  }
}

void continental(const Bits& s, Found& f) {
  static const uint8_t pat[2] = {0xAA, 0xA9};
  for (uint16_t p = 0; (p = find(s, p, pat, 16)) != kNotFound && p + 16 + 160 <= s.n; p = (uint16_t)(p + 2)) {
    Out o;
    manchester(s, (uint16_t)(p + 16), 88, o);
    if (o.n < 80) continue;
    const uint8_t* b = o.b;
    if (!b[6] || !b[7]) continue;
    uint8_t x = 0;
    for (int i = 1; i <= 9; ++i) x ^= b[i];
    if (x) continue;
    // Jeep's scale (2.728 kPa a step); Citroen sends the same frame at half that.
    f.add(TPMS_CONTINENTAL, be32(b + 1), kpaToPsiX10(b[6] * 2.728f), (int16_t)(b[7] - 50));
  }
}

void trw(const Bits& s, Found& f) {
  static const uint8_t pat[2] = {0x71, 0x55};
  for (uint16_t p = 0; (p = find(s, p, pat, 16)) != kNotFound && p + 16 + 128 <= s.n; p = (uint16_t)(p + 15)) {
    Out o;
    manchester(s, (uint16_t)(p + 16), 64, o);
    if (o.n < 64) continue;
    const uint8_t* b = o.b;
    if (crc8(b, 8, 0x07, 0x00) != 0) continue;
    if (!b[2] && !b[3] && !b[4] && !b[5]) continue;
    f.add(TPMS_TRW, be32(b + 2), kpaToPsiX10((float)(b[0] + 60)), (int16_t)(b[1] - 50));
  }
}

void kia(const Bits& s, Found& f) {
  static const uint8_t pat[2] = {0xED, 0x71};
  for (uint16_t p = 0; (p = find(s, p, pat, 16)) != kNotFound && p + 154 <= s.n; p = (uint16_t)(p + 2)) {
    Out o;
    manchester(s, (uint16_t)(p + 16), 69, o);
    if (o.n < 69) continue;
    const uint8_t* b = o.b;
    if ((uint8_t)(b[8] & 0xF8) != crc8(b, 8, 0x07, 0x76)) continue;   // 5 CRC bits, padded with 000
    const uint32_t id = ((uint32_t)b[2] << 28) | ((uint32_t)b[3] << 20) | ((uint32_t)b[4] << 12) | ((uint32_t)b[5] << 4) | (b[6] >> 4);
    if (!id) continue;
    const uint8_t pr = (uint8_t)((b[0] << 4) | (b[1] >> 4)), te = (uint8_t)((b[1] << 4) | (b[2] >> 4));
    f.add(TPMS_KIA, id, psiX10(pr / 5.0f), (int16_t)(te - 50));
  }
}

void honda(const Bits& s, Found& f) {
  static const uint8_t pat[3] = {0xDA, 0xE3, 0x54};   // 23 bits, ends on a deliberate bad pair
  for (uint16_t p = 0; (p = find(s, p, pat, 23)) != kNotFound && p + 23 + 128 <= s.n; p = (uint16_t)(p + 1)) {
    Out o;
    manchester(s, (uint16_t)(p + 23), 64, o);
    if (o.n < 64) continue;
    const uint8_t* b = o.b;
    if (crc8(b, 7, 0x07, 0x00) != b[7]) continue;
    if (b[0] > 0 && b[0] < 50) continue;   // implausible: a TRW Elantra frame read wrong
    f.add(TPMS_HONDA, be32(b + 2), psiX10(b[0] * 0.2f), (int16_t)(b[1] - 50));
  }
}

void decodeFskRow(const Bits& s, Found& f) {
  toyota(s, f);
  vdo(s, f);
  ford(s, f);
  continental(s, f);
  trw(s, f);
  kia(s, f);
  honda(s, f);
}

// --- OOK sensors (Schrader) -------------------------------------------------------------

// OOK Manchester as Schrader sends it: the frame starts at its first rising edge, which is
// the middle of a 0 bit; low-then-high is 0, high-then-low is 1.
void zerobit(const Bits& halves, Out& o, uint16_t max) {
  Bits h;
  h.push(0);
  for (uint16_t i = 0; i < halves.n; ++i) h.push(halves.at(i));
  for (uint16_t i = 0; i + 2 <= h.n && o.n < max; i = (uint16_t)(i + 2)) {
    const uint8_t a = h.at(i), b = h.at((uint16_t)(i + 1));
    if (a == b) break;
    o.push(a);
  }
}

void bitsAt(const Out& o, uint16_t from, uint8_t* out, uint8_t nbytes) {
  memset(out, 0, nbytes);
  for (uint16_t k = 0; k < nbytes * 8u; ++k) {
    const uint16_t i = (uint16_t)(from + k);
    if (i < o.n && ((o.b[i >> 3] >> (7 - (i & 7))) & 1)) out[k >> 3] |= (uint8_t)(0x80 >> (k & 7));
  }
}

void decodeOokRow(const Bits& halves, Found& f) {
  Out o;
  zerobit(halves, o, 192);
  if (o.n >= 120 && o.n <= 121) {   // Schrader EG53MA4: 40 sync bits, then 10 bytes
    uint8_t b[10];
    bitsAt(o, 40, b, 10);
    const bool blank = !b[1] && !b[2] && !b[4] && !b[5] && !b[7] && !b[8];
    if (!blank && sum8(b, 9) == b[9]) {
      const uint32_t id = ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 8) | b[6];
      f.add(TPMS_SCHRADER_GM, id, kpaToPsiX10(b[7] * 2.5f), (int16_t)((b[8] - 32) * 5 / 9));
    }
  }
  if (o.n >= 68 && o.n <= 69) {     // Schrader MRXGG4: a sync nibble, then 8 bytes
    uint8_t b[8];
    bitsAt(o, 4, b, 8);
    if (crc8(b, 7, 0x07, 0xF0) == b[7]) {
      const uint32_t id = ((uint32_t)(b[1] & 0x0F) << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
      if (id) f.add(TPMS_SCHRADER, id, kpaToPsiX10(b[5] * 2.5f), (int16_t)(b[6] - 50));
    }
  }
}

void decodeRow(const Bits& row, TpmsMode mode, Found& f) {
  if (row.n < 40) return;
  if (mode == TPMS_OOK) { decodeOokRow(row, f); return; }
  decodeFskRow(row, f);
  // Which frequency the radio calls "high" depends on its setup: try the other way too.
  static Bits inv;
  inv.n = 0;
  for (uint16_t i = 0; i < row.n; ++i) inv.push((uint8_t)(1 - row.at(i)));
  decodeFskRow(inv, f);
}

}  // namespace

const char* tpmsMakerName(uint8_t m) {
  switch (m) {
    case TPMS_TOYOTA: return "Toyota";
    case TPMS_FORD: return "Ford";
    case TPMS_VDO: return "Hyundai/Kia (VDO)";
    case TPMS_CONTINENTAL: return "Jeep/Chrysler";
    case TPMS_TRW: return "Hyundai/Honda (TRW)";
    case TPMS_KIA: return "Kia";
    case TPMS_HONDA: return "Honda";
    case TPMS_SCHRADER_GM: return "GM (Schrader)";
    case TPMS_SCHRADER: return "Schrader";
    default: return "TPMS";
  }
}

uint8_t tpmsDecode(const uint16_t* runs, size_t n, TpmsMode mode, TpmsReading* out, uint8_t max) {
  Found f;
  f.out = out;
  f.max = max;
  if (!runs || !out || !max) return 0;
  const uint16_t T = mode == TPMS_OOK ? kOokT : kFskT;
  const uint16_t maxRun = mode == TPMS_OOK ? kOokMaxRun : kFskMaxRun;
  static Bits row;
  row.n = 0;
  auto endRow = [&]() {
    decodeRow(row, mode, f);
    row.n = 0;
  };
  auto emit = [&](uint8_t level, uint32_t us) {
    const uint32_t k = (us + T / 2) / T;
    if (k > maxRun) {
      // OOK: a gap after a frame ending in a 1 still holds that bit's low half.
      if (mode == TPMS_OOK && !level && row.n) row.push(0);
      endRow();
      return;
    }
    if (mode == TPMS_OOK && !row.n && !level) return;   // OOK frames start on a pulse
    for (uint32_t i = 0; i < (k ? k : 1); ++i) row.push(level);
  };
  // Merge glitches (a flip shorter than half a half-bit) into the run around them.
  uint8_t lvl = (runs[0] & kTpmsHigh) ? 1 : 0;
  uint32_t dur = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t l = (runs[i] & kTpmsHigh) ? 1 : 0;
    const uint32_t d = runs[i] & kTpmsUsMask;
    if (l == lvl || d < T / 2u) { dur += d; continue; }
    emit(lvl, dur);
    lvl = l;
    dur = d;
  }
  emit(lvl, dur);
  if (mode == TPMS_OOK && row.n && row.at((uint16_t)(row.n - 1))) row.push(0);
  endRow();
  return f.n;
}

Sighting tpmsSighting(const TpmsReading& r, int8_t rssi, uint8_t band) {
  Sighting s;
  s.addr[0] = 0x02;
  s.addr[1] = r.maker;
  s.addr[2] = (uint8_t)(r.id >> 24); s.addr[3] = (uint8_t)(r.id >> 16);
  s.addr[4] = (uint8_t)(r.id >> 8); s.addr[5] = (uint8_t)r.id;
  s.kind = KIND_TPMS;
  s.channel = band;
  s.rssi_max = s.rssi_last = rssi;
  s.hits = 1;
  s.category = CAT_VEHICLE;
  s.sig_id = 227;   // SIG_TPMS (ss_classify.h)
  s.conf = 75;
  s.flags = 0;      // the ID is fixed for the tire's life
  s.company = 0xFFFF;
  s.fp = 0;
  return s;
}

bool tpmsFromAddr(const uint8_t a[6], uint8_t* maker, uint32_t* id) {
  if (a[0] != 0x02 || a[1] == TPMS_NONE || a[1] >= TPMS_MAKERS) return false;
  if (maker) *maker = a[1];
  if (id) *id = be32(a + 2);
  return true;
}

uint8_t tpmsName(const TpmsReading& r, char* out, size_t cap) {
  if (!out || cap < 2) return 0;
  const bool schrader24 = r.maker == TPMS_SCHRADER_GM;
  char id[10];
  snprintf(id, sizeof(id), schrader24 ? "%06lX" : "%08lX", (unsigned long)r.id);
  int n;
  if (r.psi_x10 >= 0)
    n = snprintf(out, cap, "%s tire %s %d psi", tpmsMakerName(r.maker), id, (r.psi_x10 + 5) / 10);
  else
    n = snprintf(out, cap, "%s tire %s", tpmsMakerName(r.maker), id);
  if (n < 0) return 0;
  return (uint8_t)((size_t)n >= cap ? cap - 1 : (size_t)n);
}

// --- test encoder ------------------------------------------------------------------------

namespace {

struct Enc {
  Bits h;
  void raw(const uint8_t* pat, uint16_t nbits) {
    for (uint16_t k = 0; k < nbits; ++k) h.push((uint8_t)((pat[k >> 3] >> (7 - (k & 7))) & 1));
  }
  void rawStr(const char* s) { for (; *s; ++s) h.push(*s == '1'); }
  void man(const uint8_t* b, uint16_t nbits) {
    for (uint16_t k = 0; k < nbits; ++k) {
      const uint8_t v = (uint8_t)((b[k >> 3] >> (7 - (k & 7))) & 1);
      h.push((uint8_t)(1 - v));
      h.push(v);
    }
  }
  void diffMan(const uint8_t* b, uint16_t nbits) {
    uint8_t prev = h.n ? h.at((uint16_t)(h.n - 1)) : 0;
    for (uint16_t k = 0; k < nbits; ++k) {
      const uint8_t v = (uint8_t)((b[k >> 3] >> (7 - (k & 7))) & 1);
      const uint8_t a = (uint8_t)(1 - prev);
      const uint8_t c = v ? a : (uint8_t)(1 - a);
      h.push(a);
      h.push(c);
      prev = c;
    }
  }
  void invert() { for (uint16_t i = 0; i < h.n; ++i) h.b[i >> 3] ^= (uint8_t)(0x80 >> (i & 7)); }
  // Half-bits to runs, with a little timing wobble, between quiet gaps.
  size_t runs(uint16_t T, uint16_t* out, size_t cap) {
    size_t n = 0;
    auto put = [&](uint8_t l, uint32_t us) { if (n < cap) out[n++] = (uint16_t)((l ? kTpmsHigh : 0) | (us > kTpmsUsMask ? kTpmsUsMask : us)); };
    put(0, 3000);   // quiet (or noise) before
    uint16_t i = 0, wob = 0;
    while (i < h.n) {
      const uint8_t l = h.at(i);
      uint16_t k = 0;
      while (i < h.n && h.at(i) == l) { ++i; ++k; }
      const int j = (int)(wob++ % 5) - 2;   // -2..+2 us
      put(l, (uint32_t)(k * T + j));
    }
    put(0, 3000);
    return n;
  }
};

}  // namespace

size_t tpmsEncodeForTest(uint8_t maker, uint32_t id, uint8_t pr, uint8_t te, uint16_t* runs, size_t cap) {
  Enc e;
  uint8_t b[12] = {0};
  auto putId = [&](uint8_t* p) { p[0] = (uint8_t)(id >> 24); p[1] = (uint8_t)(id >> 16); p[2] = (uint8_t)(id >> 8); p[3] = (uint8_t)id; };
  static const uint8_t pre4[4] = {0xAA, 0xAA, 0xAA, 0xA9};
  switch (maker) {
    case TPMS_TOYOTA:
      putId(b);
      b[4] = (uint8_t)(0x80 | (pr >> 1));
      b[5] = (uint8_t)(((pr & 1) << 7) | (te >> 1));
      b[6] = (uint8_t)(((te & 1) << 7) | 0x01);
      b[7] = (uint8_t)~pr;
      b[8] = crc8(b, 8, 0x07, 0x80);
      e.rawStr("0101010101010101001111");
      e.diffMan(b, 72);
      e.rawStr("00101010111");
      return e.runs(kFskT, runs, cap);
    case TPMS_FORD:
      putId(b);
      b[4] = pr; b[5] = te; b[6] = 0x46;
      b[7] = sum8(b, 7);
      e.raw(pre4, 32); e.man(b, 64); e.rawStr("0110");
      e.invert();
      return e.runs(kFskT, runs, cap);
    case TPMS_VDO:
      b[0] = 0x21; putId(b + 1); b[5] = 0x03; b[6] = pr; b[7] = te; b[8] = 0x5A;
      b[9] = crc8(b, 9, 0x07, 0xAA);
      e.rawStr("1010"); e.raw(pre4, 32); e.man(b, 80); e.rawStr("0110");
      e.invert();
      return e.runs(kFskT, runs, cap);
    case TPMS_CONTINENTAL: {
      b[0] = 0x20; putId(b + 1); b[5] = 0x01; b[6] = pr; b[7] = te; b[8] = 0x33;
      uint8_t x = 0;
      for (int i = 1; i <= 8; ++i) x ^= b[i];
      b[9] = x;
      e.raw(pre4, 32); e.man(b, 80); e.rawStr("01111110");
      e.invert();
      return e.runs(kFskT, runs, cap);
    }
    case TPMS_TRW: {
      static const uint8_t pre[2] = {0x71, 0x55};
      b[0] = pr; b[1] = te; putId(b + 2); b[6] = 0xC0;
      b[7] = crc8(b, 7, 0x07, 0x00);
      e.rawStr("0101"); e.raw(pre, 16); e.man(b, 64); e.rawStr("0110");
      return e.runs(kFskT, runs, cap);
    }
    case TPMS_KIA: {
      static const uint8_t pre[2] = {0xED, 0x71};
      const uint8_t a = 0x0F;
      b[0] = (uint8_t)((a << 4) | (pr >> 4));
      b[1] = (uint8_t)((pr << 4) | (te >> 4));
      b[2] = (uint8_t)((te << 4) | ((id >> 28) & 0x0F));
      b[3] = (uint8_t)(id >> 20); b[4] = (uint8_t)(id >> 12); b[5] = (uint8_t)(id >> 4);
      b[6] = (uint8_t)(((id & 0x0F) << 4) | 0x0A);
      // Only 5 CRC bits are sent and the receiver compares them, padded with 000, to the
      // full CRC: pick the free byte so that holds (real frames always satisfy it).
      for (b[7] = 0; crc8(b, 8, 0x07, 0x76) & 0x07; ++b[7]) {}
      b[8] = crc8(b, 8, 0x07, 0x76);
      e.rawStr("0101"); e.raw(pre, 16); e.man(b, 69); e.rawStr("01100110");
      return e.runs(kFskT, runs, cap);
    }
    case TPMS_HONDA: {
      static const uint8_t mk[3] = {0xDA, 0xE3, 0x54};
      b[0] = pr; b[1] = te; putId(b + 2); b[6] = 0xE1;
      b[7] = crc8(b, 7, 0x07, 0x00);
      e.raw(mk, 23); e.man(b, 64); e.rawStr("0110");
      return e.runs(kFskT, runs, cap);
    }
    case TPMS_SCHRADER_GM:
    case TPMS_SCHRADER: {
      // Data bits as zero-bit Manchester: bit 0 is the frame's leading 0.
      Out d;
      if (maker == TPMS_SCHRADER_GM) {
        for (int i = 0; i < 40; ++i) d.push(i >= 36);   // sync: zeros, then 1111
        b[0] = 0x80; b[1] = 0x12; b[2] = 0x34; b[3] = 0x00;
        b[4] = (uint8_t)(id >> 16); b[5] = (uint8_t)(id >> 8); b[6] = (uint8_t)id;
        b[7] = pr; b[8] = te; b[9] = sum8(b, 9);
        for (int k = 0; k < 80; ++k) d.push((uint8_t)((b[k >> 3] >> (7 - (k & 7))) & 1));
      } else {
        d.push(0); d.push(1); d.push(1); d.push(1);
        b[0] = 0xF6; b[1] = (uint8_t)(0x70 | ((id >> 24) & 0x0F));
        b[2] = (uint8_t)(id >> 16); b[3] = (uint8_t)(id >> 8); b[4] = (uint8_t)id;
        b[5] = pr; b[6] = te; b[7] = crc8(b, 7, 0x07, 0xF0);
        for (int k = 0; k < 64; ++k) d.push((uint8_t)((b[k >> 3] >> (7 - (k & 7))) & 1));
      }
      // 0 = low-then-high, 1 = high-then-low; the leading 0's low half is the quiet before.
      for (uint16_t k = 0; k < d.n; ++k) {
        const uint8_t v = (uint8_t)((d.b[k >> 3] >> (7 - (k & 7))) & 1);
        if (k) e.h.push(v);
        e.h.push((uint8_t)(1 - v));
      }
      return e.runs(kOokT, runs, cap);
    }
    default:
      return 0;
  }
}

}  // namespace ss
