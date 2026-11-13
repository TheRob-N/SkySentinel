// Link channel: which 2.4 GHz channel the cluster talks on (ESP-NOW and the master's
// Wi-Fi page). The master surveys what the nodes hear and, in automatic mode, moves the
// cluster off a crowded channel. Only 1, 6 and 11 are considered: they don't overlap.
//
// What hurts the link most is a strong network on an OVERLAPPING channel (6 vs 7, say):
// the radio can't decode it, so it doesn't take turns with it. It just sees the channel
// as busy (frames wait, and get stuck) or loses frames to it. A network on the SAME
// channel is much kinder: both sides decode each other and take turns, so it costs
// only the airtime it actually uses.
#pragma once

#include <stdint.h>

namespace ss {

constexpr uint8_t kLinkChannelDefault = 6;
constexpr uint8_t kLinkCandidates = 3;
extern const uint8_t kLinkChannels[kLinkCandidates];   // 1, 6, 11

inline bool linkChannelValid(uint8_t ch) { return ch == 1 || ch == 6 || ch == 11; }
int linkChannelIndex(uint8_t ch);   // -1 if not 1 / 6 / 11

// One thing heard on 2.4 GHz: a router (AP), or another Wi-Fi device sending frames.
struct ChanHeard {
  uint8_t channel = 0;   // 1..13
  int8_t rssi = -127;    // as heard by the cluster
  bool ap = false;
  uint8_t util = 255;    // AP's own channel-busy report, 0..255 (255 = not reported)
};

// How much `heard` would disturb a link on `link_ch` (0 = not at all). Unitless.
float linkChannelCost(uint8_t link_ch, const ChanHeard& heard);

struct LinkSurvey {
  float cost[kLinkCandidates] = {0, 0, 0};   // for 1, 6, 11
  uint16_t heard = 0;                        // things counted
  uint8_t best() const;                      // channel with the lowest cost
  void add(const ChanHeard& h);
};

// Automatic mode: should the cluster move from `current`? Returns the channel to move
// to, or 0 to stay. Moves only when another channel is clearly better (hysteresis, so
// two similar channels don't bounce), and `loss_permille` (the link's loss lately) on
// the current channel makes it readier to move.
struct LinkMovePolicy {
  float ratio = 0.6f;         // the new channel must cost at most this share of the current one
  float min_gain = 10.0f;     // ... and at least this much less
  float lossy_permille = 30;  // link losing 3%+ lately: count the current channel 1.5x
};
uint8_t linkChannelPick(uint8_t current, const LinkSurvey& s, uint16_t loss_permille,
                        const LinkMovePolicy& p = LinkMovePolicy());

// The master's automatic choice, steadied (a 15-minute survey swung 32 -> 130 for one
// channel in a day, and the cluster flipped 11 <-> 6 every 6 hours):
//  - surveys (one a minute) are averaged over about an hour;
//  - a better channel has to win `kStreak` decisions in a row (10 min apart);
//  - the link's measured loss on each channel it has been on is remembered, and it
//    won't move back to a channel where the link did worse than it does now.
class LinkChooser {
 public:
  static constexpr uint8_t kStreak = 3;
  static constexpr float kAvgSamples = 60;   // one-a-minute surveys: about an hour
  void survey(const LinkSurvey& s);
  // The link's loss on `ch` over the last 10 minutes (tenths of a percent)
  void noteLoss(uint8_t ch, uint16_t permille);
  // Every 10 minutes. `force` (SURVEY NOW): no streak needed. Returns the channel to
  // move to, or 0 to stay.
  uint8_t decide(uint8_t current, uint16_t loss_now, bool force = false);
  const float* avgCost() const { return avg_; }
  int16_t measuredLoss(uint8_t ch) const;   // tenths of a percent, -1 = never measured
  uint8_t streakChannel() const { return streak_ ? streak_ch_ : 0; }
  uint8_t streak() const { return streak_; }
  uint8_t vetoed() const { return vetoed_; }  // last decision: the channel turned down for its record
  LinkMovePolicy policy;
 private:
  float avg_[kLinkCandidates] = {0, 0, 0};
  uint16_t heard_ = 0;
  bool seeded_ = false;
  int16_t loss_[kLinkCandidates] = {-1, -1, -1};
  uint8_t streak_ch_ = 0, streak_ = 0, vetoed_ = 0;
};

// A node that lost the master looks for it on each link channel in turn, starting
// with the last one it knew: step 0, 1, 2 -> saved first, then the other two.
uint8_t linkSearchChannel(uint8_t saved, uint32_t step);

}  // namespace ss
