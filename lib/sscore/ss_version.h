// The SkySentinel release this firmware belongs to (shown on screens and the page).
#pragma once
#define SS_RELEASE "2.1.3"

#include <stdint.h>

// When this firmware was built, YYMMDDHHMM local time (2611110843 = 2026-11-11 08:43).
// Written by tools/build_stamp.py; 0 in builds without it (host tests).
extern const uint32_t ssBuildStamp;

// SS_RELEASE as a number for the heartbeat: major << 10 | minor << 5 | patch.
constexpr uint16_t ssReleaseCode(const char* s = SS_RELEASE, int part = 0, uint16_t acc = 0, uint16_t cur = 0) {
  return (*s >= '0' && *s <= '9') ? ssReleaseCode(s + 1, part, acc, (uint16_t)(cur * 10 + (*s - '0')))
         : (*s == '.')            ? ssReleaseCode(s + 1, part + 1, (uint16_t)((acc << 5) | (cur & 31)), 0)
                                  : (uint16_t)(((acc << 5) | (cur & 31)) << (5 * (2 - part)));
}
