#include "ss_devices.h"

#include <string.h>

#include "ss_classify.h"

namespace ss {

static int absDiff(int a, int b) { return a > b ? a - b : b - a; }

bool isSpoofableSig(uint16_t sig_id) {
  switch (sig_id) {
    case SIG_APPLE_AUDIO: case SIG_APPLE_DEVICE: case SIG_FAST_PAIR: case SIG_SAMSUNG_DEVICE:
    case SIG_MICROSOFT: case SIG_GENERIC_BLE: case SIG_FINDMY_NEAR_OWNER:
    case SIG_SWIFT_PAIR: case SIG_SWIFT_PAIR_AUDIO:  // Flipper's "Windows" spam
      return true;
    default:
      return false;
  }
}

DeviceTable::DeviceTable(Device* storage, size_t capacity, StitchPolicy policy, SpamPolicy spam)
    : dev_(storage), cap_(capacity), pol_(policy), spam_pol_(spam) {
  for (size_t i = 0; i < cap_; ++i) dev_[i] = Device();
}

void DeviceTable::addClusterMac(const uint8_t mac[6]) {
  if (n_cluster_ >= kMaxClusterMacs || isClusterMac(mac)) return;
  memcpy(cluster_[n_cluster_++], mac, 6);
}

// ESP32-family chips derive their interface MACs from one base address:
// Wi-Fi STA = base, soft-AP = base + 1, Bluetooth = base + 2. Match all of them.
bool DeviceTable::isClusterMac(const uint8_t mac[6]) const {
  for (uint8_t i = 0; i < n_cluster_; ++i) {
    if (memcmp(cluster_[i], mac, 5) != 0) continue;
    const int d = (int)mac[5] - (int)cluster_[i][5];
    if (d >= -2 && d <= 3) return true;
  }
  return false;
}

Device* DeviceTable::find(const uint8_t addr[6], uint8_t radio) {
  for (size_t i = 0; i < cap_; ++i)
    if (dev_[i].used && dev_[i].radio == radio && memcmp(dev_[i].addr, addr, 6) == 0) return &dev_[i];
  return nullptr;
}

Device* DeviceTable::byId(uint32_t id) {
  for (size_t i = 0; i < cap_; ++i)
    if (dev_[i].used && dev_[i].id == id) return &dev_[i];
  return nullptr;
}

Device* DeviceTable::alloc(uint32_t now_ms) {
  // Full: drop the stalest fake (spam) entry first, so a flood can't push real devices out.
  Device* lru = nullptr;
  Device* fake = nullptr;
  for (size_t i = 0; i < cap_; ++i) {
    if (!dev_[i].used) return &dev_[i];
    if (!lru || dev_[i].last_ms < lru->last_ms) lru = &dev_[i];
    if (dev_[i].spoofed && (!fake || dev_[i].last_ms < fake->last_ms)) fake = &dev_[i];
  }
  (void)now_ms;
  if (fake) lru = fake;
  if (lru) retire(*lru);  // evict the least recently seen; session totals keep it
  return lru;
}

void DeviceTable::retire(Device& d) { d = Device(); }

static void recount(uint32_t* totals, Device& d) {
  if (d.category == d.counted_cat || d.spoofed || d.alias_of || d.hidden) return;
  if (d.counted_cat < CAT_COUNT && totals[d.counted_cat] > 0) --totals[d.counted_cat];
  if (d.category < CAT_COUNT) ++totals[d.category];
  d.counted_cat = d.category;
}

DeviceTable::Result DeviceTable::ingest(uint8_t node_slot, const Sighting& s, uint32_t now_ms, Device** out) {
  const uint8_t radio = radioClass(s.kind);
  if (radio == 0 || s.category >= CAT_COUNT) return REJECTED;
  if (isClusterMac(s.addr)) return SELF;
  const uint32_t seen = now_ms >= s.age_ms ? now_ms - s.age_ms : now_ms;
  const uint8_t slot = node_slot < kMaxNodes ? node_slot : kMaxNodes - 1;

  Device* d = find(s.addr, radio);
  Result r = UPDATED;
  if (!d) {
    d = alloc(now_ms);
    if (!d) return REJECTED;
    d->used = true;
    d->id = next_id_++;
    memcpy(d->addr, s.addr, 6);
    d->radio = radio;
    d->kind = s.kind;
    d->category = s.category;
    d->counted_cat = s.category;
    d->sig_id = s.sig_id;
    d->conf = s.conf;
    d->first_ms = d->addr_first_ms = seen;
    for (uint8_t i = 0; i < kMaxNodes; ++i) { d->node_rssi[i] = -127; d->node_ms[i] = 0; }
    ++totals_[s.category];
    r = NEW;
  }

  if (seen > d->last_ms) d->last_ms = seen;
  d->hits += s.hits ? s.hits : 1;
  d->rssi_last = s.rssi_last;
  if (s.rssi_max > d->rssi_max) d->rssi_max = s.rssi_max;
  d->channel = s.channel;
  d->flags |= s.flags;
  if (d->kind != s.kind && kindIsWifi(s.kind) && s.kind == KIND_WIFI_BEACON) d->kind = s.kind;
  if (s.company != 0xFFFF) d->company = s.company;
  if (s.fp && !d->fp) d->fp = s.fp;
  if (s.conf > d->conf) {
    d->category = s.category;
    d->sig_id = s.sig_id;
    d->conf = s.conf;
    recount(totals_, *d);
  }
  d->node_rssi[slot] = s.rssi_last;
  d->node_ms[slot] = seen;
  if (r == NEW && (s.flags & SF_SPAM_PROBE)) {
    d->hidden = true;
    uncount(*d);
  } else if (d->hidden && !(s.flags & SF_SPAM_PROBE) && !d->spoofed) {
    d->hidden = false;  // its category got switched on: count it now
    d->counted_cat = d->category;
    if (d->category < CAT_COUNT) ++totals_[d->category];
  }
  if (r == NEW) {
    if (radio == 1 && (d->kind == KIND_WIFI_BEACON || d->kind == KIND_WIFI_PROBE_RESP)) {
      groupAccessPoint(*d);
      if (!d->alias_of && d->kind == KIND_WIFI_BEACON) noteNewNetwork(*d, seen);
    }
    // No random-address requirement: spam tools often send random bytes with the
    // *public* address type (seen with a Flipper), so the flag can't be trusted.
    if (radio == 2 && isSpoofableSig(d->sig_id)) noteSpoofable(*d, seen);
    if (radio == 2 && d->sig_id == SIG_FLIPPER) collapseFlipper(*d, seen);
  }
  if (out) *out = d;
  return r;
}

// A Flipper changes its address every time a spam app restarts its advertising, so
// one Flipper turns into a dozen "Flipper Zero" rows. Fold a new one into a Flipper
// heard recently at a similar signal.
void DeviceTable::collapseFlipper(Device& d, uint32_t now_ms) {
  Device* best = nullptr;
  for (size_t i = 0; i < cap_; ++i) {
    Device& o = dev_[i];
    if (&o == &d || !o.used || o.radio != 2 || o.sig_id != SIG_FLIPPER) continue;
    if (now_ms - o.last_ms > 60000 || absDiff(o.rssi_last, d.rssi_last) > 12) continue;
    if (!best || o.last_ms > best->last_ms) best = &o;
  }
  if (best) merge(*best, d);
}

// Takes a device out of the per-category totals (spoofed entries, AP aliases).
void DeviceTable::uncount(Device& d) {
  if (d.counted_cat != CAT_NONE && d.counted_cat < CAT_COUNT && totals_[d.counted_cat] > 0) --totals_[d.counted_cat];
  d.counted_cat = CAT_NONE;
}

// Routers broadcast several networks from BSSIDs that differ only in the first
// byte (locally administered variants) and/or the low nibble of the last byte.
// Group them under the first one seen.
void DeviceTable::groupAccessPoint(Device& d) {
  for (size_t i = 0; i < cap_; ++i) {
    Device& p = dev_[i];
    if (&p == &d || !p.used || p.radio != 1 || p.alias_of) continue;
    if (p.kind != KIND_WIFI_BEACON && p.kind != KIND_WIFI_PROBE_RESP) continue;
    if (memcmp(p.addr + 1, d.addr + 1, 4) != 0) continue;
    if ((p.addr[5] & 0xF0) != (d.addr[5] & 0xF0)) continue;
    d.alias_of = p.id;
    if (p.aliases < 255) ++p.aliases;
    uncount(d);
    return;
  }
}

void DeviceTable::noteSpoofable(Device& d, uint32_t now_ms) {
  // A burst already identified: anything new in the same RSSI band joins it.
  if (spamActive(now_ms) && d.rssi_last >= spam_lo_ && d.rssi_last <= spam_hi_) {
    d.spoofed = true;
    uncount(d);
    ++spoofed_;
    if ((int32_t)(now_ms - spam_last_ms_) > 0) spam_last_ms_ = now_ms;
    if (spam_event_.count < 0xFFFF) ++spam_event_.count;
    return;
  }

  spam_[spam_head_] = {d.id, now_ms, d.rssi_last, d.sig_id};
  spam_head_ = (uint8_t)((spam_head_ + 1) % kSpamRing);

  // Recent hits, sorted by RSSI; look for min_burst of them inside rssi_band dB.
  SpamHit recent[kSpamRing];
  uint8_t n = 0;
  for (const SpamHit& h : spam_)
    if (h.id && absDiff((int)(int32_t)(now_ms - h.ms), 0) <= (int)spam_pol_.window_ms) recent[n++] = h;
  if (n < spam_pol_.min_burst) return;
  for (uint8_t i = 1; i < n; ++i)  // insertion sort, n <= 48
    for (uint8_t j = i; j > 0 && recent[j].rssi < recent[j - 1].rssi; --j) {
      const SpamHit t = recent[j]; recent[j] = recent[j - 1]; recent[j - 1] = t;
    }
  for (uint8_t lo = 0; lo + spam_pol_.min_burst <= n; ++lo) {
    uint8_t hi = lo;
    while (hi + 1 < n && recent[hi + 1].rssi - recent[lo].rssi <= spam_pol_.rssi_band) ++hi;
    const uint8_t size = (uint8_t)(hi - lo + 1);
    if (size < spam_pol_.min_burst) continue;
    uint16_t types[kSpamRing];
    uint8_t nt = 0;
    for (uint8_t k = lo; k <= hi; ++k) {
      bool seen = false;
      for (uint8_t t = 0; t < nt; ++t) seen = seen || types[t] == recent[k].sig;
      if (!seen) types[nt++] = recent[k].sig;
    }
    if (nt < spam_pol_.min_types && size < spam_pol_.min_burst_one_type) continue;
    // Burst found (tight band = confident it is one transmitter). Sweep a wider band
    // around its centre, since one transmitter's RSSI fades by several dB.
    const int centre = (recent[lo].rssi + recent[hi].rssi) / 2;
    const bool already = spamActive(now_ms);
    const int8_t blo = (int8_t)(centre - spam_pol_.sweep_db), bhi = (int8_t)(centre + spam_pol_.sweep_db);
    // While an attack is ongoing (the attacker may be moving), widen rather than replace.
    spam_lo_ = already && spam_lo_ < blo ? spam_lo_ : blo;
    spam_hi_ = already && spam_hi_ > bhi ? spam_hi_ : bhi;
    uint16_t marked = 0;
    for (uint8_t k = 0; k < n; ++k) {
      if (recent[k].rssi < spam_lo_ || recent[k].rssi > spam_hi_) continue;
      Device* m = byId(recent[k].id);
      if (!m || m->spoofed) continue;
      m->spoofed = true;
      uncount(*m);
      ++spoofed_;
      ++marked;
    }
    for (SpamHit& h : spam_) h.id = 0;  // consumed
    const bool fresh = !already;
    spam_active_ = true;
    if ((int32_t)(now_ms - spam_last_ms_) > 0) spam_last_ms_ = now_ms;
    if (fresh) {
      spam_event_ = Event();
      spam_event_.type = EV_BLE_SPAM;
      memcpy(spam_event_.src, d.addr, 6);
      spam_pending_ = true;
    }
    spam_event_.count = (uint16_t)(spam_event_.count + marked);
    spam_event_.channel = (uint8_t)(int8_t)centre;  // burst RSSI, stored raw
    return;
  }
}

// Beacon flood: real routers appear a few at a time and at all sorts of signal levels;
// a spammer makes dozens of new networks a second from one radio, so at one level.
// Private (random) BSSIDs are what spam tools use; real routers' extra networks with
// such addresses are grouped under their router first (groupAccessPoint), so a burst of
// ungrouped random ones is telling: 10 in 5 s within 8 dB. Fixed addresses need 25.
void DeviceTable::noteNewNetwork(Device& d, uint32_t now_ms) {
  const bool rnd = (d.addr[0] & 0x02) != 0;
  if (wifiSpamActive(now_ms) && d.rssi_last >= wspam_lo_ && d.rssi_last <= wspam_hi_) {
    d.spoofed = true;
    uncount(d);
    ++spoofed_;
    if ((int32_t)(now_ms - wspam_last_ms_) > 0) wspam_last_ms_ = now_ms;
    if (wspam_event_.count < 0xFFFF) ++wspam_event_.count;
    return;
  }
  wspam_[wspam_head_] = {d.id, now_ms, d.rssi_last, (uint16_t)(rnd ? 1 : 0)};
  wspam_head_ = (uint8_t)((wspam_head_ + 1) % kWspamRing);
  SpamHit recent[kWspamRing];
  uint8_t n = 0;
  for (const SpamHit& h : wspam_)
    if (h.id && absDiff((int)(int32_t)(now_ms - h.ms), 0) <= (int)spam_pol_.window_ms) recent[n++] = h;
  if (n < 10) return;
  for (uint8_t i = 1; i < n; ++i)
    for (uint8_t j = i; j > 0 && recent[j].rssi < recent[j - 1].rssi; --j) {
      const SpamHit t = recent[j]; recent[j] = recent[j - 1]; recent[j - 1] = t;
    }
  for (uint8_t lo = 0; lo < n; ++lo) {
    uint8_t hi = lo, nrnd = 0;
    while (hi + 1 < n && recent[hi + 1].rssi - recent[lo].rssi <= 8) ++hi;
    for (uint8_t k = lo; k <= hi; ++k) nrnd += recent[k].sig ? 1 : 0;
    const uint8_t size = (uint8_t)(hi - lo + 1);
    if (nrnd < 10 && size < 25) continue;
    const int centre = (recent[lo].rssi + recent[hi].rssi) / 2;
    const bool already = wifiSpamActive(now_ms);
    const int8_t blo = (int8_t)(centre - spam_pol_.sweep_db), bhi = (int8_t)(centre + spam_pol_.sweep_db);
    wspam_lo_ = already && wspam_lo_ < blo ? wspam_lo_ : blo;
    wspam_hi_ = already && wspam_hi_ > bhi ? wspam_hi_ : bhi;
    uint16_t marked = 0;
    for (uint8_t k = 0; k < n; ++k) {
      if (recent[k].rssi < wspam_lo_ || recent[k].rssi > wspam_hi_) continue;
      Device* m = byId(recent[k].id);
      if (!m || m->spoofed) continue;
      m->spoofed = true;
      uncount(*m);
      ++spoofed_;
      ++marked;
    }
    for (SpamHit& h : wspam_) h.id = 0;
    wspam_active_ = true;
    if ((int32_t)(now_ms - wspam_last_ms_) > 0) wspam_last_ms_ = now_ms;
    if (!already) {
      wspam_event_ = Event();
      wspam_event_.type = EV_WIFI_SPAM;
      memcpy(wspam_event_.src, d.addr, 6);
      wspam_pending_ = true;
    }
    wspam_event_.count = (uint16_t)(wspam_event_.count + marked);
    wspam_event_.channel = (uint8_t)(int8_t)centre;
    return;
  }
}

bool DeviceTable::pollSpam(Event& out) {
  if (wspam_pending_) {
    out = wspam_event_;
    wspam_pending_ = false;
    return true;
  }
  if (!spam_pending_) return false;
  out = spam_event_;
  spam_pending_ = false;
  return true;
}

Device* DeviceTable::setName(const Name& n) {
  Device* d = find(n.addr, radioClass(n.kind));
  if (!d) return nullptr;
  if (n.what != NAME_DEVICE) return d;   // a probed network or a position: not its name
  if (n.has_ap) {
    d->ap_sec = n.ap.sec; d->ap_gen = n.ap.gen; d->ap_width = n.ap.width;
    d->ap_util = n.ap.util; d->ap_sta = n.ap.sta;
    if (n.ap.up_s) { d->ap_up_s = n.ap.up_s; d->ap_up_ms = d->last_ms; }
  }
  // A Meshtastic node's bare id ("!a1b2c3d4") never replaces the name it told us.
  if (n.len && !(n.name[0] == '!' && d->name[0] && d->name[0] != '!')) {
    const size_t l = n.len > kMaxName ? kMaxName : n.len;
    memcpy(d->name, n.name, l);
    d->name[l] = '\0';
  }
  return d;
}

bool DeviceTable::stitchable(const Device& d) const {
  return d.used && !d.spoofed && d.fp != 0 && (d.flags & SF_RANDOM_ADDR) && !(d.flags & SF_STATIC_RAND);
}

void DeviceTable::merge(Device& older, Device& newer) {
  // The older record's counted category already covers this logical device.
  if (newer.counted_cat < CAT_COUNT && totals_[newer.counted_cat] > 0) --totals_[newer.counted_cat];
  newer.counted_cat = older.counted_cat;

  newer.id = older.id;
  newer.first_ms = older.first_ms;
  newer.hits += older.hits;
  newer.rotations = (uint8_t)(older.rotations < 254 ? older.rotations + 1 : 255);
  if (older.rssi_max > newer.rssi_max) newer.rssi_max = older.rssi_max;
  if (older.conf > newer.conf) {
    newer.category = older.category;
    newer.sig_id = older.sig_id;
    newer.conf = older.conf;
  }
  if (!newer.name[0] && older.name[0]) memcpy(newer.name, older.name, sizeof(newer.name));
  if (newer.company == 0xFFFF) newer.company = older.company;
  // A rotated address is the same device that has been with us all along.
  if (older.places > newer.places) newer.places = older.places;
  if (older.last_place > newer.last_place) newer.last_place = older.last_place;
  newer.follow_alerted = newer.follow_alerted || older.follow_alerted;
  for (uint8_t i = 0; i < kMaxNodes; ++i) {
    if (older.node_ms[i] > newer.node_ms[i]) {
      newer.node_ms[i] = older.node_ms[i];
      newer.node_rssi[i] = older.node_rssi[i];
    }
  }
  recount(totals_, newer);
  retire(older);
  newer.stitch_done = true;
  ++stitches_;
}

size_t DeviceTable::maintain(uint32_t now_ms) {
  size_t merges = 0;
  for (size_t i = 0; i < cap_; ++i) {
    Device& d = dev_[i];
    if (!stitchable(d) || d.stitch_done) continue;
    if (now_ms - d.addr_first_ms > pol_.window_ms) { d.stitch_done = true; continue; }

    const bool wifi = d.radio == 1;  // radioClass(): Wi-Fi probe client
    const uint32_t quiet = wifi ? pol_.probe_quiet_ms : pol_.quiet_ms;
    const uint32_t handoff = wifi ? pol_.probe_handoff_ms : pol_.handoff_ms;
    Device* best = nullptr;
    int best_diff = 1000, second_diff = 1000;
    for (size_t j = 0; j < cap_; ++j) {
      if (j == i) continue;
      Device& o = dev_[j];
      if (!stitchable(o) || o.radio != d.radio || o.fp != d.fp) continue;
      if (o.addr_first_ms >= d.addr_first_ms) continue;          // must be the older address
      if (now_ms - o.last_ms < quiet) continue;                    // old address must have gone quiet
      if (wifi && o.last_ms > d.addr_first_ms + 1000) continue;    // both searching at once: two phones
      if (absDiff((int)o.last_ms, (int)d.addr_first_ms) > (int)handoff) continue;
      const int diff = absDiff(o.rssi_last, d.rssi_last);
      if (diff > pol_.rssi_window) continue;
      if (diff < best_diff) { second_diff = best_diff; best_diff = diff; best = &o; }
      else if (diff < second_diff) second_diff = diff;
    }
    if (!best) continue;
    const bool unique = second_diff == 1000;
    if (unique || second_diff - best_diff >= pol_.margin) {
      merge(*best, d);
      ++merges;
    }
  }
  return merges;
}

size_t DeviceTable::countNow(uint8_t category, uint32_t now_ms, uint32_t window_ms) const {
  size_t n = 0;
  for (size_t i = 0; i < cap_; ++i)
    if (dev_[i].used && !dev_[i].spoofed && !dev_[i].alias_of && !dev_[i].hidden && dev_[i].category == category &&
        now_ms - dev_[i].last_ms <= window_ms)
      ++n;
  return n;
}

size_t DeviceTable::countNowAll(uint32_t now_ms, uint32_t window_ms) const {
  size_t n = 0;
  for (size_t i = 0; i < cap_; ++i)
    if (dev_[i].used && !dev_[i].spoofed && !dev_[i].alias_of && !dev_[i].hidden && now_ms - dev_[i].last_ms <= window_ms)
      ++n;
  return n;
}

uint32_t DeviceTable::totalAll() const {
  uint32_t t = 0;
  for (uint8_t c = 1; c < CAT_COUNT; ++c) t += totals_[c];
  return t;
}

size_t DeviceTable::used() const {
  size_t n = 0;
  for (size_t i = 0; i < cap_; ++i) if (dev_[i].used) ++n;
  return n;
}

int bestNode(const Device& d, uint32_t now_ms, uint32_t window_ms, int8_t* rssi, uint8_t* count) {
  int best = -1, latest = -1;
  uint8_t n = 0;
  for (uint8_t i = 0; i < kMaxNodes; ++i) {
    if (d.node_rssi[i] == -127 || !d.node_ms[i]) continue;
    if (latest < 0 || (int32_t)(d.node_ms[i] - d.node_ms[latest]) > 0) latest = i;
    if ((int32_t)(now_ms - d.node_ms[i]) > (int32_t)window_ms) continue;
    ++n;
    if (best < 0 || d.node_rssi[i] > d.node_rssi[best]) best = i;
  }
  if (best < 0) best = latest;
  if (count) *count = n;
  if (rssi) *rssi = best >= 0 ? d.node_rssi[best] : -127;
  return best;
}

}  // namespace ss
