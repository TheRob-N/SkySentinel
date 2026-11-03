// Channel table, category-driven scan plans, and channel splitting across nodes.
#pragma once

#include <stdint.h>

namespace ss {

// Index i in a channel mask = kChannels[i].
constexpr uint8_t kChannelCount = 38;
extern const uint8_t kChannels[kChannelCount];  // 1..13, then 25 x 5 GHz

constexpr uint64_t kMask24 = (1ull << 13) - 1;                        // ch 1-13
constexpr uint64_t kMask5 = ((1ull << kChannelCount) - 1) & ~kMask24; // 36-165
constexpr uint64_t kMaskAll = kMask24 | kMask5;
constexpr uint8_t kHomeChannel = 6;                                   // ESP-NOW + AP channel
constexpr uint64_t kMaskCh6 = 1ull << 5;

int channelIndex(uint8_t channel);  // -1 if not in the table
uint8_t channelCount(uint64_t mask);
bool channelIs5GHz(uint8_t channel);

// What the radio should do for a set of enabled categories.
struct ScanPlan {
  uint64_t channel_mask = 0;  // Wi-Fi channels to visit (0 = Wi-Fi scanning off)
  uint8_t ble_duty_pct = 0;   // share of radio time for BLE scanning
  uint16_t dwell_ms = 120;    // per Wi-Fi channel
};

// Mirrors the Categories screen: BLE-only categories give BLE 100%, Wi-Fi
// categories add only the channels they need.
ScanPlan planForCategories(uint16_t category_mask);

// Splits a channel mask across `node_count` Wi-Fi nodes by interleaving, so
// every node gets a mix of 2.4 and 5 GHz. Returns node `index`'s share.
uint64_t splitChannels(uint64_t mask, uint8_t node_count, uint8_t index);

// Iterates set bits in order. Returns the channel number for the next set bit
// after position *cursor (wrapping), and updates *cursor. 0 if mask is empty.
uint8_t nextChannel(uint64_t mask, uint8_t* cursor);

}  // namespace ss
