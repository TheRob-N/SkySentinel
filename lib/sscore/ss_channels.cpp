#include "ss_channels.h"

#include "ss_proto.h"

namespace ss {

const uint8_t kChannels[kChannelCount] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
  36, 40, 44, 48, 52, 56, 60, 64,
  100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
  149, 153, 157, 161, 165,
};

int channelIndex(uint8_t channel) {
  for (uint8_t i = 0; i < kChannelCount; ++i)
    if (kChannels[i] == channel) return i;
  return -1;
}

uint8_t channelCount(uint64_t mask) {
  uint8_t n = 0;
  for (; mask; mask &= mask - 1) ++n;
  return n;
}

bool channelIs5GHz(uint8_t channel) { return channel > 14; }

// Which Wi-Fi coverage each category needs.
enum WifiNeed : uint8_t { NEED_NONE = 0, NEED_CH6 = 1, NEED_24 = 2, NEED_ALL = 3 };

static WifiNeed wifiNeedFor(uint8_t cat) {
  switch (cat) {
    case CAT_FLOCK:      return NEED_24;   // camera APs / probes are 2.4 GHz
    case CAT_DRONE:      return NEED_CH6;  // Wi-Fi Remote ID beacons, mostly ch 6
    case CAT_PENTEST:    return NEED_ALL;  // Pineapples, deauth floods: anywhere
    case CAT_PHONE:      return NEED_ALL;  // probe requests on both bands
    case CAT_OTHER_WIFI: return NEED_ALL;
    case CAT_VEHICLE:    return NEED_24;   // car hotspots (and Bluetooth below)
    default:             return NEED_NONE; // BLE-only categories
  }
}

// LoRa is handled by a sub-GHz node, so it needs neither BLE nor Wi-Fi time here.
static bool needsBle(uint8_t cat) { return cat != CAT_OTHER_WIFI && cat != CAT_NONE && cat != CAT_LORA; }

ScanPlan planForCategories(uint16_t category_mask) {
  ScanPlan p;
  WifiNeed need = NEED_NONE;
  bool ble = false;
  for (uint8_t c = 1; c < CAT_COUNT; ++c) {
    if (!(category_mask & catBit(c))) continue;
    const WifiNeed w = wifiNeedFor(c);
    if (w > need) need = w;
    ble = ble || needsBle(c);
  }
  switch (need) {
    case NEED_NONE: p.channel_mask = 0;        p.ble_duty_pct = ble ? 100 : 0; break;
    case NEED_CH6:  p.channel_mask = kMaskCh6; p.ble_duty_pct = ble ? 70 : 0;  break;
    case NEED_24:   p.channel_mask = kMask24;  p.ble_duty_pct = ble ? 50 : 0;  break;
    case NEED_ALL:  p.channel_mask = kMaskAll; p.ble_duty_pct = ble ? 35 : 0;  break;
  }
  // A single channel needs no hopping; otherwise keep dwell short enough that a
  // full 38-channel sweep stays under ~5 s.
  p.dwell_ms = (channelCount(p.channel_mask) <= 1) ? 1000 : 120;
  return p;
}

uint64_t splitChannels(uint64_t mask, uint8_t node_count, uint8_t index) {
  if (node_count <= 1) return mask;
  if (index >= node_count) return 0;
  uint64_t out = 0;
  uint8_t k = 0;
  for (uint8_t i = 0; i < kChannelCount; ++i) {
    if (!(mask & (1ull << i))) continue;
    if (k % node_count == index) out |= 1ull << i;
    ++k;
  }
  // If there are fewer channels than nodes, extra nodes share channel round-robin
  // instead of sitting idle.
  if (out == 0 && mask != 0) {
    const uint8_t n = channelCount(mask);
    uint8_t want = (uint8_t)(index % n), seen = 0;
    for (uint8_t i = 0; i < kChannelCount; ++i) {
      if (!(mask & (1ull << i))) continue;
      if (seen++ == want) { out = 1ull << i; break; }
    }
  }
  return out;
}

uint8_t nextChannel(uint64_t mask, uint8_t* cursor) {
  if (mask == 0) return 0;
  for (uint8_t step = 1; step <= kChannelCount; ++step) {
    const uint8_t i = (uint8_t)((*cursor + step) % kChannelCount);
    if (mask & (1ull << i)) {
      *cursor = i;
      return kChannels[i];
    }
  }
  return 0;
}

}  // namespace ss
