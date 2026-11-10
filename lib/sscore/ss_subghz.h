// Sub-GHz band watch for the LilyGO T-Embed CC1101 (pure C++, host tested).
//
// The radio steps through a few US bands where key fobs, garage remotes, tire sensors,
// alarm sensors and car-theft jammers live, sampling the signal level on each. Per band
// it keeps a noise floor, counts bursts (something transmitted: a fob press, a sensor),
// and raises a jamming alarm when the band stays loud for seconds on end.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ss_proto.h"

namespace ss {

struct SubGhzBand {
  const char* name;   // "315 MHz"
  float mhz;
  const char* what;   // "fobs, tire sensors"
};
// Band numbers stay fixed (alerts and saved settings use them); new bands go at the end
// and screens list them in frequency order (kSubGhzBandOrder).
constexpr uint8_t kSubGhzBands = 6;
extern const SubGhzBand kSubGhzBandList[kSubGhzBands];
extern const uint8_t kSubGhzBandOrder[kSubGhzBands];   // band numbers, lowest frequency first

struct SubGhzPolicy {
  int8_t burst_db = 30;        // a sample this far above the floor is a burst (T-Embed default, Nov 10)
  int8_t jam_db = 20;          // the band's median this far above its floor...
  uint32_t jam_ms = 3000;      // ...for this long is jamming
  uint32_t hold_ms = 60000;    // one jamming alarm per band per minute
};

struct SubGhzBurst { uint8_t band; int8_t peak; uint32_t ms; };

// One row per band in the device list: every burst on that band adds to it. Not a device
// ID (bursts carry none until they're decoded): addr = 02:53:47 ("SG", locally
// administered) + the band's frequency in kHz (3 bytes), channel = band number.
Sighting subGhzSighting(uint8_t band, int8_t peak_dbm);
// The band's kHz back from such an address, 0 if it isn't one.
uint32_t subGhzKhz(const uint8_t addr[6]);

class SubGhzWatch {
 public:
  explicit SubGhzWatch(SubGhzPolicy p = SubGhzPolicy()) : pol_(p) {}
  // Burst sensitivity: dB over the floor that counts as a burst (lower = more bursts).
  void setBurstDb(int8_t db) { pol_.burst_db = db; }
  int8_t burstDb() const { return pol_.burst_db; }
  // One look at a band: the samples' median and peak (dBm) over ~100 ms.
  void onLook(uint8_t band, int8_t median, int8_t peak, uint32_t now_ms);
  // A jamming alarm (once per hold-off per band): EV_RF_JAM, channel = band,
  // count = MHz x 100 (43392), dst[0] = dB above the floor.
  bool poll(Event& out);

  int8_t floorDbm(uint8_t band) const { return band < kSubGhzBands && floor_[band] ? (int8_t)(floor_[band] / 16) : -127; }
  int8_t lastDbm(uint8_t band) const { return band < kSubGhzBands ? last_[band] : -127; }
  int8_t peakDbm(uint8_t band) const { return band < kSubGhzBands ? peak_[band] : -127; }
  uint32_t bursts(uint8_t band) const { return band < kSubGhzBands ? bursts_[band] : 0; }
  bool jammed(uint8_t band, uint32_t now_ms) const;
  // Most recent bursts, newest first (up to 8).
  uint8_t recent(SubGhzBurst* out, uint8_t max) const;

 private:
  SubGhzPolicy pol_;
  int32_t floor_[kSubGhzBands] = {0};      // dBm x 16; 0 = not learned yet
  int8_t last_[kSubGhzBands] = {-127, -127, -127, -127, -127, -127};
  int8_t peak_[kSubGhzBands] = {-127, -127, -127, -127, -127, -127};
  uint32_t bursts_[kSubGhzBands] = {0};
  bool in_burst_[kSubGhzBands] = {false};
  uint32_t loud_since_[kSubGhzBands] = {0};
  uint32_t jam_at_[kSubGhzBands] = {0};
  uint32_t looks_[kSubGhzBands] = {0};
  SubGhzBurst ring_[8] = {};
  uint8_t ring_head_ = 0, ring_n_ = 0;
  Event ev_{};
  bool pending_ = false;
};

}  // namespace ss
