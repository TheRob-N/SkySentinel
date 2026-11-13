#include "ss_linkchan.h"

#include <math.h>

namespace ss {

const uint8_t kLinkChannels[kLinkCandidates] = {1, 6, 11};

int linkChannelIndex(uint8_t ch) {
  for (int i = 0; i < kLinkCandidates; ++i)
    if (kLinkChannels[i] == ch) return i;
  return -1;
}

float linkChannelCost(uint8_t link_ch, const ChanHeard& h) {
  if (h.channel < 1 || h.channel > 13 || h.rssi <= -127) return 0;
  const int d = h.channel > link_ch ? h.channel - link_ch : link_ch - h.channel;
  if (d > 4) return 0;   // 5+ channels apart: no overlap
  int r = h.rssi < -95 ? -95 : (h.rssi > -20 ? -20 : h.rssi);
  // Strength: -95 dBm counts 1, every 30 dB stronger counts 10x (-65 = 10, -50 = 32).
  const float e = powf(10.0f, (r + 95) / 30.0f);
  static const float kOverlap[5] = {0, 1.0f, 0.8f, 0.5f, 0.2f};   // by channels apart
  if (h.ap) {
    if (d == 0) {   // same channel: takes turns; costs what it's busy for
      const float busy = h.util == 255 ? 0.15f : h.util / 255.0f;
      return e * (0.25f + busy);
    }
    return e * kOverlap[d];
  }
  return e * (d == 0 ? 0.15f : 0.3f * kOverlap[d]);   // a device's own frames: smaller
}

void LinkSurvey::add(const ChanHeard& h) {
  bool any = false;
  for (int i = 0; i < kLinkCandidates; ++i) {
    const float c = linkChannelCost(kLinkChannels[i], h);
    cost[i] += c;
    any = any || c > 0;
  }
  if (any) ++heard;
}

uint8_t LinkSurvey::best() const {
  int b = 1;   // ties go to 6, the default
  for (int i = 0; i < kLinkCandidates; ++i)
    if (cost[i] < cost[b] - 0.01f) b = i;
  return kLinkChannels[b];
}

uint8_t linkChannelPick(uint8_t current, const LinkSurvey& s, uint16_t loss_permille, const LinkMovePolicy& p) {
  const int ci = linkChannelIndex(current);
  if (ci < 0) return kLinkChannelDefault;   // somehow off the list: go back to the default
  if (!s.heard) return 0;                   // nothing heard yet: no reason to move
  float cur = s.cost[ci];
  if (loss_permille >= p.lossy_permille) cur *= 1.5f;
  const uint8_t b = s.best();
  const float bc = s.cost[linkChannelIndex(b)];
  if (b == current) return 0;
  if (bc <= cur * p.ratio && cur - bc >= p.min_gain) return b;
  return 0;
}

void LinkChooser::survey(const LinkSurvey& s) {
  heard_ = s.heard;
  for (int i = 0; i < kLinkCandidates; ++i)
    avg_[i] = seeded_ ? avg_[i] + (s.cost[i] - avg_[i]) / kAvgSamples : s.cost[i];
  seeded_ = true;
}

void LinkChooser::noteLoss(uint8_t ch, uint16_t permille) {
  const int i = linkChannelIndex(ch);
  if (i < 0) return;
  loss_[i] = loss_[i] < 0 ? (int16_t)permille : (int16_t)((loss_[i] * 3 + permille) / 4);
}

int16_t LinkChooser::measuredLoss(uint8_t ch) const {
  const int i = linkChannelIndex(ch);
  return i < 0 ? -1 : loss_[i];
}

uint8_t LinkChooser::decide(uint8_t current, uint16_t loss_now, bool force) {
  vetoed_ = 0;
  if (!seeded_) return 0;
  LinkSurvey s;
  for (int i = 0; i < kLinkCandidates; ++i) s.cost[i] = avg_[i];
  s.heard = heard_ ? heard_ : 1;
  const uint8_t to = linkChannelPick(current, s, loss_now, policy);
  if (!to) { streak_ = 0; return 0; }
  const int16_t was = measuredLoss(to);
  if (was >= 0 && was > (int16_t)loss_now + 5) {   // been there, and the link did worse
    vetoed_ = to;
    streak_ = 0;
    return 0;
  }
  if (force) { streak_ = 0; return to; }
  if (to == streak_ch_ && streak_) ++streak_;
  else { streak_ch_ = to; streak_ = 1; }
  if (streak_ < kStreak) return 0;
  streak_ = 0;
  return to;
}

uint8_t linkSearchChannel(uint8_t saved, uint32_t step) {
  if (!linkChannelValid(saved)) saved = kLinkChannelDefault;
  uint8_t order[kLinkCandidates] = {saved, 0, 0};
  int n = 1;
  for (int i = 0; i < kLinkCandidates; ++i)
    if (kLinkChannels[i] != saved) order[n++] = kLinkChannels[i];
  return order[step % kLinkCandidates];
}

}  // namespace ss
