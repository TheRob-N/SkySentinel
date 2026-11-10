#include "ss_follow.h"

#include <string.h>

namespace ss {

const char* deploymentName(uint8_t d) {
  return d == DEPLOY_MOBILE ? "mobile" : d == DEPLOY_PORTABLE ? "portable" : "stationary";
}

FollowPolicy followPolicy(uint8_t d) {
  FollowPolicy p;
  if (d == DEPLOY_MOBILE) {        // driving: surroundings change fast
    p = {60000, 0.25f, 3, 3, 10 * 60000u, 5, 20 * 60000u, 0};
  } else if (d == DEPLOY_PORTABLE) {  // walking: slower turnover, more patience
    p = {90000, 0.30f, 3, 3, 15 * 60000u, 5, 30 * 60000u, 0};
  } else {                         // stationary: no places; flag lingering surveillance gear
    p = {60000, 0.0f, 3, 0, 0, 0, 0, 20 * 60000u};
  }
  return p;
}

void FollowMonitor::setDeployment(uint8_t d) {
  mode_ = d > (uint8_t)DEPLOY_PORTABLE ? (uint8_t)DEPLOY_STATIONARY : d;
  pol_ = followPolicy(mode_);
  started_ = false;
  n_anchor_ = 0;
}

static bool isAp(const Device& d) {
  return d.radio == 1 && (d.kind == KIND_WIFI_BEACON || d.kind == KIND_WIFI_PROBE_RESP);
}

bool FollowMonitor::eligible(const Device& d) const {
  // Weak surveillance matches (an OUI shared with ordinary gear, conf < 40) never follow
  // or linger: at home they'd fire on your own Raspberry Pi or laptop after 20 minutes.
  // (Phones and other everyday devices are low-confidence by nature and still count.)
  const bool weakSurv = (kSurveillanceMask & catBit(d.category)) && d.conf < 40;
  // Sub-GHz rows are a whole band, not one device: they'd "follow" every drive.
  return !isAp(d) && !d.spoofed && !d.hidden && !d.alias_of && !weakSurv && d.category != CAT_SUBGHZ;
}

void FollowMonitor::push(const FollowAlert& a) {
  if (q_len_ == kMaxAlerts) { q_head_ = (q_head_ + 1) % kMaxAlerts; --q_len_; }  // drop oldest
  q_[(q_head_ + q_len_) % kMaxAlerts] = a;
  ++q_len_;
}

bool FollowMonitor::poll(FollowAlert& out) {
  if (!q_len_) return false;
  out = q_[q_head_];
  q_head_ = (q_head_ + 1) % kMaxAlerts;
  --q_len_;
  return true;
}

void FollowMonitor::newPlace(const uint32_t* aps, size_t n) {
  ++place_;
  if (place_ == 0) place_ = 1;
  n_anchor_ = n > kMaxAps ? kMaxAps : n;
  memcpy(anchor_, aps, n_anchor_ * sizeof(uint32_t));
}

void FollowMonitor::update(DeviceTable& t, uint32_t now) {
  const bool moving = mode_ != DEPLOY_STATIONARY;

  // 1. Has the place changed? (once per epoch)
  if (moving) {
    if (!started_) { started_ = true; epoch_start_ = now; if (!place_) place_ = 1; }
    if (force_ || (int32_t)(now - epoch_start_) >= (int32_t)pol_.epoch_ms) {
      uint32_t aps[kMaxAps];
      size_t n = 0;
      t.forEach([&](const Device& d) {
        // Only what was heard during this epoch, so consecutive snapshots don't overlap.
        if (n < kMaxAps && isAp(d) && !d.alias_of && (int32_t)(d.last_ms - epoch_start_) >= 0) aps[n++] = d.id;
      });
      if (force_) {
        newPlace(aps, n);
      } else if (n >= pol_.min_aps) {
        if (!n_anchor_) {
          // First look around: remember it as the current place (not a move).
          n_anchor_ = n;
          memcpy(anchor_, aps, n * sizeof(uint32_t));
        } else {
          size_t common = 0;
          for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n_anchor_; ++j)
              if (aps[i] == anchor_[j]) { ++common; break; }
          const size_t uni = n + n_anchor_ - common;
          const float sim = uni ? (float)common / (float)uni : 1.0f;
          if (sim <= pol_.max_similarity) newPlace(aps, n);
        }
      }
      force_ = false;
      epoch_start_ = now;
    }
  }

  // 2. Per device: count places, raise alerts.
  t.forEachMutable([&](Device& d) {
    if (!eligible(d) || now - d.last_ms > 60000) return;
    const uint32_t with = now - d.first_ms;
    if (!moving) {
      // Stationary: surveillance gear (or a tracker) hanging around for a long time.
      const bool surv = (kSurveillanceMask & catBit(d.category)) != 0;
      if (surv && !d.follow_alerted && pol_.linger_ms && with >= pol_.linger_ms) {
        d.follow_alerted = true;
        FollowAlert a;
        a.device_id = d.id; a.lingering = true; a.with_us_ms = with;
        push(a);
      }
      return;
    }
    if (d.last_place != place_) {
      d.last_place = place_;
      if (d.places < 255) ++d.places;
    }
    if (d.follow_alerted) return;
    const bool tracker = d.category == CAT_TRACKER;
    const uint8_t needPlaces = tracker ? pol_.places_tracker : pol_.places_other;
    const uint32_t needMs = tracker ? pol_.min_ms_tracker : pol_.min_ms_other;
    if (d.places >= needPlaces && with >= needMs) {
      d.follow_alerted = true;
      FollowAlert a;
      a.device_id = d.id; a.places = d.places; a.with_us_ms = with;
      push(a);
    }
  });
}

}  // namespace ss
