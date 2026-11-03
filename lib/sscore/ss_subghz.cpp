#include "ss_subghz.h"

namespace ss {

const SubGhzBand kSubGhzBandList[kSubGhzBands] = {
    {"315 MHz", 315.00f, "car fobs, tire sensors"},
    {"345 MHz", 345.00f, "alarm sensors"},
    {"390 MHz", 390.00f, "garage remotes"},
    {"433.92", 433.92f, "fobs, sensors, remotes"},
    {"915 MHz", 915.00f, "meters, ISM gear"},
};

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
