// Node-side aggregation: collapses repeated hits from the same address into one
// sighting per report interval, so a busy channel costs a few ESP-NOW frames
// instead of hundreds.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ss_proto.h"

namespace ss {

template <size_t Capacity>
class SightingBatch {
 public:
  // Returns false (and counts a drop) when the batch is full and the address is new.
  bool add(const Sighting& s, uint32_t now_ms) {
    const uint8_t rc = radioClass(s.kind);
    for (size_t i = 0; i < n_; ++i) {
      Entry& e = e_[i];
      if (radioClass(e.s.kind) == rc && memcmp(e.s.addr, s.addr, 6) == 0) {
        if (e.s.hits < 255) ++e.s.hits;
        if (s.rssi_max > e.s.rssi_max) e.s.rssi_max = s.rssi_max;
        e.s.rssi_last = s.rssi_last;
        e.s.channel = s.channel;
        e.s.flags |= s.flags;
        if (s.conf > e.s.conf) {  // keep the most specific match seen this interval
          e.s.category = s.category; e.s.sig_id = s.sig_id; e.s.conf = s.conf;
        }
        if (!e.s.fp) e.s.fp = s.fp;
        if (e.s.company == 0xFFFF) e.s.company = s.company;
        e.last_ms = now_ms;
        return true;
      }
    }
    if (n_ >= Capacity) { if (dropped_ < 0xFFFF) ++dropped_; return false; }
    e_[n_].s = s;
    e_[n_].last_ms = now_ms;
    ++n_;
    return true;
  }

  size_t size() const { return n_; }
  bool full() const { return n_ >= Capacity; }

  // Moves up to `max` sightings into `out` (highest confidence first), filling age_ms.
  size_t take(Sighting* out, size_t max, uint32_t now_ms) {
    size_t k = 0;
    while (k < max && n_ > 0) {
      size_t best = 0;
      for (size_t i = 1; i < n_; ++i)
        if (e_[i].s.conf > e_[best].s.conf) best = i;
      out[k] = e_[best].s;
      const uint32_t age = now_ms - e_[best].last_ms;
      out[k].age_ms = age > 0xFFFF ? 0xFFFF : (uint16_t)age;
      ++k;
      e_[best] = e_[--n_];
    }
    return k;
  }

  uint16_t takeDropped() { const uint16_t d = dropped_; dropped_ = 0; return d; }
  void clear() { n_ = 0; }

 private:
  struct Entry { Sighting s; uint32_t last_ms; };
  Entry e_[Capacity];
  size_t n_ = 0;
  uint16_t dropped_ = 0;
};

// Remembers which addresses already had their name sent (re-sends after `ttl_ms`).
template <size_t Capacity>
class NameTracker {
 public:
  explicit NameTracker(uint32_t ttl_ms = 300000) : ttl_(ttl_ms) {}
  bool shouldSend(const uint8_t addr[6], uint32_t now_ms) {
    size_t oldest = 0;
    for (size_t i = 0; i < Capacity; ++i) {
      if (used_[i] && memcmp(addr_[i], addr, 6) == 0) {
        if (now_ms - sent_[i] < ttl_) return false;
        sent_[i] = now_ms;
        return true;
      }
      if (!used_[i]) { oldest = i; break; }
      if (sent_[i] < sent_[oldest]) oldest = i;
    }
    used_[oldest] = true;
    memcpy(addr_[oldest], addr, 6);
    sent_[oldest] = now_ms;
    return true;
  }

 private:
  uint32_t ttl_;
  uint8_t addr_[Capacity][6] = {};
  uint32_t sent_[Capacity] = {};
  bool used_[Capacity] = {};
};

}  // namespace ss
