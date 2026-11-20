#include "ss_154.h"

#include <stdio.h>
#include <string.h>

namespace ss {

namespace {

struct Rd {
  const uint8_t* p;
  size_t n, i = 0;
  bool ok = true;
  Rd(const uint8_t* p_, size_t n_) : p(p_), n(n_) {}
  uint8_t u8() { if (i + 1 > n) { ok = false; return 0; } return p[i++]; }
  uint16_t u16() { const uint8_t a = u8(), b = u8(); return (uint16_t)(a | (b << 8)); }
  void skip(size_t k) { if (i + k > n) { ok = false; i = n; } else i += k; }
  void ext(uint8_t out[8]) {   // sent least significant byte first
    if (i + 8 > n) { ok = false; i = n; return; }
    for (int k = 0; k < 8; ++k) out[7 - k] = p[i + k];
    i += 8;
  }
};

// Auxiliary security header length: security control (1) + frame counter (4, unless
// suppressed in 2015 frames) + key identifier (0 / 1 / 5 / 9 by key id mode).
size_t auxLen(uint8_t sc, uint8_t version) {
  size_t l = 1;
  if (!(version >= 2 && (sc & 0x20))) l += 4;
  static const uint8_t kKeyId[4] = {0, 1, 5, 9};
  return l + kKeyId[(sc >> 3) & 0x03];
}

}  // namespace

bool parse154(const uint8_t* p, size_t len, Frame154& f) {
  f = Frame154();
  if (!p || len < 3) return false;
  Rd r(p, len);
  const uint16_t fcf = r.u16();
  f.ftype = fcf & 0x07;
  f.secured = (fcf & 0x08) != 0;
  const bool comp = (fcf & 0x40) != 0;
  const uint8_t dmode = (fcf >> 10) & 0x03, smode = (fcf >> 14) & 0x03;
  f.version = (fcf >> 12) & 0x03;
  if (f.ftype == 2 || f.ftype > 3) return false;   // ACKs say nothing about a sender; ignore the rest
  if (!(f.version >= 2 && (fcf & 0x100))) f.seq = r.u8();   // 2015: sequence number may be suppressed
  if (dmode == 1 || smode == 1) return false;     // reserved addressing mode

  // Which PAN ids are present (802.15.4-2006 rules, and the 2015 table for version 2).
  bool dpan, span;
  if (f.version < 2) {
    dpan = dmode != 0;
    span = smode != 0 && !comp;
  } else if (dmode && smode) {
    if (dmode == 3 && smode == 3) { dpan = !comp; span = false; }
    else { dpan = true; span = !comp; }
  } else if (dmode) { dpan = !comp; span = false; }
  else if (smode) { dpan = false; span = !comp; }
  else { dpan = comp; span = false; }

  uint16_t dpanId = 0xFFFF;
  if (dpan) dpanId = r.u16();
  if (dmode == 2) r.skip(2); else if (dmode == 3) r.skip(8);
  if (span) f.pan = r.u16();
  else if (smode && dpan) f.pan = dpanId;   // compressed: same PAN as the destination
  if (smode == 2) { f.src_short = r.u16(); f.has_short = true; }
  else if (smode == 3) { r.ext(f.src_ext); f.has_ext = true; }
  if (!r.ok) return false;
  if (f.secured) {
    const size_t at = r.i;
    const uint8_t sc = r.u8();
    if (!r.ok) return false;
    r.i = at;
    r.skip(auxLen(sc, f.version));
    if (!r.ok) return false;
  }
  if (f.version >= 2 && (fcf & 0x200)) return smode != 0;   // IE present: leave the payload unread

  const uint8_t* pl = p + r.i;
  const size_t pn = len - r.i;
  if (f.ftype == 0) {   // beacon: superframe (2), GTS, pending addresses, then the payload
    Rd b(pl, pn);
    b.skip(2);
    const uint8_t gts = b.u8();
    const uint8_t ngts = gts & 0x07;
    if (ngts) { b.skip(1); b.skip(3u * ngts); }
    const uint8_t pend = b.u8();
    b.skip(2u * (pend & 0x07) + 8u * ((pend >> 4) & 0x07));
    if (b.ok && b.i < pn) {
      const uint8_t proto = pl[b.i];
      if (proto == 0x00 && pn - b.i >= 3) f.zigbee_beacon = true;   // Zigbee: protocol id 0
      else if (proto == 0x03) f.thread_beacon = true;              // Thread 1.1 beacon
    }
    return smode != 0;
  }
  if (f.ftype == 3) return smode != 0;   // MAC command (association, data request...)

  // Data frame
  if (!pn) return smode != 0;
  if (f.secured) return smode != 0;   // payload encrypted (Thread); classified from the header
  const uint8_t d0 = pl[0];
  // 6LoWPAN dispatch: IPHC 011xxxxx, mesh 10xxxxxx, fragments 11000xxx / 11100xxx, IPv6 0x41
  const bool lowpan = (d0 & 0xE0) == 0x60 || (d0 & 0xC0) == 0x80 || (d0 & 0xF8) == 0xC0 ||
                      (d0 & 0xF8) == 0xE0 || d0 == 0x41;
  // Zigbee NWK frame control: type (bits 0-1) data / command, protocol version (bits 2-5) 2
  if (pn >= 8 && ((d0 >> 2) & 0x0F) == 2 && (d0 & 0x03) <= 1) {
    const uint16_t nfc = (uint16_t)(pl[0] | (pl[1] << 8));
    Rd n(pl, pn);
    n.skip(2 + 2 + 2 + 1 + 1);            // frame control, dst, src, radius, seq
    if (nfc & 0x0800) n.skip(8);          // destination IEEE address
    if (nfc & 0x1000) {                   // source IEEE address: who really sent it
      uint8_t e[8];
      n.ext(e);
      if (n.ok && !f.has_ext) { memcpy(f.src_ext, e, 8); f.has_ext = true; }
    }
    f.kind = KIND_ZIGBEE;
    return smode != 0 || f.has_ext;
  }
  if (lowpan) f.kind = KIND_THREAD;
  return smode != 0;
}

Match classify154(const Frame154& f) {
  Match m;
  m.category = CAT_SMARTHOME;
  if (f.zigbee_beacon) { m.sig_id = SIG_ZIGBEE_COORD; m.conf = 80; return m; }
  if (f.thread_beacon) { m.sig_id = SIG_THREAD; m.conf = 75; return m; }
  if (f.kind == KIND_ZIGBEE) { m.sig_id = SIG_ZIGBEE; m.conf = 70; return m; }
  if (f.kind == KIND_THREAD) { m.sig_id = SIG_THREAD; m.conf = 70; return m; }
  // Encrypted at the MAC layer, 2006 or 2015 frame: Thread (Zigbee never does that)
  if (f.ftype == 1 && f.secured && f.version >= 1) { m.sig_id = SIG_THREAD; m.conf = 55; return m; }
  m.sig_id = SIG_154_OTHER;
  m.conf = 25;
  return m;
}

void addr154(const Frame154& f, uint8_t out[6]) {
  if (f.has_ext) {
    out[0] = f.src_ext[0]; out[1] = f.src_ext[1]; out[2] = f.src_ext[2];
    out[3] = f.src_ext[5]; out[4] = f.src_ext[6]; out[5] = f.src_ext[7];
    return;
  }
  out[0] = 0x02; out[1] = 0x15;
  out[2] = (uint8_t)(f.pan >> 8); out[3] = (uint8_t)f.pan;
  out[4] = (uint8_t)(f.src_short >> 8); out[5] = (uint8_t)f.src_short;
}

Sighting make154Sighting(const Frame154& f, const Match& m, uint8_t channel, int8_t rssi) {
  Sighting s;
  addr154(f, s.addr);
  const uint16_t sig = m.sig_id;
  s.kind = sig == SIG_ZIGBEE || sig == SIG_ZIGBEE_COORD ? KIND_ZIGBEE : sig == SIG_THREAD ? KIND_THREAD : KIND_154_OTHER;
  s.channel = channel;
  s.rssi_max = s.rssi_last = rssi;
  s.hits = 1;
  s.category = m.category;
  s.sig_id = m.sig_id;
  s.conf = m.conf;
  // A short address is handed out by the network and can change: flag it like a private one.
  s.flags = (uint8_t)((f.has_ext ? 0 : SF_RANDOM_ADDR) | (m.strong ? SF_STRONG_MATCH : 0));
  s.company = 0xFFFF;
  s.fp = 0;   // not a fingerprint: short-address devices must never be stitched together
  return s;
}

uint8_t name154(const Frame154& f, const Match& m, char* out, size_t cap) {
  if (!out || cap < 2 || f.pan == 0xFFFF) return 0;
  const char* p = m.sig_id == SIG_THREAD ? "Thread" : m.sig_id == SIG_154_OTHER ? "802.15.4" : "Zigbee";
  const int n = snprintf(out, cap, "%s PAN %04X", p, f.pan);
  return n < 0 ? 0 : (uint8_t)((size_t)n >= cap ? cap - 1 : (size_t)n);
}

}  // namespace ss
