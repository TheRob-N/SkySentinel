// Resistive touch calibration (CYD's XPT2046): three taps at known screen points turn
// raw 12-bit readings into screen pixels, whatever way the panel is wired (axes swapped
// or reversed). Pure C++, host tested.
#pragma once

#include <stdint.h>

namespace ss {

struct TouchCal {
  uint8_t version = 1;
  uint8_t swap = 0;          // raw X runs along the screen's Y
  int16_t a0 = 0, a1 = 0;    // raw value (along screen X) at the left and right targets
  int16_t b0 = 0, b1 = 0;    // raw value (along screen Y) at the top and bottom targets
  bool valid() const { return version == 1 && a0 != a1 && b0 != b1; }
};

// Where the three targets are drawn: top-left, top-right, bottom-left.
struct TouchTargets {
  int16_t left, right, top, bottom;
};

// raw[i] = {raw x, raw y} measured at the three targets. False if the taps don't
// make sense (too close together).
bool touchCalibrate(const int16_t raw[3][2], TouchCal& out);
// Raw reading -> screen pixel, clamped to w x h.
void touchMap(const TouchCal& c, const TouchTargets& t, int16_t rx, int16_t ry, uint16_t w, uint16_t h,
              int16_t& sx, int16_t& sy);

}  // namespace ss
