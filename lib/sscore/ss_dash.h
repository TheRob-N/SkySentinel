// The dashboard summary a master sends to a touch-screen node (CYD): counts per
// category, the closest surveillance devices, and the latest alert. Pure C++, host tested.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ss_devices.h"
#include "ss_proto.h"

namespace ss {

// How loud a device is: 0 quiet, 1 notice, 2 alert. Same rules as the web page:
// a Find My tag away from its owner, a strong ALPR or body-cam match = alert;
// anything else in a surveillance category at 40%+ = notice.
uint8_t alertLevel(const Device& d);

// What to call a device on a small screen: its name, else what it is.
void dashLabel(const Device& d, char* out, size_t cap);

// Fills counts, devices_now and the closest surveillance devices (following first,
// then loudest, then strongest signal). Leaves deploy, nodes and the alert alone.
void buildDash(const DeviceTable& table, uint32_t now_ms, uint16_t cats, Dash& out);

// Devices screen: the same list as the web page (not hidden, grouped or spoofed, heard
// in the last minute, category switched on), best first: confidence, then most recent.
// Fills rows [offset, offset + kDashRows).
void buildDeviceView(const DeviceTable& table, uint32_t now_ms, uint16_t cats, uint16_t offset, DashDevices& out);
// One device by id (found = false if it's gone).
void buildDetail(DeviceTable& table, uint32_t id, uint32_t now_ms, DashDetail& out);

// The latest alert, for the banner, plus a history of everything (alerts, notices,
// node joins and losses) for the Events screen. A notice doesn't push out an alert
// less than a minute old from the banner; it still goes in the history.
class DashAlerts {
 public:
  static constexpr uint8_t kHistory = 30;
  void raise(uint8_t level, uint8_t category, const char* text, uint32_t now_ms);
  void note(uint8_t level, uint8_t category, const char* text, uint32_t now_ms);  // history only
  void fill(DashAlert& out, uint32_t now_ms) const;
  void page(uint16_t offset, uint32_t now_ms, DashEvents& out) const;  // newest first
  uint16_t count() const { return n_; }

 private:
  DashAlert a_;
  uint32_t ms_ = 0;
  struct Item { uint8_t level, category; uint32_t ms; char text[kDashAlertText + 1]; };
  Item hist_[kHistory];
  uint8_t head_ = 0;   // next slot to write
  uint8_t n_ = 0;
};

}  // namespace ss
