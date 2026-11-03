#include "ss_dash.h"

#include <stdio.h>
#include <string.h>

#include "ss_classify.h"

namespace ss {

static constexpr uint32_t kListedMs = 60000;   // gone from the list after a minute
static constexpr uint32_t kQuietMs = 10000;    // dimmed after 10 s
static constexpr uint32_t kAlertHoldMs = 60000;

uint8_t alertLevel(const Device& d) {
  if (d.category == CAT_TRACKER && d.sig_id == SIG_FINDMY_SEPARATED) return 2;
  const bool strong = d.flags & SF_STRONG_MATCH;
  if ((d.category == CAT_FLOCK || d.category == CAT_BODYCAM) && (strong || d.conf >= 75)) return 2;
  if ((kSurveillanceMask & catBit(d.category)) && d.conf >= 40) return 1;
  return 0;
}

void dashLabel(const Device& d, char* out, size_t cap) {
  if (!cap) return;
  snprintf(out, cap, "%s", d.name[0] ? d.name : sigName(d.sig_id));
}

void buildDash(const DeviceTable& table, uint32_t now_ms, uint16_t cats, Dash& out) {
  out.cats = cats;
  for (uint8_t c = 1; c < CAT_COUNT; ++c) {
    const size_t n = table.countNow(c, now_ms);
    out.now[c] = (uint8_t)(n > 255 ? 255 : n);
  }
  const size_t all = table.countNowAll(now_ms);
  out.devices_now = (uint16_t)(all > 0xFFFF ? 0xFFFF : all);

  // Keep the best kDashThreats by score without allocating.
  struct Pick { const Device* d; uint32_t score; };
  Pick best[kDashThreats];
  uint8_t n = 0;
  table.forEach([&](const Device& d) {
    if (d.hidden || d.alias_of || d.spoofed) return;
    if (!(kSurveillanceMask & catBit(d.category)) || !(cats & catBit(d.category))) return;
    const uint32_t age = now_ms - d.last_ms;
    if ((int32_t)age > (int32_t)kListedMs) return;
    const uint8_t level = alertLevel(d);
    if (!level) return;
    const uint32_t score = ((uint32_t)(d.follow_alerted ? 1 : 0) << 20) | ((uint32_t)level << 16) |
                           ((uint32_t)(age <= kQuietMs ? 1 : 0) << 8) | (uint32_t)(uint8_t)(d.rssi_last + 128);
    uint8_t at = n;
    while (at > 0 && best[at - 1].score < score) --at;
    if (at >= kDashThreats) return;
    const uint8_t last = n < kDashThreats ? n : kDashThreats - 1;
    for (uint8_t i = last; i > at; --i) best[i] = best[i - 1];
    best[at] = {&d, score};
    if (n < kDashThreats) ++n;
  });
  out.n_threats = n;
  for (uint8_t i = 0; i < n; ++i) {
    const Device& d = *best[i].d;
    DashThreat& t = out.threats[i];
    t = DashThreat();
    t.category = d.category;
    t.rssi = d.rssi_last;
    t.conf = d.conf;
    if (d.follow_alerted) t.flags |= DT_FOLLOWING;
    if (d.flags & SF_STRONG_MATCH) t.flags |= DT_STRONG;
    if (now_ms - d.last_ms > kQuietMs) t.flags |= DT_QUIET;
    dashLabel(d, t.text, sizeof(t.text));
  }
}

static uint8_t radioOf(const Device& d) { return d.radio <= 4 ? d.radio : 0; }

void buildDeviceView(const DeviceTable& table, uint32_t now_ms, uint16_t cats, uint16_t offset, DashDevices& out) {
  out = DashDevices();
  out.offset = offset;
  auto listed = [&](const Device& d) {
    return !d.hidden && !d.alias_of && !d.spoofed && (cats & catBit(d.category)) &&
           (int32_t)(now_ms - d.last_ms) <= (int32_t)kListedMs;
  };
  // score: confidence first, then how recently heard. Higher = earlier in the list.
  auto score = [&](const Device& d) -> uint32_t {
    const uint32_t age = now_ms - d.last_ms;
    return ((uint32_t)d.conf << 16) | (age > 0xFFFF ? 0 : 0xFFFF - age);
  };
  // The row at position p is the device with exactly p devices ranked above it (ties
  // broken by id). Small tables (a few hundred to 2048 entries) and 6 rows a second:
  // counting passes are cheap and need no memory.
  uint32_t total = 0;
  table.forEach([&](const Device& d) { if (listed(d)) ++total; });
  out.total = (uint16_t)(total > 0xFFFF ? 0xFFFF : total);
  auto above = [&](const Device& a, const Device& b) {  // a ranks above b
    const uint32_t sa = score(a), sb = score(b);
    return sa != sb ? sa > sb : a.id < b.id;
  };
  table.forEach([&](const Device& d) {
    if (!listed(d)) return;
    uint32_t rank = 0;
    table.forEach([&](const Device& o) { if (&o != &d && listed(o) && above(o, d)) ++rank; });
    if (rank < offset || rank >= (uint32_t)offset + kDashRows) return;
    DashDevRow& r = out.rows[rank - offset];
    r.id = d.id;
    r.category = d.category;
    r.rssi = d.rssi_last;
    r.conf = d.conf;
    r.flags = (uint8_t)(((d.flags & SF_STRONG_MATCH) ? DR_STRONG : 0) | (d.follow_alerted ? DR_FOLLOWING : 0) |
                        ((d.flags & SF_RANDOM_ADDR) ? DR_RANDOM : 0));
    const uint32_t age = (now_ms - d.last_ms) / 1000;
    r.age_s = (uint8_t)(age > 255 ? 255 : age);
    r.radio = radioOf(d);
    dashLabel(d, r.text, sizeof(r.text));
  });
  const uint32_t left = total > offset ? total - offset : 0;
  out.n = (uint8_t)(left < kDashRows ? left : kDashRows);
}

void buildDetail(DeviceTable& table, uint32_t id, uint32_t now_ms, DashDetail& out) {
  out = DashDetail();
  out.id = id;
  const Device* d = table.byId(id);
  if (!d) return;
  out.found = true;
  memcpy(out.addr, d->addr, 6);
  out.category = d->category;
  out.radio = radioOf(*d);
  out.conf = d->conf;
  out.flags = (uint8_t)(((d->flags & SF_STRONG_MATCH) ? DR_STRONG : 0) | (d->follow_alerted ? DR_FOLLOWING : 0) |
                        ((d->flags & SF_RANDOM_ADDR) ? DR_RANDOM : 0) | (d->spoofed ? DR_SPOOFED : 0));
  out.channel = d->channel;
  out.rotations = d->rotations;
  out.places = d->places;
  out.company = d->company;
  out.rssi = d->rssi_last;
  out.rssi_max = d->rssi_max;
  out.age_s = (now_ms - d->last_ms) / 1000;
  out.first_s = (now_ms - d->first_ms) / 1000;
  out.hits = d->hits;
  snprintf(out.sig, sizeof(out.sig), "%s", sigName(d->sig_id));
  snprintf(out.name, sizeof(out.name), "%s", d->name);
}

void DashAlerts::note(uint8_t level, uint8_t category, const char* text, uint32_t now_ms) {
  Item& it = hist_[head_];
  it.level = level;
  it.category = category;
  it.ms = now_ms;
  snprintf(it.text, sizeof(it.text), "%s", text);
  head_ = (uint8_t)((head_ + 1) % kHistory);
  if (n_ < kHistory) ++n_;
}

void DashAlerts::page(uint16_t offset, uint32_t now_ms, DashEvents& out) const {
  out = DashEvents();
  out.total = n_;
  out.offset = offset;
  for (uint8_t i = 0; i < kDashEvents && offset + i < n_; ++i) {
    const Item& it = hist_[(head_ + kHistory - 1 - offset - i) % kHistory];  // newest first
    DashEvent& e = out.ev[i];
    e.level = it.level;
    e.category = it.category;
    e.age_s = (now_ms - it.ms) / 1000;
    snprintf(e.text, sizeof(e.text), "%s", it.text);
    out.n = (uint8_t)(i + 1);
  }
}

void DashAlerts::raise(uint8_t level, uint8_t category, const char* text, uint32_t now_ms) {
  if (!level) return;
  note(level, category, text, now_ms);
  if (a_.seq && level < a_.level && now_ms - ms_ < kAlertHoldMs) return;
  a_.seq = (uint16_t)(a_.seq + 1 ? a_.seq + 1 : 1);
  a_.level = level;
  a_.category = category;
  snprintf(a_.text, sizeof(a_.text), "%s", text);
  ms_ = now_ms;
}

void DashAlerts::fill(DashAlert& out, uint32_t now_ms) const {
  out = a_;
  const uint32_t age = a_.seq ? (now_ms - ms_) / 1000 : 0;
  out.age_s = (uint16_t)(age > 0xFFFF ? 0xFFFF : age);
}

}  // namespace ss
