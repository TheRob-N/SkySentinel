// Status screen for boards that have one: Heltec Wireless Tracker (0.96" ST7735S, 160x80,
// -DSS_DISPLAY_ST7735) and LilyGO T-Embed CC1101 (1.9" ST7789, 320x170, jog wheel,
// -DSS_DISPLAY_TEMBED). The master can turn node screens off
// (CONFIG node_flags), which also turns the backlight off.
#pragma once

#include <Arduino.h>

#include "ss_proto.h"

namespace ss {
namespace display {

struct Info {
  const char* role = "";          // "NODE", "MASTER", "STANDALONE"
  bool paired = false;            // node: talking to a master
  uint16_t master_id = 0;
  int8_t link_rssi = -127;        // node: how well we hear the master
  bool configured = false;        // node: has the master's orders
  uint8_t ble_duty = 0;
  uint8_t wifi_channels = 0;      // 0 = Wi-Fi off / home channel only
  bool lora = false;
  const char* lora_on = "";
  uint32_t lora_packets = 0;
  uint32_t sent = 0;              // node: sightings reported; others: devices now
  uint32_t session = 0;           // master / standalone: devices this session
  uint8_t nodes_online = 0;       // master
  uint8_t battery_pct = 255;      // 255 = no battery / unknown
  bool usb = false;               // plugged into a computer: shows "USB" instead of a battery %
  uint8_t gps = 0;                // GpsState (ss_gps.h), 0 = no GPS
  uint8_t gps_sats = 0;
  uint32_t uptime_s = 0;
  const char* label = "";         // the name given on the master ("Truck rear")
  bool identify = false;          // master asked "which one is this?": fill the screen
  uint8_t pair = 0;               // 1 unpaired, waiting; 2 a master offered (show the code); 3 master: pairing open
  uint32_t pair_code = 0;
  uint16_t pair_master = 0;
};

bool begin();
void setOn(bool on);
bool isOn();
// Redraws what changed. Call about once a second.
void update(const Info& info);

// Battery on the Heltec Wireless Tracker V1.1 (divider on GPIO1, enabled by GPIO2).
// 255 when running on USB alone.
uint8_t batteryPercent();

#ifdef SS_DISPLAY_TEMBED
// Jog wheel and buttons; call every loop. Turning flips pages, the BACK key turns the
// screen off and on.
void tick(uint32_t now_ms);
// The ALERTS page: the master's summary (MSG_DASH) and recent events (MSG_DASH_VIEW).
void onDash(const Dash& d);
void onEvents(const DashEvents& e);
// HUNT: a Bluetooth sighting this board made itself (any task; quick).
void onSighting(const Sighting& s, const char* name);
// HUNT on a Bluetooth device is running: Bluetooth should listen full-time.
bool huntBle();
// Turns the picture around (wheel on the left or the right); saved on the board.
void flip();
#endif

}  // namespace display
}  // namespace ss
