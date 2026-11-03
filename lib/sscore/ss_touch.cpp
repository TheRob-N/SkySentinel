#include "ss_touch.h"

#include <stdlib.h>

namespace ss {

static constexpr int kMinSpan = 300;  // raw counts between targets; real panels give ~2500+

bool touchCalibrate(const int16_t raw[3][2], TouchCal& out) {
  // Top-left -> top-right moves along screen X; top-left -> bottom-left along screen Y.
  const int dxx = abs(raw[1][0] - raw[0][0]), dxy = abs(raw[1][1] - raw[0][1]);
  const int dyx = abs(raw[2][0] - raw[0][0]), dyy = abs(raw[2][1] - raw[0][1]);
  TouchCal c;
  c.swap = dxy > dxx ? 1 : 0;
  const int ax = c.swap ? 1 : 0, by = c.swap ? 0 : 1;
  // The axis that moves along X must not also be the one that moves along Y.
  if ((c.swap ? dxy : dxx) < kMinSpan || (c.swap ? dyx : dyy) < kMinSpan) return false;
  c.a0 = raw[0][ax];
  c.a1 = raw[1][ax];
  c.b0 = raw[0][by];
  c.b1 = raw[2][by];
  if (!c.valid()) return false;
  out = c;
  return true;
}

void touchMap(const TouchCal& c, const TouchTargets& t, int16_t rx, int16_t ry, uint16_t w, uint16_t h,
              int16_t& sx, int16_t& sy) {
  const int a = c.swap ? ry : rx, b = c.swap ? rx : ry;
  long x = t.left + (long)(a - c.a0) * (t.right - t.left) / (c.a1 - c.a0);
  long y = t.top + (long)(b - c.b0) * (t.bottom - t.top) / (c.b1 - c.b0);
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= w) x = w - 1;
  if (y >= h) y = h - 1;
  sx = (int16_t)x;
  sy = (int16_t)y;
}

}  // namespace ss
