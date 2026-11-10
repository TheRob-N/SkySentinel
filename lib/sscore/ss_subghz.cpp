#include "ss_subghz.h"

namespace ss {

const SubGhzBand kSubGhzBandList[kSubGhzBands] = {
    {"315 MHz", 315.00f, "Ford fobs, tire sensors"},   // e.g. Ford N5F-A08TAA
    {"345 MHz", 345.00f, "alarm sensors"},
    {"390 MHz", 390.00f, "garage remotes"},
    {"433.92", 433.92f, "fobs, sensors, remotes"},
    {"915 MHz", 915.00f, "meters, ISM gear"},
    // Toyota / Lexus smart keys (e.g. HYQ14FBA) send at 314.3 MHz, outside the 315 MHz
    // look's 650 kHz window (Nov 10: a Ford fob showed, a Toyota fob didn't).
    {"314.35", 314.35f, "Toyota / Lexus fobs"},
};
const uint8_t kSubGhzBandOrder[kSubGhzBands] = {5, 0, 1, 2, 3, 4};

Sighting subGhzSighting(uint8_t band, int8_t peak_dbm) {
  Sighting s;
  if (band >= kSubGhzBands) band = 0;
  const uint32_t khz = (uint32_t)(kSubGhzBandList[band].mhz * 1000.0f + 0.5f);
  s.addr[0] = 0x02; s.addr[1] = 0x53; s.addr[2] = 0x47;
  s.addr[3] = (uint8_t)(khz >> 16); s.addr[4] = (uint8_t)(khz >> 8); s.addr[5] = (uint8_t)khz;
  s.kind = KIND_SUBGHZ;
  s.channel = band;
  s.rssi_max = s.rssi_last = peak_dbm;
  s.hits = 1;
  s.category = CAT_SUBGHZ;
  s.sig_id = (uint16_t)(210 + band);   // SIG_SUBGHZ_BAND + band (ss_classify.h)
  s.conf = 50;
  s.flags = 0;
  s.company = 0xFFFF;
  return s;
}

uint32_t subGhzKhz(const uint8_t a[6]) {
  if (a[0] != 0x02 || a[1] != 0x53 || a[2] != 0x47) return 0;
  return ((uint32_t)a[3] << 16) | ((uint32_t)a[4] << 8) | a[5];
}

void SubGhzWatch::onLook(uint8_t b, int8_t median, int8_t peak, uint32_t now_ms) {
  if (b >= kSubGhzBands) return;
  ++looks_[b];
  last_[b] = median;
  peak_[b] = peak;
  // Noise floor: follows the median down quickly, up very slowly (so a jammer or a busy
  // minute doesn't become "normal"). Starts at the first look.
  const int32_t m16 = (int32_t)median * 16;
  if (!floor_[b]) floor_[b] = m16;
  else if (m16 < floor_[b]) floor_[b] += (m16 - floor_[b]) / 4;
  else floor_[b] += (m16 - floor_[b]) / 256 + 1;
  const int8_t fl = floorDbm(b);
  const bool warm = looks_[b] > 8;

  // Burst: a transmission came and went inside this look.
  const bool burst = warm && peak >= fl + pol_.burst_db;
  if (burst && !in_burst_[b]) {
    ++bursts_[b];
    ring_[ring_head_] = {b, peak, now_ms};
    ring_head_ = (uint8_t)((ring_head_ + 1) % 8);
    if (ring_n_ < 8) ++ring_n_;
  }
  in_burst_[b] = burst;

  // Jamming: the whole look (median) stays loud.
  if (warm && median >= fl + pol_.jam_db) {
    if (!loud_since_[b]) loud_since_[b] = now_ms ? now_ms : 1;
    if (now_ms - loud_since_[b] >= pol_.jam_ms && (!jam_at_[b] || now_ms - jam_at_[b] >= pol_.hold_ms) && !pending_) {
      jam_at_[b] = now_ms ? now_ms : 1;
      ev_ = Event();
      ev_.type = EV_RF_JAM;
      ev_.channel = b;
      ev_.count = (uint16_t)(kSubGhzBandList[b].mhz * 100.0f + 0.5f > 65535 ? 65535 : kSubGhzBandList[b].mhz * 100.0f + 0.5f);
      ev_.dst[0] = (uint8_t)(median - fl);
      pending_ = true;
    }
  } else {
    loud_since_[b] = 0;
  }
}

bool SubGhzWatch::poll(Event& out) {
  if (!pending_) return false;
  out = ev_;
  pending_ = false;
  return true;
}

bool SubGhzWatch::jammed(uint8_t b, uint32_t now_ms) const {
  return b < kSubGhzBands && loud_since_[b] && now_ms - loud_since_[b] >= pol_.jam_ms;
}

uint8_t SubGhzWatch::recent(SubGhzBurst* out, uint8_t max) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < ring_n_ && n < max; ++i) out[n++] = ring_[(uint8_t)(ring_head_ + 8 - 1 - i) % 8];
  return n;
}

}  // namespace ss
