// CYD touch dashboard: the "Cheap Yellow Display" (ESP32-2432S028) and LCDWiki's
// 2.8" ESP32-32E board, as a Classic Bluetooth node with a 320x240 touch screen.
// Built only with -DSS_CYD (env:cyd).
//
// First boot walks through: tap three targets (touch calibration), pick the board,
// check the colors. After that it shows the master's dashboard: latest alert,
// category tiles (tap to switch a category on/off for the whole cluster), the
// closest surveillance devices, and how the node itself is doing.
//
// If the screen stays blank or garbled: hold BOOT for 3 s to try the other display
// chip (ILI9341 / ST7789); the board reboots with it.
#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "ss_proto.h"

namespace ss {
namespace cyd {

// What the node itself is doing, for the header, footer and "no master" screen.
struct NodeState {
  uint16_t my_id = 0;
  bool paired = false;
  uint16_t master_id = 0;
  int8_t link_rssi = -127;
  bool configured = false;
  uint32_t classic_found = 0;
  uint32_t inquiries = 0;
  uint32_t wifi_frames = 0;
  uint8_t wifi_channels = 0;
  bool classic_on = false;
  const char* label = "";
  bool identify = false;        // master asked "which one is this?"
  bool screen_off = false;      // master turned node screens off (a tap wakes it for 30 s)
  bool unpaired = false;        // no cluster key: show how to pair
  bool pair_offer = false;      // a master offered: show the code
  uint32_t pair_code = 0;
  uint16_t pair_master = 0;
  uint8_t gps = 0;              // GpsState of this CYD's own GPS (0 = none)
  uint8_t gps_sats = 0;
  uint8_t sd = 0;               // SdState: the SD card log
  uint32_t sd_saved = 0;
};

void begin(Preferences* prefs);
// One line on the boot screen while things start: "SD card ..... logging".
// ok: 1 = fine (green), 0 = missing / needs attention (amber), 2 = just information.
void bootStep(const char* what, const char* result, uint8_t ok);
// Call often (every loop). Reads touch, redraws what changed, runs the BOOT-button escape.
void loop(const NodeState& s, uint32_t now_ms);
void onDash(const Dash& d, uint32_t now_ms);
// Data for the screen that's open (DEVICES, a device, EVENTS, CLUSTER): MSG_DASH_VIEW payload.
void onView(const uint8_t* p, size_t n, uint32_t now_ms);
// A tap that the master should act on (category toggle, deployment).
bool takeCommand(DashCommand& out);
bool pendingCommand();
// SETTINGS > Pair with a master (confirmed): the node should forget its master.
bool wantsUnpair();
// Serial: "cyd setup" | "cyd driver auto|ili9341|st7789" | "cyd invert on|off|auto"
bool handleCommand(const String& line);
// Battery on the ESP32-32E (255 = none, 254 = probably on USB). Other boards: 255.
uint8_t batteryPercent();
// Short description for the status JSON: "ili9341" / "st7789", board, setup state.
void describe(Print& out);

}  // namespace cyd
}  // namespace ss
