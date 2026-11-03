// "Is something following me?" for a board that moves (vehicle or handheld), and
// "is something lingering?" for one that doesn't. Pure C++, host tested.
//
// Movement without GPS: about once a minute, take the set of Wi-Fi access points
// heard. When most of them differ from the set at the last "place", we've moved to a
// new place. Every device counts the distinct places it was heard in; one heard in
// enough places, over enough time, is moving with us. (GPS, when a board has it, can
// call notePlaceChange() on distance travelled instead.)
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ss_devices.h"

namespace ss {

enum Deployment : uint8_t { DEPLOY_STATIONARY = 0, DEPLOY_MOBILE = 1, DEPLOY_PORTABLE = 2 };
const char* deploymentName(uint8_t d);

struct FollowPolicy {
  uint32_t epoch_ms;          // how often the surroundings are compared
  float max_similarity;       // AP-set overlap at or below this = a new place
  uint8_t min_aps;            // ignore epochs with fewer APs (open country, garage)
  uint8_t places_tracker;     // places before a tracker alert
  uint32_t min_ms_tracker;    // ...and at least this long with us
  uint8_t places_other;       // same for any other device
  uint32_t min_ms_other;
  uint32_t linger_ms;         // stationary: surveillance gear present this long
};
FollowPolicy followPolicy(uint8_t deployment);

struct FollowAlert {
  uint32_t device_id = 0;
  bool lingering = false;     // stationary alert instead of following
  uint8_t places = 0;
  uint32_t with_us_ms = 0;
};

class FollowMonitor {
 public:
  void setDeployment(uint8_t d);
  uint8_t deployment() const { return mode_; }
  // Call about once a second.
  void update(DeviceTable& table, uint32_t now_ms);
  // External movement signal (GPS): counts as a new place right away.
  void notePlaceChange() { force_ = true; }
  bool poll(FollowAlert& out);
  uint16_t place() const { return place_; }

 private:
  static constexpr size_t kMaxAps = 64;
  static constexpr size_t kMaxAlerts = 8;
  void newPlace(const uint32_t* aps, size_t n);
  bool eligible(const Device& d) const;
  void push(const FollowAlert& a);

  uint8_t mode_ = DEPLOY_STATIONARY;
  FollowPolicy pol_ = followPolicy(DEPLOY_STATIONARY);
  uint32_t epoch_start_ = 0;
  bool started_ = false, force_ = false;
  uint16_t place_ = 0;
  uint32_t anchor_[kMaxAps];
  size_t n_anchor_ = 0;
  FollowAlert q_[kMaxAlerts];
  size_t q_head_ = 0, q_len_ = 0;
};

}  // namespace ss
